// Black-box test for PaletteStore data layer.
//
// Compiled & linked separately from the main editor exe. Exercises
// mod-switch, recent/pin lifecycle, eviction, persistence, and edge
// cases without driving any GUI.
//
// Uses ALO_PALETTE_DIR to redirect persistence into a unique temp
// directory before Store construction. The test never reads or writes
// %APPDATA%\AloParticleEditor\texture-palettes.ini; a crashed run can
// leave only temp files behind.
//
// Build (from repo root):
//   tests\build_test_palette_store.bat
// Run:
//   tests\test_palette_store.exe

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "UI/TexturePalette.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace TexturePalette;

static int g_passed = 0;
static int g_failed = 0;
static std::wstring g_paletteDir;
static std::wstring g_iniPath;

#define ASSERT_TRUE(cond) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: ASSERT_TRUE(%s)\n", __LINE__, #cond); } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: ASSERT_EQ(%s, %s)\n", __LINE__, #a, #b); } \
} while(0)

#define ASSERT_STREQ(a, b) do { \
    std::wstring _a = (a); std::wstring _b = (b); \
    if (_a == _b) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: ASSERT_STREQ\n", __LINE__); } \
} while(0)

// ---- Helpers ------------------------------------------------------------

static bool CreateTempPaletteDir()
{
    wchar_t tempRoot[MAX_PATH];
    DWORD n = GetTempPathW(_countof(tempRoot), tempRoot);
    if (n == 0 || n >= _countof(tempRoot)) return false;

    for (DWORD attempt = 0; attempt < 100; ++attempt)
    {
        wchar_t candidate[MAX_PATH];
        swprintf_s(candidate, L"%sAloPaletteStoreTest-%lu-%llu-%lu",
                   tempRoot,
                   GetCurrentProcessId(),
                   (unsigned long long)GetTickCount64(),
                   attempt);
        if (CreateDirectoryW(candidate, NULL))
        {
            g_paletteDir = candidate;
            g_iniPath = g_paletteDir + L"\\texture-palettes.ini";
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    return false;
}

static void CleanupTempPaletteDir()
{
    Store::Instance().ClearActiveMod();
    DeleteFileW(g_iniPath.c_str());
    RemoveDirectoryW(g_paletteDir.c_str());
    SetEnvironmentVariableW(L"ALO_PALETTE_DIR", NULL);
}

static void ResetState()
{
    // Force a clean slate between tests. ClearActiveMod also erases
    // the INI section for whatever mod is currently active, which is
    // what we want — each test starts from a known state.
    Store::Instance().ClearActiveMod();
    // Wipe the entire INI file so per-mod sections from prior tests
    // don't leak in (ClearActiveMod only wipes the current section).
    DeleteFileW(g_iniPath.c_str());
}

static size_t CountWithSlot(const std::vector<Entry>& v, SlotMask slot)
{
    size_t n = 0;
    for (const Entry& e : v) if (e.slotMask & slot) ++n;
    return n;
}

static bool ContainsFilename(const std::vector<Entry>& v, const wchar_t* fn)
{
    for (const Entry& e : v) if (e.filename == fn) return true;
    return false;
}

static int IndexOfFilename(const std::vector<Entry>& v, const wchar_t* fn)
{
    for (size_t i = 0; i < v.size(); ++i) if (v[i].filename == fn) return (int)i;
    return -1;
}

// ---- Test cases ---------------------------------------------------------

static void test_cold_start_empty()
{
    std::printf("test_cold_start_empty\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    ASSERT_STREQ(Store::Instance().ActiveMod(), L"C:\\Test\\ModA");
    ASSERT_TRUE(Store::Instance().Pins(SLOT_COLOR).empty());
    ASSERT_TRUE(Store::Instance().Recents(SLOT_COLOR).empty());
    ASSERT_TRUE(Store::Instance().Pins(SLOT_BUMP).empty());
    ASSERT_TRUE(Store::Instance().Recents(SLOT_BUMP).empty());
    ASSERT_EQ(Store::Instance().ActiveFilter(), SLOT_COLOR);
}

static void test_touch_new_recent()
{
    std::printf("test_touch_new_recent\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"a.tga", SLOT_COLOR);
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(recents.size(), (size_t)1);
    ASSERT_STREQ(recents[0].filename, L"a.tga");
    ASSERT_EQ(recents[0].isPinned, false);
    ASSERT_TRUE((recents[0].slotMask & SLOT_COLOR) != 0);
}

static void test_touch_existing_bumps_order()
{
    std::printf("test_touch_existing_bumps_order\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"first.tga",  SLOT_COLOR);
    Store::Instance().TouchRecent(L"second.tga", SLOT_COLOR);
    Store::Instance().TouchRecent(L"third.tga",  SLOT_COLOR);
    // Re-touch first.tga — should move it to position 0.
    Store::Instance().TouchRecent(L"first.tga",  SLOT_COLOR);
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(recents.size(), (size_t)3);
    ASSERT_STREQ(recents[0].filename, L"first.tga");
    ASSERT_STREQ(recents[1].filename, L"third.tga");
    ASSERT_STREQ(recents[2].filename, L"second.tga");
}

static void test_touch_existing_with_different_slot()
{
    std::printf("test_touch_existing_with_different_slot\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"shared.tga", SLOT_COLOR);
    Store::Instance().TouchRecent(L"shared.tga", SLOT_BUMP);
    // Should now appear in BOTH Color and Bump filters.
    auto colorRecents = Store::Instance().Recents(SLOT_COLOR);
    auto bumpRecents  = Store::Instance().Recents(SLOT_BUMP);
    ASSERT_TRUE(ContainsFilename(colorRecents, L"shared.tga"));
    ASSERT_TRUE(ContainsFilename(bumpRecents,  L"shared.tga"));
    // Total entries (across both filters) is still 1 logical entry.
    ASSERT_EQ(colorRecents.size(), (size_t)1);
    ASSERT_EQ(bumpRecents.size(),  (size_t)1);
}

static void test_recent_eviction_at_cap()
{
    std::printf("test_recent_eviction_at_cap\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    // Fill recents to the cap.
    for (size_t i = 0; i < MAX_RECENTS; ++i)
    {
        wchar_t fn[32];
        swprintf_s(fn, L"r%zu.tga", i);
        Store::Instance().TouchRecent(fn, SLOT_COLOR);
    }
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), MAX_RECENTS);
    // Adding one more should evict the oldest (r0).
    Store::Instance().TouchRecent(L"overflow.tga", SLOT_COLOR);
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(recents.size(), MAX_RECENTS);
    ASSERT_TRUE(!ContainsFilename(recents, L"r0.tga"));   // evicted
    ASSERT_TRUE(ContainsFilename(recents, L"overflow.tga"));
    ASSERT_STREQ(recents[0].filename, L"overflow.tga");   // most recent first
}

static void test_pin_from_recent()
{
    std::printf("test_pin_from_recent\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"pinme.tga", SLOT_COLOR);
    ASSERT_TRUE(Store::Instance().TogglePin(L"pinme.tga"));
    auto pins    = Store::Instance().Pins(SLOT_COLOR);
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(pins.size(),    (size_t)1);
    ASSERT_EQ(recents.size(), (size_t)0);
    ASSERT_STREQ(pins[0].filename, L"pinme.tga");
    ASSERT_TRUE(pins[0].isPinned);
}

static void test_pin_overflow_rejected()
{
    std::printf("test_pin_overflow_rejected\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    // Pin MAX_PINS entries.
    for (size_t i = 0; i < MAX_PINS; ++i)
    {
        wchar_t fn[32];
        swprintf_s(fn, L"p%zu.tga", i);
        Store::Instance().TouchRecent(fn, SLOT_COLOR);
        ASSERT_TRUE(Store::Instance().TogglePin(fn));
    }
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(), MAX_PINS);
    // 9th pin must be rejected.
    Store::Instance().TouchRecent(L"overflow.tga", SLOT_COLOR);
    ASSERT_TRUE(!Store::Instance().TogglePin(L"overflow.tga"));
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(), MAX_PINS);
    // overflow.tga should remain in recents.
    ASSERT_TRUE(ContainsFilename(Store::Instance().Recents(SLOT_COLOR), L"overflow.tga"));
}

static void test_unpin_returns_to_recent()
{
    std::printf("test_unpin_returns_to_recent\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"foo.tga", SLOT_COLOR);
    Store::Instance().TogglePin(L"foo.tga");
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(),    (size_t)1);
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)0);
    // Unpin.
    ASSERT_TRUE(Store::Instance().TogglePin(L"foo.tga"));
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(),    (size_t)0);
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(recents.size(), (size_t)1);
    ASSERT_STREQ(recents[0].filename, L"foo.tga");
}

static void test_mod_switch_isolation()
{
    std::printf("test_mod_switch_isolation\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"a-foo.tga", SLOT_COLOR);
    Store::Instance().TouchRecent(L"a-bar.tga", SLOT_COLOR);
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)2);
    // Switch to ModB — should see empty palette.
    Store::Instance().SetActiveMod(L"C:\\Test\\ModB");
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)0);
    Store::Instance().TouchRecent(L"b-only.tga", SLOT_COLOR);
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)1);
    // Switch back to ModA.
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    auto aRecents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(aRecents.size(), (size_t)2);
    ASSERT_TRUE(ContainsFilename(aRecents, L"a-foo.tga"));
    ASSERT_TRUE(ContainsFilename(aRecents, L"a-bar.tga"));
    ASSERT_TRUE(!ContainsFilename(aRecents, L"b-only.tga"));
    // Switch back to ModB.
    Store::Instance().SetActiveMod(L"C:\\Test\\ModB");
    auto bRecents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(bRecents.size(), (size_t)1);
    ASSERT_STREQ(bRecents[0].filename, L"b-only.tga");
}

