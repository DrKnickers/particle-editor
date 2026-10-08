// `ParticleEditor.exe --tool inspect` end to end (Phase 4B PR 1). Drives the
// built exe (NEEDS_EXE: run-native-unit-tests passes its path as argv[1]) on
// fixtures this test writes, and checks exit codes, the output, the path-alias
// refusals and that nothing else is left on disk. Every run has a deadline: a
// tool run that opened a dialog would hang and fail here.
//
// Exit codes (src/host/ToolMain.h): 0 ok, 2 usage or alias, 3 invalid source,
// 13 output not written.

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

static int g_failed = 0;
static std::wstring g_exe;
static std::wstring g_dir;

static void check(bool ok, const char* what)
{
    if (!ok) { g_failed++; printf("  FAIL: %s\n", what); }
    else printf("  ok: %s\n", what);
}

static std::wstring P(const wchar_t* name) { return g_dir + L"\\" + name; }

static std::string ReadAll(const std::wstring& path)
{
    std::string bytes;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return bytes;
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &got, nullptr) && got > 0) bytes.append(buffer, got);
    CloseHandle(h);
    return bytes;
}

static void WriteAll(const std::wstring& path, const std::string& bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(h);
}

static std::string Sha256Hex(const std::string& bytes)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[32] = {};
    DWORD size = sizeof(digest);
    CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0);
    CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
    CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE b : digest) { result += hex[b >> 4]; result += hex[b & 15]; }
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
    e->name = "Root";
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
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> line(command.begin(), command.end());
    line.push_back(0);
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, g_dir.c_str(), &si, &pi)) return -2;
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

static std::set<std::wstring> Listing()
{
    std::set<std::wstring> names;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileW(P(L"*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return names;
    do
    {
        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..") names.insert(name);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    return names;
}

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

int main(int argc, char** argv)
{
    printf("test_tool_inspect_cli\n");
    if (argc < 2) { printf("usage: test_tool_inspect_cli <ParticleEditor.exe>\n"); return 2; }
    wchar_t exe[MAX_PATH * 4];
    MultiByteToWideChar(CP_ACP, 0, argv[1], -1, exe, MAX_PATH * 4);
    wchar_t full[MAX_PATH * 4];
    GetFullPathNameW(exe, MAX_PATH * 4, full, nullptr);
    g_exe = full;

    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    g_dir = std::wstring(temp) + L"pe tool cli " + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_dir.c_str(), nullptr);

    Fixture(P(L"good.alo"), [](ParticleSystem::Emitter& e) { e.name = "Fum\xC3\xA9" "e"; });
    Fixture(P(L"nan.alo"), [](ParticleSystem::Emitter& e) { e.lifetime = std::numeric_limits<float>::quiet_NaN(); });
    Fixture(P(L"inf3f.alo"), [](ParticleSystem::Emitter& e) { e.unknown3f = std::numeric_limits<float>::infinity(); });
    Fixture(P(L"badname.alo"), [](ParticleSystem::Emitter& e) { e.name = "bad \xE9"; });
    Fixture(P(L"variance.alo"), [](ParticleSystem::Emitter& e) { e.randomRotationVariance = std::numeric_limits<float>::infinity(); });
    WriteAll(P(L"garbage.alo"), "this is not a particle file");
    CreateDirectoryW(P(L"adir").c_str(), nullptr);
    const std::string goodBytes = ReadAll(P(L"good.alo"));
    const std::string goodHash = Sha256Hex(goodBytes);
    const std::set<std::wstring> before = Listing();

    printf("success\n");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"good.json")}) == 0, "inspect exits 0");
    const std::string out = ReadAll(P(L"good.json"));
    nlohmann::json dump;
    try { dump = nlohmann::json::parse(out); } catch (...) {}
    check(dump.is_object() && dump["schema"] == 1, "the output is schema 1 JSON");
    check(dump.is_object() && dump["sourceDocumentHash"] == goodHash, "sourceDocumentHash is the input's SHA-256");
    check(out.find("Fum\xC3\xA9" "e") != std::string::npos, "a non-ASCII name is written byte for byte");
    check(out.size() < 3 || static_cast<unsigned char>(out[0]) != 0xEF, "the output has no BOM");
    check(ReadAll(P(L"good.alo")) == goodBytes, "the input is unchanged");
    check(Run({L"--tool", L"inspect", L"good.alo", L"--out", L"relative.json"}) == 0, "relative paths resolve against the working directory");

    printf("invalid sources\n");
    check(Run({L"--tool", L"inspect", P(L"nan.alo"), L"--out", P(L"x.json")}) == 3, "NaN in a used field exits 3");
    check(Run({L"--tool", L"inspect", P(L"inf3f.alo"), L"--out", P(L"x.json")}) == 3, "infinity in unknown3f exits 3");
    check(Run({L"--tool", L"inspect", P(L"badname.alo"), L"--out", P(L"x.json")}) == 3, "a lone 0xE9 byte in a name exits 3");
    check(Run({L"--tool", L"inspect", P(L"garbage.alo"), L"--out", P(L"x.json")}) == 3, "a file that does not load exits 3");
    check(Run({L"--tool", L"inspect", P(L"missing.alo"), L"--out", P(L"x.json")}) == 3, "a missing input exits 3");
    check(!Exists(P(L"x.json")), "no output is written for an invalid source");
    check(Run({L"--tool", L"inspect", P(L"variance.alo"), L"--out", P(L"variance.json")}) == 0, "a raw 0x17 infinity loads clamped and exits 0");
    try
    {
        const nlohmann::json v = nlohmann::json::parse(ReadAll(P(L"variance.json")));
        check(v["emitters"][0]["randomRotationVariance"] == 1.0, "and inspects as the clamped 1");
    }
    catch (...) { check(false, "and inspects as the clamped 1"); }

    printf("usage and aliases\n");
    check(Run({L"--tool"}) == 2, "--tool alone exits 2");
    check(Run({L"--tool", L"frobnicate", P(L"good.alo"), L"--out", P(L"y.json")}) == 2, "an unknown verb exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo")}) == 2, "a missing --out exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"y.json"), L"--extra"}) == 2, "an extra argument exits 2");
    check(Run({L"--observe", L"spec.json", L"outdir", L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"y.json")}) == 2,
          "--tool with --observe is refused as a mode conflict");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"good.alo")}) == 2, "--out naming the input exits 2");
    std::wstring upper = P(L"GOOD.ALO");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", upper}) == 2, "a case-different spelling of the input exits 2");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", g_dir + L"\\adir\\..\\good.alo"}) == 2, "a dot-dot spelling of the input exits 2");
    CreateHardLinkW(P(L"link.alo").c_str(), P(L"good.alo").c_str(), nullptr);
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"link.alo")}) == 2, "a hard link to the input exits 2");
    check(ReadAll(P(L"good.alo")) == goodBytes && Sha256Hex(ReadAll(P(L"good.alo"))) == goodHash,
          "the input's hash is unchanged after every alias refusal");

    printf("output failures\n");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"no such dir\\z.json")}) == 13, "an output in a missing directory exits 13");
    check(Run({L"--tool", L"inspect", P(L"good.alo"), L"--out", P(L"adir")}) == 13, "an output that is a directory exits 13");

    printf("nothing else on disk\n");
    std::set<std::wstring> expected = before;
    for (const wchar_t* name : {L"good.json", L"relative.json", L"variance.json", L"link.alo"}) expected.insert(name);
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
