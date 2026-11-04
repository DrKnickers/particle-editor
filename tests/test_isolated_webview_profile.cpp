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
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
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
    const fs::path  root = fs::temp_directory_path() / (L"AloIsoProfileTest-" + std::to_wstring(GetCurrentProcessId()));
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
    const fs::path held     = root / L"WebView2-iso-6262";
    const fs::path heldFile = held / L"lock";
    Touch(heldFile);
    HANDLE lock =
        CreateFileW(heldFile.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

    // The owner PID is read only from the exact shape ComputeUserDataFolder names.
    const std::wstring stem = L"WebView2-iso-";
    CHECK(host::IsolatedProfileOwnerPid(L"WebView2-iso-4242", stem) == 4242, "owner of a per-PID profile");
    CHECK(host::IsolatedProfileOwnerPid(L"WebView2-iso-4242-99", stem) == 4242, "owner of a salted profile");
    CHECK(host::IsolatedProfileOwnerPid(L"AloParticleEditor_WebView2-iso-12", L"AloParticleEditor_WebView2-iso-") == 12,
          "owner of a TEMP-fallback profile");
    bool rejected = true;
    for (const wchar_t* leaf :
         {L"WebView2", L"WebView2-iso-", L"WebView2-iso-abc", L"WebView2-iso-12x", L"WebView2-iso-12-",
          L"WebView2-iso-12-x", L"WebView2-iso-12-3-4", L"WebView2-iso-99999999999", L"WebView2-iso-4294967296",
          L"AloParticleEditor_WebView2-iso-12", L"xWebView2-iso-12"})
        rejected = rejected && host::IsolatedProfileOwnerPid(leaf, stem) == 0;
    CHECK(rejected, "every other shape, an overflowing PID and a different stem have no owner");

    // The sweep removes dead owners' profiles and leaves everything else.
    const fs::path sweep = root / L"sweep";
    const fs::path own   = sweep / L"WebView2-iso-1";
    for (const wchar_t* leaf : {L"WebView2-iso-1", L"WebView2-iso-2", L"WebView2-iso-3", L"WebView2-iso-3-77",
                                L"WebView2", L"foo-iso-3", L"WebView2-iso-3x"})
        Touch(sweep / leaf / L"EBWebView" / L"data");
    Touch(sweep / L"WebView2-iso-30"); // a FILE named like a profile
    const fs::path outside = root / L"outside";
    Touch(outside / L"precious.txt");
    const std::wstring junctionCommand =
        L"cmd /c mklink /J \"" + (sweep / L"WebView2-iso-31").wstring() + L"\" \"" + outside.wstring() + L"\" >nul";
    const bool junction = _wsystem(junctionCommand.c_str()) == 0;
    CHECK(junction, "test made a junction named like a dead profile");
    auto live = [](unsigned long pid)
    {
        return pid == 1 || pid == 2;
    };
    const int removed = host::SweepStaleIsolatedProfiles(own, live);
    CHECK(removed == 2, "the sweep reports the two dead profiles it removed");
    CHECK(!fs::exists(sweep / L"WebView2-iso-3") && !fs::exists(sweep / L"WebView2-iso-3-77"),
          "a dead owner's profile and its salted sibling are removed");
    CHECK(fs::exists(own / L"EBWebView" / L"data") && fs::exists(sweep / L"WebView2-iso-2" / L"EBWebView" / L"data"),
          "our own and a live sibling's profiles are kept");
    CHECK(fs::exists(sweep / L"WebView2" / L"EBWebView" / L"data"), "the daily-driver profile is kept");
    CHECK(fs::exists(sweep / L"foo-iso-3") && fs::exists(sweep / L"WebView2-iso-3x") &&
              fs::is_regular_file(sweep / L"WebView2-iso-30"),
          "unrelated folders and a file named like a profile are kept");
    CHECK(fs::exists(outside / L"precious.txt"), "a junction is never followed");
    if (junction) RemoveDirectoryW((sweep / L"WebView2-iso-31").c_str());

    // A dead profile still held open is left for a later sweep, and the rest still go.
    const fs::path heldDead = sweep / L"WebView2-iso-40";
    Touch(heldDead / L"lock");
    Touch(sweep / L"WebView2-iso-41" / L"data");
    HANDLE deadLock = CreateFileW((heldDead / L"lock").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    host::SweepStaleIsolatedProfiles(own, live);
    CHECK(fs::exists(heldDead / L"lock") && !fs::exists(sweep / L"WebView2-iso-41"),
          "a held profile survives without stopping the sweep");
    CloseHandle(deadLock);

    // A folder that is not an isolated profile never sweeps its siblings.
    CHECK(host::SweepStaleIsolatedProfiles(sweep / L"WebView2", [](unsigned long) { return false; }) == 0 &&
              fs::exists(own),
          "a non-isolated folder sweeps nothing");

    fs::remove_all(root, error);
    std::printf("%s (%d failures)\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===", g_failed);
    return g_failed ? 1 : 0;
}