static void test_per_mod_filter_persists()
{
    std::printf("test_per_mod_filter_persists\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().SetActiveFilter(SLOT_BUMP);
    Store::Instance().SetActiveMod(L"C:\\Test\\ModB");
    ASSERT_EQ(Store::Instance().ActiveFilter(), SLOT_COLOR);   // Mod B default
    Store::Instance().SetActiveFilter(SLOT_BUMP);
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    ASSERT_EQ(Store::Instance().ActiveFilter(), SLOT_BUMP);    // Mod A's saved filter
}

static void test_case_insensitive_mod_path()
{
    std::printf("test_case_insensitive_mod_path\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\MyMod");
    Store::Instance().TouchRecent(L"x.tga", SLOT_COLOR);
    Store::Instance().SetActiveMod(L"c:\\test\\mymod");   // different casing
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)1);
    ASSERT_STREQ(Store::Instance().Recents(SLOT_COLOR)[0].filename, L"x.tga");
}

static void test_slot_filter()
{
    std::printf("test_slot_filter\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"color-only.tga", SLOT_COLOR);
    Store::Instance().TouchRecent(L"bump-only.tga",  SLOT_BUMP);
    Store::Instance().TouchRecent(L"both.tga",       SLOT_COLOR);
    Store::Instance().TouchRecent(L"both.tga",       SLOT_BUMP);
    auto color = Store::Instance().Recents(SLOT_COLOR);
    auto bump  = Store::Instance().Recents(SLOT_BUMP);
    // color: color-only + both
    ASSERT_TRUE(ContainsFilename(color, L"color-only.tga"));
    ASSERT_TRUE(!ContainsFilename(color, L"bump-only.tga"));
    ASSERT_TRUE(ContainsFilename(color, L"both.tga"));
    // bump: bump-only + both
    ASSERT_TRUE(!ContainsFilename(bump, L"color-only.tga"));
    ASSERT_TRUE(ContainsFilename(bump, L"bump-only.tga"));
    ASSERT_TRUE(ContainsFilename(bump, L"both.tga"));
}

static void test_clear_active_mod()
{
    std::printf("test_clear_active_mod\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"a.tga", SLOT_COLOR);
    Store::Instance().TogglePin(L"a.tga");
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(), (size_t)1);
    Store::Instance().ClearActiveMod();
    // Re-activate the same mod; should be empty (INI section was wiped).
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    ASSERT_EQ(Store::Instance().Pins(SLOT_COLOR).size(),    (size_t)0);
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)0);
}

