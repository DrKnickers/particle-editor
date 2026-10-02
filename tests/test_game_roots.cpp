// Unit tests for src/GameRoots.h: the GameData / corruption leaf classification
// shared by the startup sibling-root lookup (main.cpp AddSiblingGamePath) and
// mod discovery (ModManager::DiscoverMods).

#include "GameRoots.h"

#include <cstdio>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using gameroots::RootFlavor;

int main()
{
    std::printf("test_game_roots\n");

    // Trailing separators of either kind are stripped; casing is kept.
    CHECK(gameroots::TrimTrailingSlashes(L"C:\\Games\\GameData\\\\/") == L"C:\\Games\\GameData",
          "TrimTrailingSlashes strips mixed trailing separators");
    CHECK(gameroots::TrimTrailingSlashes(L"") == L"", "TrimTrailingSlashes keeps empty empty");

    // Leaf classification is case-insensitive and ignores trailing slashes.
    CHECK(gameroots::ClassifyGameRoot(L"D:\\Steam\\EaW\\corruption") == RootFlavor::FoC,
          "corruption is FoC");
    CHECK(gameroots::ClassifyGameRoot(L"D:\\Steam\\EaW\\CORRUPTION\\") == RootFlavor::FoC,
          "CORRUPTION\\ is FoC (case + trailing slash)");
    CHECK(gameroots::ClassifyGameRoot(L"D:/Steam/EaW/gamedata/") == RootFlavor::BaseGame,
          "gamedata/ is the base game (forward slashes)");
    CHECK(gameroots::ClassifyGameRoot(L"D:\\Steam\\EaW\\Mods") == RootFlavor::Other,
          "an unrelated leaf is neither");
    CHECK(gameroots::ClassifyGameRoot(L"GameData") == RootFlavor::BaseGame,
          "a bare leaf with no parent still classifies");
    CHECK(gameroots::ClassifyGameRoot(L"D:\\corruption2") == RootFlavor::Other,
          "a leaf that merely starts with corruption is neither");

    // Sibling lookup swaps the leaf under the same parent.
    CHECK(gameroots::SiblingGameRoot(L"D:\\Steam\\EaW\\corruption\\") == L"D:\\Steam\\EaW\\GameData",
          "corruption's sibling is GameData");
    CHECK(gameroots::SiblingGameRoot(L"D:\\Steam\\EaW\\GameData") == L"D:\\Steam\\EaW\\corruption",
          "GameData's sibling is corruption");
    CHECK(gameroots::SiblingGameRoot(L"D:\\Steam\\EaW\\Other").empty(),
          "an unrelated root has no sibling");
    CHECK(gameroots::SiblingGameRoot(L"GameData").empty(),
          "a root with no parent folder has no sibling");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_failed ? 1 : 0;
}
