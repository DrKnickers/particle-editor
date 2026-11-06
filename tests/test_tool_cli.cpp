// `ParticleEditor.exe --tool` end to end: inspect, validate,
// patch and build. Drives the built exe (NEEDS_EXE:
// run-native-unit-tests passes its path as argv[1]) on fixtures this test
// writes, and checks exit codes, the output, the path-alias refusals and that
// nothing else is left on disk. Every run has a deadline: a tool run that
// opened a dialog would hang and fail here.
//
// Exit codes (src/host/ToolMain.h): 0 ok, 2 usage or alias, 3 invalid source,
// 4 selector, 5 rule, 6 postcondition, 13 output not written. validate writes
// its report on exit 3 too; patch writes one for every outcome but a usage or
// alias refusal.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include "effect/ParticleSystem.h"
#include "simulation/ParticleSystemInstance.h"
#include "common/files.h"
#include "host/third_party/nlohmann/json.hpp"

#include <cstdio>
#include <limits>
#include <set>
#include <string>
#include <vector>

// Link stub -- see test_alo_roundtrip.cpp.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int          g_failed = 0;
static std::wstring g_exe;
static std::wstring g_dir;

static void check(bool ok, const char* what)
{
    if (!ok)
    {
        g_failed++;
        printf("  FAIL: %s\n", what);
    }
    else
        printf("  ok: %s\n", what);
}

static std::wstring P(const wchar_t* name)
{
    return g_dir + L"\\" + name;
}

static std::string ReadAll(const std::wstring& path)
{
    std::string bytes;
    HANDLE      h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return bytes;
    char  buffer[65536];
    DWORD got = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got > 0) bytes.append(buffer, got);
    CloseHandle(h);
    return bytes;
}

static void WriteAll(const std::wstring& path, const std::string& bytes)
{
    HANDLE h       = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD  written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
}

static std::string Sha256Hex(const std::string& bytes)
{
    HCRYPTPROV provider   = 0;
    HCRYPTHASH hash       = 0;
    BYTE       digest[32] = {};
    DWORD      size       = sizeof(digest);
    CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0);
    CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE b : digest)
    {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}

static void SaveFixture(ParticleSystem& system, const std::wstring& path)
{
    MemoryFile file;
    system.write(&file);
    std::string bytes(file.size(), '\0');
    file.seek(0);
    file.read(&bytes[0], static_cast<unsigned long>(bytes.size()));
    WriteAll(path, bytes);
}

static void Fixture(const std::wstring& path, void (*edit)(ParticleSystem::Emitter&))
{
    ParticleSystem system;
    system.setName("cli_fixture");
    ParticleSystem::Emitter* e = system.addRootEmitter();
    e->name                    = "Root";
    if (edit) edit(*e);
    SaveFixture(system, path);
}

static std::wstring Quote(const std::wstring& arg)
{
    return L"\"" + arg + L"\"";
}

// Runs the exe with `args` and returns its exit code, or -1 on a timeout.
static int Run(const std::vector<std::wstring>& args)
{
    std::wstring command = Quote(g_exe);
    for (const std::wstring& a : args) command += L" " + Quote(a);
    STARTUPINFOW si         = {sizeof(si)};
    si.dwFlags              = STARTF_USESHOWWINDOW;
    si.wShowWindow          = SW_HIDE;
    PROCESS_INFORMATION  pi = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) return -2;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

