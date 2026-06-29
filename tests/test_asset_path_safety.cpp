// Regression test for asset-name path-safety (src/AssetPathSafety.h, audit F-PATH).
//
// Untrusted .alo texture/shader names used to flow VERBATIM into
// new PhysicalFile(...) -> CreateFile. A crafted "\\attacker\share\x" leaks the
// user's NetNTLM hash over outbound SMB; "C:\..." opens an arbitrary local file;
// "..\..\x" escapes the mod root. IsSafeRelativeAssetName must FLAG these and
// SanitizeAssetName must reduce a flagged name to its bare basename, while
// leaving legitimate relative game-asset names (interior backslashes, dotted
// filenames) untouched. The newer fail-closed texture candidate helper must
// return no candidates for unsafe names. Header-only; see
// tests/build_test_asset_path_safety.bat.

#include "AssetPathSafety.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// An UNSAFE name: IsSafeRelativeAssetName==false AND SanitizeAssetName reduces it
// to the basename (everything after the last slash/backslash).
static void expectUnsafe(const std::string& n, const std::string& expectBase, const char* label)
{
    CHECK(!IsSafeRelativeAssetName(n), label);
    CHECK(SanitizeAssetName(n) == expectBase, label);
}

// A SAFE name: flagged safe AND passes through Sanitize unchanged.
static void expectSafe(const std::string& n, const char* label)
{
    CHECK(IsSafeRelativeAssetName(n), label);
    CHECK(SanitizeAssetName(n) == n, label);
}

int main()
{
    std::printf("test_asset_path_safety\n");

    // --- UNSAFE: reduced to basename ---
    expectUnsafe("..\\x",                 "x",        "..\\x  (leading parent-traversal)");
    expectUnsafe("x\\..\\y",              "y",        "x\\..\\y  (interior parent-traversal)");
    expectUnsafe("..",                    "..",       "..  (bare parent, no separator -> basename stays '..')");
    expectUnsafe("C:\\WINDOWS\\WIN.INI",  "WIN.INI",  "C:\\WINDOWS\\WIN.INI  (drive-absolute)");
    expectUnsafe("\\\\HOST\\SHARE\\X",    "X",        "\\\\HOST\\SHARE\\X  (UNC, no colon)");
    expectUnsafe("\\rooted",              "rooted",   "\\rooted  (root-relative)");
    expectUnsafe("/etc/x",                "x",        "/etc/x  (forward-slash rooted)");
    expectUnsafe("C:FOO",                 "FOO",      "C:FOO  (drive-RELATIVE, no slash -> opens vs C: CWD)");
    expectUnsafe("FOO.TGA:BAD",           "BAD",      "FOO.TGA:BAD  (NTFS alternate data stream)");

    // --- SAFE: unchanged ---
    expectSafe("FX\\FOO.TGA",  "FX\\FOO.TGA  (legit relative with interior backslash)");
    expectSafe("BAR.DDS",      "BAR.DDS  (plain relative filename)");
    expectSafe("A.B..C.TGA",   "A.B..C.TGA  (the '..' not bracketed by separators -> NOT flagged)");
    expectSafe("",             "empty string  (harmless)");

    // --- FAIL-CLOSED texture candidates ---
    {
        std::vector<std::string> c = SafeTextureCandidates("FX\\foo.tga");
        CHECK(c.size() == 4, "SafeTextureCandidates: safe relative texture yields four candidates");
        if (c.size() == 4)
        {
            CHECK(c[0] == "Data\\Art\\Textures\\FX\\foo.tga", "candidate[0] is texture-root original");
            CHECK(c[1] == "Data\\Art\\Textures\\FX\\foo.dds", "candidate[1] is texture-root .dds fallback");
            CHECK(c[2] == "FX\\foo.tga", "candidate[2] is bare original");
            CHECK(c[3] == "FX\\foo.dds", "candidate[3] is bare .dds fallback");
        }
    }
    CHECK(SafeTextureCandidates("..\\x").empty(),       "SafeTextureCandidates rejects parent traversal");
    CHECK(SafeTextureCandidates("C:\\x").empty(),       "SafeTextureCandidates rejects drive-absolute");
    CHECK(SafeTextureCandidates("\\\\unc\\x").empty(),  "SafeTextureCandidates rejects UNC");
    CHECK(SafeTextureCandidates("x.dds:ads").empty(),   "SafeTextureCandidates rejects ADS colon");
    CHECK(SafeTextureCandidates("ok/..\\x.tga").empty(), "SafeTextureCandidates rejects mixed-slash traversal");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
