// Unit test for src/ModLayers.h pure helpers.
#include <cstdio>
#include <string>
#include <vector>
#include "ModLayers.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool veq(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b)
{ return a == b; }

int main()
{
    using namespace modlayers;

    // --- CanonicalizeLayerPath ---
    CHECK(CanonicalizeLayerPath(L"C:\\Mods\\Mod\\") == L"C:\\Mods\\Mod");
    CHECK(CanonicalizeLayerPath(L"C:\\Mods\\Mod//") == L"C:\\Mods\\Mod");
    CHECK(CanonicalizeLayerPath(L"C:\\Mods\\Mod  ") == L"C:\\Mods\\Mod");
    CHECK(CanonicalizeLayerPath(L"C:\\Mods\\Mod") == L"C:\\Mods\\Mod");

    // --- LayerPathsEqual (case- and slash-insensitive) ---
    CHECK(LayerPathsEqual(L"C:\\Mods\\Mod\\", L"c:\\mods\\Mod"));
    CHECK(!LayerPathsEqual(L"C:\\Mods\\Mod", L"C:\\Mods\\Mod"));

    // --- BuildContentRoots: existence filter + dedup + order + trailing slash ---
    auto existsAll = [](const std::wstring&) { return true; };
    {
        std::vector<std::wstring> got = BuildContentRoots(
            { L"C:\\m\\Mod", L"C:\\m\\Core", L"C:\\m" }, existsAll);
        CHECK(veq(got, { L"C:\\m\\Mod\\", L"C:\\m\\Core\\", L"C:\\m\\" }));
    }
    {
        // dedup case-insensitively, keep first occurrence/order; drop empties
        std::vector<std::wstring> got = BuildContentRoots(
            { L"C:\\m\\A", L"", L"c:\\m\\a\\", L"C:\\m\\B" }, existsAll);
        CHECK(veq(got, { L"C:\\m\\A\\", L"C:\\m\\B\\" }));
    }
    {
        // existence filter drops the missing one
        auto existsNoB = [](const std::wstring& p) { return p != L"C:\\m\\B"; };
        std::vector<std::wstring> got = BuildContentRoots(
            { L"C:\\m\\A", L"C:\\m\\B", L"C:\\m\\C" }, existsNoB);
        CHECK(veq(got, { L"C:\\m\\A\\", L"C:\\m\\C\\" }));
    }

    // --- MigrateLegacySelection ---
    CHECK(veq(MigrateLegacySelection(L"", {}, false), {}));          // Unmodded
    CHECK(veq(MigrateLegacySelection(L"C:\\m", {}, true),
              { L"C:\\m" }));                                        // mod only
    CHECK(veq(MigrateLegacySelection(L"C:\\m", { L"Mod", L"GCW" }, true),
              { L"C:\\m\\Mod", L"C:\\m\\GCW", L"C:\\m" }));          // submods front, root last
    // Core appended once for an un-migrated, non-empty, Core-less stack:
    CHECK(veq(MigrateLegacySelection(L"C:\\m", { L"Mod" }, false),
              { L"C:\\m\\Mod", L"C:\\m\\Core", L"C:\\m" }));
    // ...NOT appended when already migrated:
    CHECK(veq(MigrateLegacySelection(L"C:\\m", { L"Mod" }, true),
              { L"C:\\m\\Mod", L"C:\\m" }));
    // ...NOT appended when already named (any casing):
    CHECK(veq(MigrateLegacySelection(L"C:\\m", { L"Mod", L"Core" }, false),
              { L"C:\\m\\Mod", L"C:\\m\\Core", L"C:\\m" }));
    // ...NOT appended when the stack is empty:
    CHECK(veq(MigrateLegacySelection(L"C:\\m", {}, false), { L"C:\\m" }));
    // ...NOT appended when the stack has only empty-string names (no real submod emitted):
    CHECK(veq(MigrateLegacySelection(L"C:\\m", { L"" }, false), { L"C:\\m" }));

    // --- SerializeMultiSz / ParseMultiSz round-trip ---
    {
        std::vector<std::wstring> in = { L"C:\\a", L"C:\\b\\c" };
        std::wstring blob = SerializeMultiSz(in);
        std::vector<std::wstring> out = ParseMultiSz(blob.data(), blob.size());
        CHECK(veq(out, in));
        CHECK(veq(ParseMultiSz(SerializeMultiSz({}).data(),
                               SerializeMultiSz({}).size()), {}));   // empty list
    }
    {
        // long-path stack exceeding the old fixed 1024-wchar reader. 16 paths ×
        // (~83 + 1 null) + 1 ≈ 1345 wchars — genuinely past 1024 ELEMENTS (not just
        // bytes), which is what wstring::size() counts and what the old buffer held.
        std::vector<std::wstring> in;
        for (int i = 0; i < 16; ++i)
            in.push_back(std::wstring(L"C:\\GameLibrary\\steamapps\\common\\Example Strategy Game\\expansion\\Mods\\SampleMod\\Layer")
                         + std::to_wstring(i));
        std::wstring blob = SerializeMultiSz(in);
        CHECK(blob.size() > 1024);   // wchar count, not bytes
        CHECK(veq(ParseMultiSz(blob.data(), blob.size()), in));
    }

    if (g_fail == 0) std::printf("=== ModLayers: ALL PASS ===\n");
    else             std::printf("=== ModLayers: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
