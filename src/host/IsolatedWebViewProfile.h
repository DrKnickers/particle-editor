#pragma once
#include <filesystem>
#include <string>
#include <system_error>

// Headless runs (capture, automation, test-host) get a per-PID WebView2 profile so they
// never contend with the daily driver's locked profile. Nothing deletes those folders and
// Windows reuses PIDs, so a "throwaway" profile could start with an earlier run's
// localStorage: a stale right-dock or panel size changes the scene rect, and with it the
// viewport geometry a reproducible run (--observe, --replay-proof) records. A repeat
// --observe run measured 918 px instead of 658 px wide this way.
//
// The fix is to start the profile empty. Header-only so
// tests/test_isolated_webview_profile.cpp can drive it against a temp directory.

namespace host {

// Only ever wipes a folder whose leaf names an isolated profile; anything else is left
// alone and returned unchanged.
inline bool IsIsolatedProfileLeaf(const std::filesystem::path& folder)
{
    return folder.filename().wstring().find(L"WebView2-iso-") != std::wstring::npos;
}

// Returns a folder that exists and is empty: the given per-PID folder wiped, or a fresh
// sibling named with `salt` when the wipe fails (a stray runtime process from the earlier
// owner of this PID can still hold files open).
inline std::filesystem::path FreshIsolatedProfileFolder(const std::filesystem::path& folder,
                                                        unsigned long long salt)
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

} // namespace host
