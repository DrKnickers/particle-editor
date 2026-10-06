// Unit test for src/host/IsolatedWebViewProfile.h.
//
// Per-PID headless WebView2 profiles were never deleted, and Windows reuses PIDs, so an
// --observe repeat inherited a two-day-old profile whose localStorage closed the right
// dock: the viewport came up 918 px wide instead of 658 px and the bundle differed.
// FreshIsolatedProfileFolder must hand every isolated run an empty profile, must fall
// back to a fresh sibling when the old one cannot be removed, and must never touch a
// folder that is not an isolated profile.

#include "host/IsolatedWebViewProfile.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

namespace fs = std::filesystem;

static void Touch(const fs::path& path)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "alo:right-dock none";
}

int main()
{
    std::printf("test_isolated_webview_profile\n");
    const fs::path root = fs::temp_directory_path() /
        (L"AloIsoProfileTest-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    fs::remove_all(root, error);

    // A reused PID's stale profile is wiped in place.
    const fs::path stale = root / L"WebView2-iso-4242";
    Touch(stale / L"EBWebView" / L"Default" / L"Local Storage" / L"leveldb" / L"000003.log");
    const fs::path fresh = host::FreshIsolatedProfileFolder(stale, 7);
    CHECK(fresh == stale, "a removable stale profile keeps its per-PID name");
    CHECK(fs::exists(fresh) && fs::is_empty(fresh), "the stale profile starts empty");

    // An absent profile is created empty.
    const fs::path absent = root / L"WebView2-iso-5151";
    CHECK(host::FreshIsolatedProfileFolder(absent, 7) == absent && fs::is_empty(absent),
          "an absent profile is created empty");

    // A file held open without sharing cannot be removed: use a fresh sibling instead.
    const fs::path held = root / L"WebView2-iso-6262";
    const fs::path heldFile = held / L"lock";
    Touch(heldFile);
    HANDLE lock = CreateFileW(heldFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE, "test holds a profile file open without sharing");
    const fs::path fallback = host::FreshIsolatedProfileFolder(held, 99);
    CloseHandle(lock);
    CHECK(fallback == fs::path(held.wstring() + L"-99"), "a locked profile falls back to a salted sibling");
    CHECK(fs::exists(fallback) && fs::is_empty(fallback), "the fallback sibling starts empty");

    // Anything that is not an isolated profile is never deleted.
    const fs::path daily = root / L"WebView2";
    Touch(daily / L"keep.txt");
    CHECK(host::FreshIsolatedProfileFolder(daily, 7) == daily && fs::exists(daily / L"keep.txt"),
          "the daily-driver profile is left untouched");

    fs::remove_all(root, error);
    std::printf("%s (%d failures)\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===", g_failed);
    return g_failed ? 1 : 0;
}
