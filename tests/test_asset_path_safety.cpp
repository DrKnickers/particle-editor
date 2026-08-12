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

    // --- UNSAFE: Win32 strips trailing dots/spaces, so these normalize to ".." ---
    // An exact-".." comparison misses every one of these; a third-party .alo's
    // texture name reaches a real file-open sink, so the whole dots-and-spaces
    // class is rejected rather than enumerated.
    expectUnsafe(".. \\x",                "x",        ".. \\x  (dot-dot-SPACE -> Win32 strips the space)");
    expectUnsafe("x\\.. \\y",             "y",        "x\\.. \\y  (interior dot-dot-space)");
    expectUnsafe("..\\x",                 "x",        "..\\x  (plain parent, still rejected)");
    expectUnsafe("...\\x",                "x",        "...\\x  (three dots)");
    expectUnsafe(". .\\x",                "x",        ". .\\x  (dot-space-dot)");
    expectUnsafe("..  ",                  "..  ",     "..  (trailing spaces, no separator)");

    // --- SAFE: a lone "." is the CURRENT directory, not traversal ---
    // Rejecting these would not just refuse the name, it would push it through
    // SanitizeAssetName's basename reduction and silently load a DIFFERENT file
    // (FX\.\FIRE.TGA -> FIRE.TGA). A silent misresolution for existing mods is
    // worse than the traversal the guard exists to stop.
    expectSafe("FX\\.\\FIRE.TGA",   "FX\\.\\FIRE.TGA  (current-dir segment is legitimate)");
    expectSafe(".\\FIRE.TGA",       ".\\FIRE.TGA  (leading current-dir segment)");
    expectSafe("FX\\. \\FIRE.TGA",  "FX\\. \\FIRE.TGA  (dot-space normalizes to current dir)");

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

    // --- Operator-supplied custom slot paths ---
    //
    // A DIFFERENT rule from everything above: here an absolute local path is
    // the legitimate case, so the tests below are mostly about NOT rejecting.
    // Only a path pointing at another machine is refused.
    {
        // REJECT: the leak. A UNC path makes CreateFile authenticate outbound
        // over SMB and hand the user's NetNTLM hash to whoever runs that host.
        CHECK(!IsLocalCustomAssetPath(L"\\\\attacker\\share\\x.dds"),
              "custom path: UNC \\\\attacker\\share rejected (the NetNTLM leak)");
        CHECK(!IsLocalCustomAssetPath(L"//attacker/share/x.dds"),
              "custom path: forward-slash UNC rejected (same path, other spelling)");
        CHECK(!IsLocalCustomAssetPath(L"\\/attacker\\share"),
              "custom path: mixed-separator UNC rejected");
        CHECK(!IsLocalCustomAssetPath(L"\\\\?\\UNC\\attacker\\share\\x.dds"),
              "custom path: extended-length UNC rejected");
        CHECK(!IsLocalCustomAssetPath(L"\\\\.\\PhysicalDrive0"),
              "custom path: device namespace rejected");
        CHECK(!IsLocalCustomAssetPath(L"\\\\localhost.attacker\\share\\x.dds"),
              "custom path: localhost prefix spoof remains remote");
        CHECK(!IsLocalCustomAssetPath(L"\\\\127.0.0.1.attacker\\share\\x.dds"),
              "custom path: loopback prefix spoof remains remote");
        CHECK(!IsLocalCustomAssetPath(L"\\\\wsl.localhost.attacker\\share\\x.dds"),
              "custom path: WSL prefix spoof remains remote");

        // ACCEPT: the overreach guards. Each of these is a path a user really
        // supplies; rejecting any of them breaks the feature outright, which is
        // worse than the defect. An "always false" predicate fails here and
        // ONLY here.
        CHECK(IsLocalCustomAssetPath(L"C:\\textures\\grass.dds"),
              "custom path: ordinary drive-absolute accepted (overreach guard)");
        CHECK(IsLocalCustomAssetPath(L"D:/mods/mymod/ground.tga"),
              "custom path: forward-slash drive-absolute accepted");
        CHECK(IsLocalCustomAssetPath(L"C:\\Users\\me\\My Textures\\a b.dds"),
              "custom path: spaces in path accepted");
        CHECK(IsLocalCustomAssetPath(L"\\textures\\grass.dds"),
              "custom path: SINGLE leading separator is drive-relative, still local -> accepted");
        CHECK(IsLocalCustomAssetPath(L"textures\\grass.dds"),
              "custom path: plain relative accepted");
        CHECK(IsLocalCustomAssetPath(L""),
              "custom path: empty accepted (this is how a slot is CLEARED)");
        CHECK(IsLocalCustomAssetPath(L"\\\\localhost\\share\\x.dds"),
              "custom path: localhost UNC accepted");
        CHECK(IsLocalCustomAssetPath(L"//LOCALHOST/share/x.dds"),
              "custom path: localhost UNC is case-insensitive and separator-agnostic");
        CHECK(IsLocalCustomAssetPath(L"\\\\127.0.0.1\\share\\x.dds"),
              "custom path: IPv4 loopback UNC accepted");
        CHECK(IsLocalCustomAssetPath(L"\\\\wsl.localhost\\Ubuntu\\x.dds"),
              "custom path: WSL pseudo-UNC accepted");

        // Boundary: a lone separator must not be read as the start of a UNC
        // pair by an over-eager two-character check on a one-character string.
        CHECK(IsLocalCustomAssetPath(L"\\"),
              "custom path: lone separator accepted (no out-of-range read)");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
