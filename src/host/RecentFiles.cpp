#include "BridgeDispatchShared.h"

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <utility>

namespace host {

namespace {

// Recent-files registry helpers.
//
// Storage layout matches legacy's `AddToHistory` / `GetHistory` in
// src/main.cpp:650-768 — values under `HKCU\Software\AloParticleEditor`
// keyed by filename, with the FILETIME payload encoded as REG_BINARY.
// The list is ordered most-recent-first by reading the FILETIME values
// and sorting descending. The cap of 9 (`kMaxRecentFiles`) mirrors the
// legacy `NUM_HISTORY_ITEMS` constant (since removed from src/main.cpp).
constexpr size_t kMaxRecentFiles = 9;

// Legacy history values use the filename itself as the registry value name.
// All interactive paths are absolute; accepting a separator, drive qualifier,
// or a bare .alo filename also preserves relative automation inputs without
// misclassifying an unrelated FILETIME-sized binary setting in the shared key.
bool LooksLikeRecentFileName(const wchar_t* name)
{
    if (!name || !*name) return false;
    if (std::wcschr(name, L'\\') || std::wcschr(name, L'/')) return true;
    if (name[0] != L'\0' && name[1] == L':') return true;

    const wchar_t* extension = std::wcsrchr(name, L'.');
    return extension && _wcsicmp(extension, L".alo") == 0;
}

// Read every registry-backed history entry into a vector ordered
// most-recent-first. This intentionally does not cap the result: the writer
// needs the complete sorted list so it can physically delete stale values.
// The public reader below applies the nine-item presentation cap.
std::vector<std::wstring> ReadRecentFilesUncapped()
{
    std::vector<std::pair<ULONGLONG, std::wstring>> entries;

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                      KEY_READ, &hKey) != ERROR_SUCCESS)
    {
        return {};
    }

    for (int i = 0;; ++i)
    {
        wchar_t name[1024] = {};
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        DWORD type = 0, size = 0;
        LONG err = RegEnumValueW(hKey, i, name, &nameLen, nullptr,
                                 &type, nullptr, &size);
        if (err != ERROR_SUCCESS) break;

        // This shared key also stores editor settings. Only path-named
        // REG_BINARY values with a FILETIME-sized payload belong to the MRU.
        if (type == REG_BINARY && size == sizeof(FILETIME)
            && LooksLikeRecentFileName(name))
        {
            FILETIME ft = {};
            DWORD sz = sizeof(ft);
            if (RegQueryValueExW(hKey, name, nullptr, &type,
                                 reinterpret_cast<BYTE*>(&ft),
                                 &sz) == ERROR_SUCCESS)
            {
                ULARGE_INTEGER ull;
                ull.LowPart  = ft.dwLowDateTime;
                ull.HighPart = ft.dwHighDateTime;
                entries.emplace_back(ull.QuadPart, std::wstring(name));
            }
        }
    }
    RegCloseKey(hKey);

    // Registry enumeration order is not MRU order. Sort by the persisted
    // timestamp before either presenting or physically trimming entries.
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<std::wstring> out;
    out.reserve(entries.size());
    for (auto& e : entries) out.push_back(std::move(e.second));
    return out;
}

}  // namespace

// Read the registry-backed history into a vector ordered most-recent-first.
// Silently returns an empty vector when the key does not exist (first run).
std::vector<std::wstring> ReadRecentFiles()
{
    auto out = ReadRecentFilesUncapped();
    if (out.size() > kMaxRecentFiles)
    {
        out.resize(kMaxRecentFiles);
    }
    return out;
}

// Add (or move-to-top) `path` in the registry. Mirrors legacy AddToHistory:
// write the FILETIME for "now" under the path-as-key, then physically delete
// valid MRU entries beyond the cap. Returns the capped most-recent-first list.
std::vector<std::wstring> WriteRecentFile(const std::wstring& path)
{
    FILETIME ft;
    SYSTEMTIME st;
    GetSystemTime(&st);
    SystemTimeToFileTime(&st, &ft);

    // If the value already exists, this refreshes its FILETIME and moves
    // the path to the top of the logical MRU ordering.
    WriteRegBinary(path.c_str(), &ft, sizeof(ft));

    // Use the complete filtered and timestamp-sorted list. Calling the capped
    // public reader here makes this trim branch unreachable.
    auto list = ReadRecentFilesUncapped();

    if (list.size() > kMaxRecentFiles)
    {
        HKEY hTrim;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                          KEY_READ | KEY_WRITE, &hTrim) == ERROR_SUCCESS)
        {
            for (size_t i = kMaxRecentFiles; i < list.size(); ++i)
            {
                RegDeleteValueW(hTrim, list[i].c_str());
            }
            RegCloseKey(hTrim);
        }
        list.resize(kMaxRecentFiles);
    }
    return list;
}

}  // namespace host