static void test_malformed_filenames_rejected()
{
    std::printf("test_malformed_filenames_rejected\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"good.tga",       SLOT_COLOR);
    Store::Instance().TouchRecent(L"with=equals.tga", SLOT_COLOR);   // = is INI metachar
    Store::Instance().TouchRecent(L"with\nnewline",   SLOT_COLOR);   // newline
    Store::Instance().TouchRecent(L"",                SLOT_COLOR);   // empty
    auto recents = Store::Instance().Recents(SLOT_COLOR);
    ASSERT_EQ(recents.size(), (size_t)1);
    ASSERT_STREQ(recents[0].filename, L"good.tga");
}

static void test_empty_mod_path_is_noop()
{
    std::printf("test_empty_mod_path_is_noop\n");
    ResetState();
    // Set then clear via empty path.
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"a.tga", SLOT_COLOR);
    Store::Instance().SetActiveMod(L"");
    // Operations on no-active-mod are silently dropped.
    Store::Instance().TouchRecent(L"orphan.tga", SLOT_COLOR);
    ASSERT_TRUE(!Store::Instance().TogglePin(L"orphan.tga"));
    ASSERT_TRUE(Store::Instance().Recents(SLOT_COLOR).empty());
    // Reactivate ModA — its data should still be there.
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    ASSERT_EQ(Store::Instance().Recents(SLOT_COLOR).size(), (size_t)1);
}

