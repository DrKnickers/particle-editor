#pragma once
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Headless runs (capture, automation, test-host) get a per-PID WebView2 profile so they
// never contend with the daily driver's locked profile. A run cannot delete its own folder
// (SweepStaleIsolatedProfiles below clears dead runs' folders at the next headless start),
// and Windows reuses PIDs, so a "throwaway" profile could start with an earlier run's
// localStorage: a stale right-dock or panel size changes the scene rect, and with it the
// viewport geometry a reproducible run (--observe, --replay-proof) records. A repeat
// --observe run measured 918 px instead of 658 px wide this way.
//
// The fix is to start the profile empty. Header-only so
// tests/test_isolated_webview_profile.cpp can drive it against a temp directory.

namespace host
{

// Only ever wipes a folder whose leaf names an isolated profile; anything else is left
// alone and returned unchanged.
inline bool IsIsolatedProfileLeaf(const std::filesystem::path& folder)
{
    return folder.filename().wstring().find(L"WebView2-iso-") != std::wstring::npos;
}

// Returns a folder that exists and is empty: the given per-PID folder wiped, or a fresh
// sibling named with `salt` when the wipe fails (a stray runtime process from the earlier
// owner of this PID can still hold files open).
inline std::filesystem::path FreshIsolatedProfileFolder(const std::filesystem::path& folder, unsigned long long salt)
{
    if (!IsIsolatedProfileLeaf(folder)) return folder;
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (!std::filesystem::exists(folder, error))
    {
        std::filesystem::create_directories(folder, error);
        return folder;
    }
    auto fallback = folder;
    fallback += L"-" + std::to_wstring(salt);
    std::filesystem::remove_all(fallback, error);
    std::filesystem::create_directories(fallback, error);
    return fallback;
}

// The PID an isolated profile folder is named for. The leaf must be exactly `stem` + <pid>
// or `stem` + <pid> + "-" + <salt>, digits only; any other shape returns 0, which is never
// an editor's PID (it is the idle process).
inline unsigned long IsolatedProfileOwnerPid(const std::filesystem::path& folder, const std::wstring& stem)
{
    const std::wstring leaf = folder.filename().wstring();
    if (stem.empty() || leaf.compare(0, stem.size(), stem) != 0) return 0;
    auto digitsEnd = [&leaf](size_t from)
    {
        while (from < leaf.size() && leaf[from] >= L'0' && leaf[from] <= L'9') ++from;
        return from;
    };
    const size_t pidBegin = stem.size();
    const size_t pidEnd   = digitsEnd(pidBegin);
    if (pidEnd == pidBegin || pidEnd - pidBegin > 10) return 0;
    if (pidEnd != leaf.size())
    {
        if (leaf[pidEnd] != L'-') return 0;
        const size_t saltEnd = digitsEnd(pidEnd + 1);
        if (saltEnd == pidEnd + 1 || saltEnd != leaf.size()) return 0;
    }
    const unsigned long long pid = std::stoull(leaf.substr(pidBegin, pidEnd - pidBegin));
    return pid > 0xFFFFFFFFull ? 0 : static_cast<unsigned long>(pid);
}

// A headless run cannot delete its own profile on the way out: the WebView2 browser
// processes hold it open until after the host is gone. So each isolated run sweeps, best
// effort, the profiles beside `ownFolder` whose owner `isOwnerLive(pid)` reports gone. Only
// directories named with ownFolder's stem are candidates; files, junctions and symlinks are
// skipped. Returns how many were removed.
template <class IsOwnerLive>
inline int SweepStaleIsolatedProfiles(const std::filesystem::path& ownFolder, IsOwnerLive isOwnerLive)
{
    if (!IsIsolatedProfileLeaf(ownFolder)) return 0;
    const std::wstring                 leaf = ownFolder.filename().wstring();
    const std::wstring                 stem = leaf.substr(0, leaf.find(L"WebView2-iso-") + 13);
    std::vector<std::filesystem::path> stale;
    std::error_code                    error;
    for (std::filesystem::directory_iterator it(ownFolder.parent_path(), error), end; !error && it != end;
         it.increment(error))
    {
        std::error_code typeError;
        if (it->symlink_status(typeError).type() != std::filesystem::file_type::directory) continue;
        const unsigned long pid = IsolatedProfileOwnerPid(it->path(), stem);
        if (pid != 0 && !isOwnerLive(pid)) stale.push_back(it->path());
    }
    int removed = 0;
    for (const auto& folder : stale)
    {
        std::filesystem::remove_all(folder, error);
        if (!std::filesystem::exists(folder, error) && !error) ++removed;
    }
    return removed;
}

} // namespace host
