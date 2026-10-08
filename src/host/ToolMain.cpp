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
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemJson.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "StringConv.h"

namespace host
{
namespace
{
const int kExitOk = 0;
const int kExitUsage = 2;
const int kExitSource = 3;
const int kExitIo = 13;
const int kExitInternal = 14;

// The loader's own caps bound what it builds; this bounds the single read.
const unsigned long long kMaxSourceBytes = 512ull * 1024 * 1024;

const wchar_t* kUsage =
    L"usage: ParticleEditor.exe --tool inspect <in.alo> --out <inspect.json>\n";

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

std::string Sha256Hex(const std::string& bytes)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    BYTE digest[32] = {};
    DWORD size = sizeof(digest);
    const bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof(digest);
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) return "";
    const char* hex = "0123456789abcdef";
    std::string result;
    for (BYTE byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}

// The canonical form of `path` for alias comparison: the final path of the
// file (or, for a file that does not exist yet, of its directory plus its
// name), so case, relative, 8.3, `\\?\` and trailing-dot spellings agree.
std::wstring CanonicalPath(const std::wstring& path)
{
    wchar_t full[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    std::wstring result = (n > 0 && n < std::size(full)) ? std::wstring(full, n) : path;
    auto finalPath = [](const std::wstring& p, std::wstring* out) {
        HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        wchar_t buffer[MAX_PATH * 4];
        DWORD len = GetFinalPathNameByHandleW(h, buffer, static_cast<DWORD>(std::size(buffer)), FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (len == 0 || len >= std::size(buffer)) return false;
        *out = std::wstring(buffer, len);
        return true;
    };
    std::wstring resolved;
    if (finalPath(result, &resolved)) return resolved;
    const size_t slash = result.find_last_of(L"\\/");
    if (slash != std::wstring::npos && finalPath(result.substr(0, slash), &resolved))
        return resolved + L"\\" + result.substr(slash + 1);
    return result;
}

bool SameFileIdentity(const std::wstring& a, const std::wstring& b)
{
    auto identity = [](const std::wstring& p, BY_HANDLE_FILE_INFORMATION* info) {
        HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        const bool ok = GetFileInformationByHandle(h, info) != 0;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION ia = {}, ib = {};
    return identity(a, &ia) && identity(b, &ib) && ia.dwVolumeSerialNumber == ib.dwVolumeSerialNumber &&
           ia.nFileIndexHigh == ib.nFileIndexHigh && ia.nFileIndexLow == ib.nFileIndexLow;
}

// True when `a` and `b` name the same file: equal canonical paths (compared
// case-insensitively, as NTFS does) or, for files that exist, the same volume
// and file index, which also catches a hard link.
bool SameFile(const std::wstring& a, const std::wstring& b)
{
    const std::wstring ca = CanonicalPath(a), cb = CanonicalPath(b);
    if (CompareStringOrdinal(ca.c_str(), static_cast<int>(ca.size()), cb.c_str(), static_cast<int>(cb.size()), TRUE) == CSTR_EQUAL)
        return true;
    return SameFileIdentity(a, b);
}

// Reads the whole file. Returns 0, or the exit code with the reason on stderr.
int ReadSource(const std::wstring& path, std::string* bytes)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        fwprintf(stderr, L"[tool] cannot open %ls (error %lu)\n", path.c_str(), GetLastError());
        return kExitSource;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || static_cast<unsigned long long>(size.QuadPart) > kMaxSourceBytes)
    {
        CloseHandle(h);
        fwprintf(stderr, L"[tool] %ls is unreadable or larger than %llu bytes\n", path.c_str(), kMaxSourceBytes);
        return kExitSource;
    }
    bytes->resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes->size())
    {
        DWORD got = 0;
        const size_t left = bytes->size() - done;
        const DWORD want = static_cast<DWORD>(left < (1u << 24) ? left : (1u << 24));
        if (!ReadFile(h, &(*bytes)[done], want, &got, nullptr) || got == 0)
        {
            CloseHandle(h);
            fwprintf(stderr, L"[tool] read of %ls failed (error %lu)\n", path.c_str(), GetLastError());
            return kExitSource;
        }
        done += got;
    }
    CloseHandle(h);
    return kExitOk;
}

// Parses the bytes read once from the source, so the hash and the document
// describe the same content.
std::unique_ptr<ParticleSystem> ParseSource(const std::string& bytes, std::string* error)
{
    try
    {
        MemoryFile file;
        if (!bytes.empty()) file.write(bytes.data(), static_cast<unsigned long>(bytes.size()));
        file.seek(0);
        return std::make_unique<ParticleSystem>(&file);
    }
    catch (wexception& e) { *error = WideToUtf8(e.wwhat()); }
    catch (const std::exception& e) { *error = e.what(); }
    catch (...) {}
    if (error->empty()) *error = "the file is not a particle system";
    return nullptr;
}

// Writes `bytes` to a unique sibling, then renames it over `path`. Nothing is
// left behind on failure.
bool WriteFileAtomically(const std::wstring& path, const std::string& bytes)
{
    const std::wstring temp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
              written == bytes.size() && FlushFileBuffers(h);
    CloseHandle(h);
    ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
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
