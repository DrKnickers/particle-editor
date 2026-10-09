// File and document helpers shared by the `--tool` verbs. See ToolIO.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>
#include "ToolIO.h"
#include "effect/ParticleSystem.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "StringConv.h"

namespace host
{
// The loader's own caps bound what it builds; this bounds the single read.
static const unsigned long long kMaxSourceBytes = 512ull * 1024 * 1024;

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
static std::wstring CanonicalPath(const std::wstring& path)
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

static bool SameFileIdentity(const std::wstring& a, const std::wstring& b)
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

bool ReadFileBytes(const std::wstring& path, std::string* bytes, std::string* error)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        *error = "cannot open the file (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || static_cast<unsigned long long>(size.QuadPart) > kMaxSourceBytes)
    {
        CloseHandle(h);
        *error = "the file is unreadable or larger than 512 MiB";
        return false;
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
            *error = "the read failed (error " + std::to_string(GetLastError()) + ")";
            CloseHandle(h);
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
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

std::string SaveToBytes(ParticleSystem& system)
{
    MemoryFile file;
    system.write(&file);
    file.seek(0);
    std::string bytes(file.size(), '\0');
    if (!bytes.empty()) file.read(&bytes[0], static_cast<unsigned long>(bytes.size()));
    return bytes;
}
} // namespace host