static void test_seam_ini_lands_in_temp_dir()
{
    // Seam proof: the ini must physically exist under the ALO_PALETTE_DIR temp
    // dir. If PaletteDir() ever regresses to ignoring the env var, every other
    // test here would still pass (round-tripping through the REAL APPDATA store)
    // — this is the one assertion that catches that silently-dangerous state.
    std::printf("test_seam_ini_lands_in_temp_dir\n");
    ResetState();
    Store::Instance().SetActiveMod(L"C:\\Test\\ModA");
    Store::Instance().TouchRecent(L"seam-proof.tga", SLOT_COLOR);
    const std::wstring ini = g_paletteDir + L"\\texture-palettes.ini";
    ASSERT_TRUE(GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES);
}

// ---- Driver -------------------------------------------------------------

int main()
{
    if (!CreateTempPaletteDir())
    {
        std::fprintf(stderr, "Failed to create temp palette directory.\n");
        return 1;
    }
    if (!SetEnvironmentVariableW(L"ALO_PALETTE_DIR", g_paletteDir.c_str()))
    {
        std::fprintf(stderr, "Failed to set ALO_PALETTE_DIR.\n");
        CleanupTempPaletteDir();
        return 1;
    }
    std::printf("Using temp palette dir: %ls\n", g_paletteDir.c_str());

    // Run tests.
    test_cold_start_empty();
    test_touch_new_recent();
    test_touch_existing_bumps_order();
    test_touch_existing_with_different_slot();
    test_recent_eviction_at_cap();
    test_pin_from_recent();
    test_pin_overflow_rejected();
    test_unpin_returns_to_recent();
    test_mod_switch_isolation();
    test_per_mod_filter_persists();
    test_case_insensitive_mod_path();
    test_slot_filter();
    test_clear_active_mod();
    test_malformed_filenames_rejected();
    test_empty_mod_path_is_noop();
    test_seam_ini_lands_in_temp_dir();

    CleanupTempPaletteDir();

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