static std::set<std::wstring> Listing()
{
    std::set<std::wstring> names;
    WIN32_FIND_DATAW       data;
    HANDLE                 h = FindFirstFileW(P(L"*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
    {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") names.insert(name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return names;
}

static bool Exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static nlohmann::json ReadJson(const std::wstring& path)
{
    try
    {
        return nlohmann::json::parse(ReadAll(path));
    }
    catch (...)
    {
        return nlohmann::json();
    }
}

// The codes of a validate report's findings with `severity`.
static std::set<std::string> Codes(const nlohmann::json& report, const char* severity)
{
    std::set<std::string> codes;
    if (report.is_object() && report["findings"].is_array())
        for (const nlohmann::json& f : report["findings"])
            if (f["severity"] == severity) codes.insert(f["code"].get<std::string>());
    return codes;
}

// check(), printing the report when it fails.
static void checkReport(bool ok, const char* what, const nlohmann::json& report)
{
    check(ok, what);
    if (!ok) printf("    report: %s\n", report.dump().c_str());
}

static void TestValidate(const std::string& goodHash)
{
    printf("validate\n");
    {
        ParticleSystem system;
        system.setName("canon");
        system.addRootEmitter()->name = "Root";
        SaveFixture(system, P(L"canon.alo"));
    }
    check(Run({L"--tool", L"validate", P(L"canon.alo"), L"--out", P(L"canon.report.json")}) == 0,
          "a clean source exits 0");
    nlohmann::json r = ReadJson(P(L"canon.report.json"));
    check(r.is_object() && r["schema"] == 1 && r["verb"] == "validate" && r["valid"] == true && r["exitCode"] == 0,
          "its report says valid, with the exit code");
    check(r.is_object() && r["canonical"] == true && r["findings"].empty() && r["textureResolution"] == "unverified",
          "a file this writer saved is canonical, has no findings, and texture resolution is unverified");

    check(Run({L"--tool", L"validate", P(L"good.alo"), L"--out", P(L"good.report.json")}) == 0,
          "a name warning does not fail validation");
    r = ReadJson(P(L"good.report.json"));
    check(r.is_object() && r["sourceDocumentHash"] == goodHash && r["internalName"] == "cli_fixture" &&
              r["derivedName"] == "good",
          "the report carries the hash, the internal name and the file-derived name");
    check(Codes(r, "warning") == std::set<std::string>({"internal-name"}) && Codes(r, "error").empty(),
          "an internal name that differs from the file name is a warning");

    check(Run({L"--tool", L"validate", P(L"selflink.alo"), L"--out", P(L"selflink.report.json")}) == 3,
          "a repaired graph exits 3");
    r = ReadJson(P(L"selflink.report.json"));
    check(r.is_object() && r["valid"] == false && r["exitCode"] == 3 &&
              Codes(r, "error") == std::set<std::string>({"graph-repaired"}),
          "the report is still written and names the repair");
    check(r.is_object() && r["findings"].size() == 1 &&
              r["findings"][0]["message"].get<std::string>().find("spawnOnDeath") != std::string::npos,
          "the repair message names the cleared slot");

    check(Run({L"--tool", L"validate", P(L"nan.alo"), L"--out", P(L"nan.report.json")}) == 3 &&
              Codes(ReadJson(P(L"nan.report.json")), "error") == std::set<std::string>({"invalid-value"}),
          "a non-finite value exits 3");
    check(Run({L"--tool", L"validate", P(L"inf3f.alo"), L"--out", P(L"inf3f.report.json")}) == 3,
          "an infinite unknown3f exits 3");
    r = ReadJson(P(L"inf3f.report.json"));
    checkReport(Codes(r, "error") == std::set<std::string>({"invalid-value"}) &&
                    r["findings"][0]["path"] == "emitters[0].unknown3f",
                "the finding names the value's inspect path", r);
    check(Run({L"--tool", L"validate", P(L"badname.alo"), L"--out", P(L"badname.report.json")}) == 3 &&
              Codes(ReadJson(P(L"badname.report.json")), "error") == std::set<std::string>({"invalid-value"}),
          "a name that is not UTF-8 exits 3, and the report is still valid JSON");
    check(Run({L"--tool", L"validate", P(L"garbage.alo"), L"--out", P(L"garbage.report.json")}) == 3,
          "a file that does not load exits 3");
    r = ReadJson(P(L"garbage.report.json"));
    check(Codes(r, "error") == std::set<std::string>({"load-failed"}) && r["canonical"].is_null() &&
              r["sourceDocumentHash"].is_string(),
          "a load failure is reported with the hash and no canonical verdict");
    check(Run({L"--tool", L"validate", P(L"missing.alo"), L"--out", P(L"missing.report.json")}) == 3 &&
              Codes(ReadJson(P(L"missing.report.json")), "error") == std::set<std::string>({"read-failed"}),
          "a missing input exits 3 with a report");

    check(Run({L"--tool", L"validate", P(L"variance.alo"), L"--out", P(L"variance.report.json")}) == 0,
          "a value the loader clamps is valid");
    r = ReadJson(P(L"variance.report.json"));
    check(r.is_object() && r["canonical"] == false, "but not canonical: a save does not reproduce the bytes");

    check(Run({L"--tool", L"validate", P(L"atlas.alo"), L"--out", P(L"atlas.report.json")}) == 0,
          "an out-of-grid atlas index is not an error");
    r                         = ReadJson(P(L"atlas.report.json"));
    nlohmann::json advisories = nlohmann::json::array();
    for (const nlohmann::json& f : r["findings"])
        if (f["severity"] == "advisory") advisories.push_back(f);
    checkReport(advisories.size() == 1 && advisories[0]["code"] == "atlas-out-of-grid" &&
                    advisories[0]["path"] == "emitters[0].tracks[5].keys[1]",
                "it is one advisory, at the key's inspect path (index 3 is inside the 2x2 grid)", r);

    check(Run({L"--tool", L"validate", P(L"good.alo"), L"--out", P(L"good.alo")}) == 2,
          "validate --out naming the input exits 2");
    check(Run({L"--tool", L"validate", P(L"good.alo")}) == 2, "validate without --out exits 2");
    check(Run({L"--tool", L"validate", P(L"good.alo"), L"--out", P(L"no such dir\\z.json")}) == 13,
          "an unwritable report exits 13");

    // The repository's shipped .alo fixtures: <root>\x64\<Config>\ParticleEditor.exe.
    std::wstring root = g_exe;
    for (int i = 0; i < 3; i++) root = root.substr(0, root.find_last_of(L"\\/"));
    const wchar_t* shipped[] = {L"site\\guide\\downloads\\P_EXPLOSION_EXAMPLE.ALO",
                                L"site\\guide\\downloads\\P_EXPLOSION_EXAMPLE_FLIPBOOK.ALO",
                                L"web\\apps\\editor\\tests\\fixtures\\a11y-base-state.alo",
                                L"web\\apps\\editor\\tests\\fixtures\\bump-asteroids.alo",
                                L"web\\apps\\editor\\tests\\fixtures\\replay-guard.alo",
                                L"web\\apps\\editor\\tests\\fixtures\\replay-weather.alo",
                                L"web\\apps\\editor\\tests\\fixtures\\singleton-emitter.alo"};
    // Valid is not canonical. Three fixtures re-save to different bytes:
    // a11y-base-state and singleton-emitter hold the legacy writer's
    // uninitialised bool byte (0xCC / 0x8D in mini-chunk 0x2B; it loads as
    // true and saves as 01), and bump-asteroids, built by hand, omits the
    // optional mini-chunks 0x06, 0x48 and 0x49 that the writer adds.
    const bool canonicalExpected[] = {true, true, false, false, true, true, false};
    int        matched             = 0;
    for (size_t i = 0; i < sizeof(shipped) / sizeof(shipped[0]); i++)
    {
        const int code = Run({L"--tool", L"validate", root + L"\\" + shipped[i], L"--out", P(L"shipped.report.json")});
        r              = ReadJson(P(L"shipped.report.json"));
        const bool ok  = code == 0 && r.is_object() && r["valid"] == true && r["canonical"] == canonicalExpected[i];
        if (ok)
            matched++;
        else
            wprintf(L"    unexpected verdict: %ls (exit %d)\n", shipped[i], code);
        DeleteFileW(P(L"shipped.report.json").c_str());
    }
    check(matched == 7, "all 7 shipped .alo fixtures are valid, each with its pinned canonical verdict");
}

// A patch request for the root of a fixture written by Fixture() ("Root",
// or "Fum\xC3\xA9e" in good.alo) with the given mutations.
static std::string PatchRequest(const std::string& hash, const char* root, const nlohmann::json& mutations)
{
    nlohmann::json path = nlohmann::json::array();
    path.push_back({{"role", "root"}, {"name", root}});
    const nlohmann::json request = {
        {"schema", 1},
        {"selector", {{"sourceDocumentHash", hash}, {"rolePath", path}, {"expected", {{"name", root}}}}},
        {"mutations", mutations}};
    return request.dump();
}

static int Patch(const wchar_t* in, const wchar_t* request, const wchar_t* out, const wchar_t* report)
{
    return Run({L"--tool", L"patch", P(in), P(request), L"--out", P(out), L"--report", P(report)});
}

static void TestPatch(const std::string& goodHash)
{
    printf("patch\n");
    const char*    root = "Fum\xC3\xA9"
                          "e";
    nlohmann::json set  = nlohmann::json::array();
    set.push_back({{"op", "setProperty"}, {"field", "lifetime"}, {"value", 2.5}});
    WriteAll(P(L"set.json"), PatchRequest(goodHash, root, set));
    check(Patch(L"good.alo", L"set.json", L"cand.alo", L"cand.report.json") == 0, "a patch exits 0");
    nlohmann::json    r    = ReadJson(P(L"cand.report.json"));
    const std::string cand = ReadAll(P(L"cand.alo"));
    checkReport(r.is_object() && r["outcome"] == "ok" && r["candidateHash"] == Sha256Hex(cand) &&
                    r["ops"][0]["fields"][0]["persisted"] == 2.5,
                "the report gives the outcome, the persisted value and the candidate's hash", r);
    {
        MemoryFile file;
        file.write(cand.data(), static_cast<unsigned long>(cand.size()));
        file.seek(0);
        bool ok = false;
        try
        {
            ParticleSystem loaded(&file);
            ok = loaded.getEmitter(0).lifetime == 2.5f && loaded.getName() == "cli_fixture";
        }
        catch (...)
        {
        }
        check(ok, "the candidate loads with the new value and keeps the internal name");
    }
    check(Sha256Hex(ReadAll(P(L"good.alo"))) == goodHash, "the input is unchanged");

    const std::string canonHash = Sha256Hex(ReadAll(P(L"canon.alo")));
    WriteAll(P(L"zero.json"), PatchRequest(canonHash, "Root", nlohmann::json::array()));
    check(Patch(L"canon.alo", L"zero.json", L"zero.alo", L"zero.report.json") == 0 &&
              ReadAll(P(L"zero.alo")) == ReadAll(P(L"canon.alo")),
          "a zero-op patch of a canonical file writes it back byte for byte");

    WriteAll(P(L"stale.json"), PatchRequest(std::string(64, '0'), root, set));
    check(Patch(L"good.alo", L"stale.json", L"x.alo", L"stale.report.json") == 4 && !Exists(P(L"x.alo")) &&
              ReadJson(P(L"stale.report.json"))["refusal"]["category"] == "selector-hash-mismatch",
          "a stale hash exits 4, with a report and no candidate");
    nlohmann::json bad = nlohmann::json::array();
    bad.push_back({{"op", "setBlendMode"}, {"value", 14}});
    WriteAll(P(L"rule.json"), PatchRequest(goodHash, root, bad));
    check(Patch(L"good.alo", L"rule.json", L"x.alo", L"rule.report.json") == 5 && !Exists(P(L"x.alo")),
          "a rule refusal exits 5");
    WriteAll(P(L"broken.json"), "{\"schema\": 1,");
    check(Patch(L"good.alo", L"broken.json", L"x.alo", L"broken.report.json") == 2 &&
              ReadJson(P(L"broken.report.json"))["refusal"]["category"] == "request-malformed",
          "a malformed request exits 2 with a report");
    check(Patch(L"good.alo", L"absent.json", L"x.alo", L"absent.report.json") == 2, "a missing request exits 2");
    check(Patch(L"garbage.alo", L"set.json", L"x.alo", L"garbage.patch.json") == 3,
          "a source that does not load exits 3");
    check(!Exists(P(L"x.alo")), "no refused patch wrote a candidate");

    // Paths.
    const wchar_t* names[] = {L"good.alo", L"set.json", L"alias.alo", L"alias.report.json"};
    bool           aliases = true;
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
        {
            const wchar_t* args[4] = {names[0], names[1], names[2], names[3]};
            args[j]                = args[i];
            aliases                = aliases && Patch(args[0], args[1], args[2], args[3]) == 2;
        }
    check(aliases, "every pair of the four paths naming one file exits 2");
    check(Sha256Hex(ReadAll(P(L"good.alo"))) == goodHash && !Exists(P(L"alias.alo")) &&
              !Exists(P(L"alias.report.json")),
          "and writes nothing");
    check(Run({L"--tool", L"patch", P(L"good.alo"), P(L"set.json"), L"--out", P(L"x.alo")}) == 2,
          "patch without --report exits 2");
    check(Patch(L"good.alo", L"set.json", L"no such dir\\x.alo", L"nodir.report.json") == 13 &&
              ReadJson(P(L"nodir.report.json"))["refusal"]["category"] == "io",
          "--out in a missing directory exits 13 with a report");
    check(Patch(L"good.alo", L"set.json", L"adir", L"adir.report.json") == 13, "--out naming a directory exits 13");
    WriteAll(P(L"readonly.alo"), "previous");
    SetFileAttributesW(P(L"readonly.alo").c_str(), FILE_ATTRIBUTE_READONLY);
    check(Patch(L"good.alo", L"set.json", L"readonly.alo", L"readonly.report.json") == 13 &&
              ReadAll(P(L"readonly.alo")) == "previous",
          "a read-only --out exits 13 and keeps its contents");
    SetFileAttributesW(P(L"readonly.alo").c_str(), FILE_ATTRIBUTE_NORMAL);
    check(Patch(L"good.alo", L"set.json", L"x.alo", L"no such dir\\r.json") == 13, "an unwritable report exits 13");
    DeleteFileW(P(L"x.alo").c_str()); // that run committed its candidate before the report failed
}

static int Build(const wchar_t* spec, const wchar_t* out, const wchar_t* report)
{
    return Run({L"--tool", L"build", P(spec), L"--out", P(out), L"--report", P(report)});
}

static void TestBuild()
{
    printf("build\n");
    const nlohmann::json spec = {
        {"schema", 1},
        {"emitters",
         nlohmann::json::array(
             {{{"name", "Smoke"},
               {"mutations", nlohmann::json::array({{{"op", "setProperty"}, {"field", "lifetime"}, {"value", 2.5}}})},
               {"lifeChild", {{"name", "Trail"}, {"mutations", nlohmann::json::array()}}}},
              {{"name", "Fire"}, {"mutations", nlohmann::json::array({{{"op", "setBlendMode"}, {"value", 1}}})}}})}};
    WriteAll(P(L"spec.json"), spec.dump());
    check(Build(L"spec.json", L"P_Built.alo", L"built.report.json") == 0, "a build exits 0");
    const nlohmann::json r     = ReadJson(P(L"built.report.json"));
    const std::string    built = ReadAll(P(L"P_Built.alo"));
    checkReport(r.is_object() && r["outcome"] == "ok" && r["verb"] == "build" &&
                    r["candidateHash"] == Sha256Hex(built) &&
                    r["emitters"][0]["ops"][0]["fields"][0]["persisted"] == 2.5,
                "the report gives the outcome, the persisted value and the hash", r);
    {
        MemoryFile file;
        file.write(built.data(), static_cast<unsigned long>(built.size()));
        file.seek(0);
        bool ok = false;
        try
        {
            ParticleSystem loaded(&file);
            ok = loaded.getName() == "p_built" && loaded.getEmitters().size() == 3 &&
                 loaded.getEmitter(0).lifetime == 2.5f && loaded.getEmitter(1).parent == &loaded.getEmitter(0) &&
                 loaded.getEmitter(2).blendMode == 1;
        }
        catch (...)
        {
        }
        check(ok, "the built file loads with its tree, its edits and the --out file's internal name");
    }
    WriteAll(P(L"badspec.json"), "{\"schema\": 1}");
    check(Build(L"badspec.json", L"x.alo", L"badspec.report.json") == 2 &&
              ReadJson(P(L"badspec.report.json"))["refusal"]["category"] == "spec-malformed",
          "a malformed spec exits 2 with a report");
    check(Build(L"absent.json", L"x.alo", L"absentspec.report.json") == 2, "a missing spec exits 2");
    check(Build(L"spec.json", L"spec.json", L"alias.report.json") == 2 &&
              Build(L"spec.json", L"x.alo", L"x.alo") == 2 && Build(L"spec.json", L"x.alo", L"spec.json") == 2,
          "any two of the three paths naming one file exits 2");
    check(Run({L"--tool", L"build", P(L"spec.json"), L"--out", P(L"x.alo")}) == 2, "build without --report exits 2");
    check(!Exists(P(L"x.alo")) && !Exists(P(L"alias.report.json")), "no refused build wrote a candidate");
}

int main(int argc, char** argv)
{
    printf("test_tool_cli\n");
    if (argc < 2)
    {
        printf("usage: test_tool_cli <ParticleEditor.exe>\n");
        return 2;
    }
    wchar_t exe[MAX_PATH * 4];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, exe, MAX_PATH * 4);
    wchar_t full[MAX_PATH * 4];
    GetFullPathNameW(exe, MAX_PATH * 4, full, nullptr);
    g_exe = full;

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"pe tool cli " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    Fixture(P(L"good.alo"),
            [](ParticleSystem::Emitter& e)
            {
                e.name = "Fum\xC3\xA9"
                         "e";
            });
    Fixture(P(L"nan.alo"), [](ParticleSystem::Emitter& e) { e.lifetime = std::numeric_limits<float>::quiet_NaN(); });
    Fixture(P(L"inf3f.alo"), [](ParticleSystem::Emitter& e) { e.unknown3f = std::numeric_limits<float>::infinity(); });
    Fixture(P(L"badname.alo"), [](ParticleSystem::Emitter& e) { e.name = "bad \xE9"; });
    Fixture(P(L"variance.alo"),
            [](ParticleSystem::Emitter& e) { e.randomRotationVariance = std::numeric_limits<float>::infinity(); });
    {
        ParticleSystem system;
        system.setName("selflink");
        ParticleSystem::Emitter* e = system.addRootEmitter();
        e->name                    = "Loop";
        e->spawnOnDeath            = 0; // itself: the loader clears it
        SaveFixture(system, P(L"selflink.alo"));
    }
    Fixture(P(L"atlas.alo"),
            [](ParticleSystem::Emitter& e)
            {
                e.textureSize                         = 4; // a 2x2 grid
                ParticleSystem::Emitter::Track& index = *e.tracks[ParticleSystem::TRACK_INDEX];
                index.keys.clear();
                index.keys.insert(ParticleSystem::Emitter::Track::Key(0, 0));
                index.keys.insert(ParticleSystem::Emitter::Track::Key(50, 7));
                index.keys.insert(ParticleSystem::Emitter::Track::Key(100, 3));
            });
    WriteAll(P(L"garbage.alo"), "this is not a particle file");
    CreateDirectoryW(P(L"adir").c_str(), nullptr);
    const std::string            goodBytes = ReadAll(P(L"good.alo"));
    const std::string            goodHash  = Sha256Hex(goodBytes);
    const std::set<std::wstring> before    = Listing();

    printf("success\n");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"good.json")}) == 0, "inspect exits 0");
    const std::string out = ReadAll(P(L"good.json"));
    nlohmann::json    dump;
    try
    {
        dump = nlohmann::json::parse(out);
    }
    catch (...)
    {
    }
    check(dump.is_object() && dump["schema"] == 1, "the output is schema 1 JSON");
    check(dump.is_object() && dump["sourceDocumentHash"] == goodHash, "sourceDocumentHash is the input's SHA-256");
    check(out.find("Fum\xC3\xA9"
                   "e") != std::string::npos,
          "a non-ASCII name is written byte for byte");
    check(out.size() < 3 || static_cast<unsigned char>(out[0]) != 0xEF, "the output has no BOM");
    check(ReadAll(P(L"good.alo")) == goodBytes, "the input is unchanged");
    check(Run({L"--tool", L"inspect", L"good.alo", L"--out", L"relative.json"}) == 0,
          "relative paths resolve against the working directory");

    printf("invalid sources\n");
    check(Run({L"--tool", L"inspect", P(L"nan.alo"), L"--out", P(L"x.json")}) == 3, "NaN in a used field exits 3");
    check(Run({L"--tool", L"inspect", P(L"inf3f.alo"), L"--out", P(L"x.json")}) == 3, "infinity in unknown3f exits 3");
    check(Run({L"--tool", L"inspect", P(L"badname.alo"), L"--out", P(L"x.json")}) == 3,
          "a lone 0xE9 byte in a name exits 3");
    check(Run({L"--tool", L"inspect", P(L"garbage.alo"), L"--out", P(L"x.json")}) == 3,
          "a file that does not load exits 3");
    check(Run({L"--tool", L"inspect", P(L"missing.alo"), L"--out", P(L"x.json")}) == 3, "a missing input exits 3");
    check(!Exists(P(L"x.json")), "no output is written for an invalid source");
    check(Run({L"--tool", L"inspect", P(L"variance.alo"), L"--out", P(L"variance.json")}) == 0,
          "a raw 0x17 infinity loads clamped and exits 0");
    try
    {
        const nlohmann::json v = nlohmann::json::parse(ReadAll(P(L"variance.json")));
        check(v["emitters"][0]["randomRotationVariance"] == 1.0, "and inspects as the clamped 1");
    }
    catch (...)
    {
        check(false, "and inspects as the clamped 1");
    }

    printf("usage and aliases\n");
    check(Run({L"--tool"}) == 2, "--tool alone exits 2");
    check(Run({L"--tool", L"frobnicate", P(L"good.alo"), L"--out", P(L"y.json")}) == 2, "an unknown verb exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo")}) == 2, "a missing --out exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"y.json"), L"--extra"}) == 2,
          "an extra argument exits 2");
    check(Run({L"--observe", L"spec.json", L"outdir", L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"y.json")}) ==
              2,
          "--tool with --observe is refused as a mode conflict");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"good.alo")}) == 2,
          "--out naming the input exits 2");
    std::wstring upper = P(L"GOOD.ALO");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", upper}) == 2,
          "a case-different spelling of the input exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", g_dir + L"\\adir\\..\\good.alo"}) == 2,
          "a dot-dot spelling of the input exits 2");
    CreateHardLinkW(P(L"link.alo").c_str(), P(L"good.alo").c_str(), nullptr);
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"link.alo")}) == 2,
          "a hard link to the input exits 2");
    check(ReadAll(P(L"good.alo")) == goodBytes && Sha256Hex(ReadAll(P(L"good.alo"))) == goodHash,
          "the input's hash is unchanged after every alias refusal");

    printf("output failures\n");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"no such dir\\z.json")}) == 13,
          "an output in a missing directory exits 13");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"adir")}) == 13,
          "an output that is a directory exits 13");

    TestValidate(goodHash);
    TestPatch(goodHash);
    TestBuild();

    printf("nothing else on disk\n");
    std::set<std::wstring> expected = before;
    for (const wchar_t* name : {L"good.json",
                                L"relative.json",
                                L"variance.json",
                                L"link.alo",
                                L"canon.alo",
                                L"canon.report.json",
                                L"good.report.json",
                                L"selflink.report.json",
                                L"nan.report.json",
                                L"inf3f.report.json",
                                L"badname.report.json",
                                L"garbage.report.json",
                                L"missing.report.json",
                                L"variance.report.json",
                                L"atlas.report.json",
                                L"set.json",
                                L"cand.alo",
                                L"cand.report.json",
                                L"zero.json",
                                L"zero.alo",
                                L"zero.report.json",
                                L"stale.json",
                                L"stale.report.json",
                                L"rule.json",
                                L"rule.report.json",
                                L"broken.json",
                                L"broken.report.json",
                                L"absent.report.json",
                                L"garbage.patch.json",
                                L"nodir.report.json",
                                L"adir.report.json",
                                L"readonly.alo",
                                L"readonly.report.json",
                                L"spec.json",
                                L"P_Built.alo",
                                L"built.report.json",
                                L"badspec.json",
                                L"badspec.report.json",
                                L"absentspec.report.json"})
        expected.insert(name);
    const std::set<std::wstring> after = Listing();
    check(after == expected, "only the expected outputs exist (no temp files)");
    for (const std::wstring& name : after)
        if (!expected.count(name)) wprintf(L"    unexpected: %ls\n", name.c_str());

    // Clean up.
    for (const std::wstring& name : Listing())
        if (name != L"adir") DeleteFileW(P(name.c_str()).c_str());
    RemoveDirectoryW(P(L"adir").c_str());
    RemoveDirectoryW(g_dir.c_str());

    printf("%s\n", g_failed == 0 ? "all checks passed" : "FAILED");
    return g_failed == 0 ? 0 : 1;
}
