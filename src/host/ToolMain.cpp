// `--tool` entry point. See ToolMain.h for the grammar and exit codes.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <crtdbg.h>
#include "ToolMain.h"
#include "ToolIO.h"
#include "ToolPatch.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "effect/ParticleSystemIO.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "StringConv.h"

namespace host
{
namespace
{
const wchar_t* kUsage =
    L"usage: ParticleEditor.exe --tool inspect  <in.alo> --out <inspect.json>\n"
    L"       ParticleEditor.exe --tool validate <in.alo> --out <report.json>\n"
    L"       ParticleEditor.exe --tool patch    <in.alo> <patch.json> --out <candidate.alo> --report <report.json>\n";

std::vector<std::wstring> CommandLineArgs()
{
    std::vector<std::wstring> args;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv) return args;
    for (int i = 0; i < count; i++) args.push_back(argv[i]);
    LocalFree(argv);
    return args;
}

// Reads the whole source. Returns 0, or kExitSource with the reason on stderr.
int ReadSource(const std::wstring& path, std::string* bytes)
{
    std::string error;
    if (ReadFileBytes(path, bytes, &error)) return kExitOk;
    fwprintf(stderr, L"[tool] cannot read %ls: ", path.c_str());
    fprintf(stderr, "%s\n", error.c_str());
    return kExitSource;
}

int Inspect(const std::wstring& input, const std::wstring& output)
{
    if (SameFile(input, output))
    {
        fwprintf(stderr, L"[tool] --out names the input file\n");
        return kExitUsage;
    }
    std::string bytes;
    if (int code = ReadSource(input, &bytes)) return code;
    std::string error;
    std::unique_ptr<ParticleSystem> system = ParseSource(bytes, &error);
    if (!system)
    {
        fprintf(stderr, "[tool] the source did not load: %s\n", error.c_str());
        return kExitSource;
    }
    nlohmann::json dump = particle_json::ParticleSystemJson(*system);
    std::string path, reason;
    if (particle_json::FindInvalidInspectValue(dump, &path, &reason))
    {
        fprintf(stderr, "[tool] the source holds a value inspect cannot report: %s %s\n", path.c_str(), reason.c_str());
        return kExitSource;
    }
    const std::string hash = Sha256Hex(bytes);
    if (hash.empty()) return kExitInternal;
    dump["sourceDocumentHash"] = hash;
    if (!WriteFileAtomically(output, dump.dump(2) + "\n"))
    {
        fwprintf(stderr, L"[tool] cannot write %ls (error %lu)\n", output.c_str(), GetLastError());
        return kExitIo;
    }
    fprintf(stderr, "[tool] verb=inspect exit=0 emitters=%zu sourceDocumentHash=%s\n",
            system->getEmitters().size(), hash.c_str());
    return kExitOk;
}

nlohmann::json Finding(const char* severity, const char* code, const std::string& message, const std::string& path = "")
{
    nlohmann::json finding = {{"severity", severity}, {"code", code}, {"message", message}};
    if (!path.empty()) finding["path"] = path;
    return finding;
}

// Valid means the source loads exactly as written: no load failure, no spawn
// link the loader had to clear, and every value reportable by inspect.
// `canonical` says whether a save of the loaded document reproduces the
// source bytes. Warnings and advisories never make a source invalid.
int Validate(const std::wstring& input, const std::wstring& output)
{
    if (SameFile(input, output))
    {
        fwprintf(stderr, L"[tool] --out names the input file\n");
        return kExitUsage;
    }
    nlohmann::json findings = nlohmann::json::array();
    nlohmann::json report = {
        {"schema", 1}, {"verb", "validate"}, {"sourceDocumentHash", nullptr}, {"internalName", nullptr},
        {"derivedName", DeriveParticleSystemName(input)}, {"canonical", nullptr}, {"textureResolution", "unverified"},
    };
    std::string bytes;
    std::unique_ptr<ParticleSystem> system;
    if (ReadSource(input, &bytes) != kExitOk)
        findings.push_back(Finding("error", "read-failed", "the source could not be read"));
    else
    {
        const std::string hash = Sha256Hex(bytes);
        if (hash.empty()) return kExitInternal;
        report["sourceDocumentHash"] = hash;
        std::string error;
        system = ParseSource(bytes, &error);
        if (!system) findings.push_back(Finding("error", "load-failed", error));
    }
    if (system)
    {
        report["internalName"] = system->getName();
        for (const std::string& repair : system->getGraphRepairs())
            findings.push_back(Finding("error", "graph-repaired", repair));
        std::string path, reason;
        if (particle_json::FindInvalidInspectValue(particle_json::ParticleSystemJson(*system), &path, &reason))
            findings.push_back(Finding("error", "invalid-value", reason, path));
        if (system->getName() != report["derivedName"].get<std::string>())
            findings.push_back(Finding("warning", "internal-name",
                "the internal name '" + system->getName() + "' differs from '" + report["derivedName"].get<std::string>() +
                "', the name the file name gives; the game registers the effect under the internal name"));
        const std::vector<ParticleSystem::Emitter*>& emitters = system->getEmitters();
        for (size_t i = 0; i < emitters.size(); i++)
        {
            size_t k = 0;
            for (const auto& key : emitters[i]->tracks[ParticleSystem::TRACK_INDEX]->keys)
            {
                const EditResult atlas = ClassifyAtlasIndex(*emitters[i], key.value);
                if (atlas.outcome == EditOutcome::OutOfGrid)
                    findings.push_back(Finding("advisory", "atlas-out-of-grid", atlas.reason,
                        "emitters[" + std::to_string(i) + "].tracks[" + std::to_string(ParticleSystem::TRACK_INDEX) +
                        "].keys[" + std::to_string(k) + "]"));
                k++;
            }
        }
        report["canonical"] = SaveToBytes(*system) == bytes;
    }
    bool valid = true;
    for (const nlohmann::json& finding : findings) valid = valid && finding["severity"] != "error";
    const int code = valid ? kExitOk : kExitSource;
    report["valid"] = valid;
    report["exitCode"] = code;
    report["findings"] = findings;
    // A name that is not UTF-8 is already an error finding; replace its bytes
    // here so the report itself is always valid JSON.
    if (!WriteFileAtomically(output, report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n"))
    {
        fwprintf(stderr, L"[tool] cannot write %ls (error %lu)\n", output.c_str(), GetLastError());
        return kExitIo;
    }
    fprintf(stderr, "[tool] verb=validate exit=%d valid=%s findings=%zu\n", code, valid ? "true" : "false", findings.size());
    return code;
}

// The four paths must name four different files. The report is written for
// every outcome once they do.
int Patch(const std::wstring& input, const std::wstring& request, const std::wstring& output, const std::wstring& reportPath)
{
    const std::wstring* paths[] = {&input, &request, &output, &reportPath};
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (SameFile(*paths[i], *paths[j]))
            {
                fwprintf(stderr, L"[tool] %ls and %ls name the same file\n", paths[i]->c_str(), paths[j]->c_str());
                return kExitUsage;
            }
    nlohmann::json report;
    int code = kExitOk;
    std::string source, requestBytes, error;
    if (!ReadFileBytes(input, &source, &error))
    {
        code = kExitSource;
        report = {{"schema", 1}, {"verb", "patch"}, {"outcome", "refused"}, {"exitCode", code},
                  {"refusal", {{"category", "source-invalid"}, {"reason", "the source could not be read: " + error}, {"op", nullptr}}}};
    }
    else if (!ReadFileBytes(request, &requestBytes, &error))
    {
        code = kExitUsage;
        report = {{"schema", 1}, {"verb", "patch"}, {"outcome", "refused"}, {"exitCode", code},
                  {"refusal", {{"category", "request-malformed"}, {"reason", "the request could not be read: " + error}, {"op", nullptr}}}};
    }
    else if (const std::string hash = Sha256Hex(source); hash.empty())
    {
        code = kExitInternal;
        report = {{"schema", 1}, {"verb", "patch"}, {"outcome", "refused"}, {"exitCode", code},
                  {"refusal", {{"category", "internal"}, {"reason", "the source could not be hashed"}, {"op", nullptr}}}};
    }
    else
        code = RunPatch(source, hash, requestBytes, input, output, PatchHooks(), &report);
    // A name that is not UTF-8 is refused before it reaches the report;
    // replace any stray bytes so the report is always valid JSON.
    if (!WriteFileAtomically(reportPath, report.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n"))
    {
        fwprintf(stderr, L"[tool] cannot write %ls (error %lu)\n", reportPath.c_str(), GetLastError());
        return kExitIo;
    }
    fprintf(stderr, "[tool] verb=patch exit=%d", code);
    if (report.contains("refusal")) fprintf(stderr, " %s: %s", report["refusal"]["category"].get<std::string>().c_str(),
                                            report["refusal"]["reason"].get<std::string>().c_str());
    fprintf(stderr, "\n");
    return code;
}

int Run(const std::vector<std::wstring>& args)
{
    // args[0] is the exe. The tool takes the whole command line: `--tool`
    // first, then exactly one verb's arguments, so it never combines with a
    // GUI run mode.
    if (args.size() < 3 || args[1] != L"--tool")
    {
        fwprintf(stderr, L"[tool] --tool must be the first argument and cannot be combined with other run modes\n%ls", kUsage);
        return kExitUsage;
    }
    if (args[2] == L"inspect" && args.size() == 6 && args[4] == L"--out" && !args[3].empty() && !args[5].empty())
        return Inspect(args[3], args[5]);
    if (args[2] == L"validate" && args.size() == 6 && args[4] == L"--out" && !args[3].empty() && !args[5].empty())
        return Validate(args[3], args[5]);
    if (args[2] == L"patch" && args.size() == 9 && args[5] == L"--out" && args[7] == L"--report" && !args[3].empty() &&
        !args[4].empty() && !args[6].empty() && !args[8].empty())
        return Patch(args[3], args[4], args[6], args[8]);
    fwprintf(stderr, L"[tool] unrecognised arguments\n%ls", kUsage);
    return kExitUsage;
}
} // namespace

bool IsToolCommandLine()
{
    for (const std::wstring& arg : CommandLineArgs())
        if (arg == L"--tool") return true;
    return false;
}

int RunToolMain()
{
    // No dialog may ever block an unattended run: no critical-error or crash
    // boxes, and in Debug the CRT reports to stderr instead of a modal box.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#ifndef NDEBUG
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    int code = kExitInternal;
    try
    {
        code = Run(CommandLineArgs());
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[tool] internal error: %s\n", e.what());
    }
    catch (...)
    {
        fprintf(stderr, "[tool] internal error\n");
    }
    fflush(stderr);
    return code;
}
} // namespace host
