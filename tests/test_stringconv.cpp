// Unit tests for the host UTF-8 <-> UTF-16 helpers (src/host/StringConv.h),
// the pair consolidated from 3 copies by the DRY-audit cpp-host-0 work (#299).
// Pins the invariants that consolidation centralized: round-trip fidelity, the
// empty-string path (the HostBridgeProxy concern), and embedded-NUL handling
// (the helpers convert by .size(), NOT a NUL-terminated -1 length — so a string
// with an interior NUL survives, which a naive -1 conversion would truncate).
#include "../src/host/StringConv.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
static void ok(bool c, const char* m)
{
    printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m);
    if (!c) ++g_fail;
}

int main()
{
    using host::Utf8ToWide;
    using host::WideToUtf8;

    // --- empty-string paths (HostBridgeProxy's old srcLen==0 / res.empty() branches) ---
    ok(Utf8ToWide(std::string()).empty(),  "Utf8ToWide(\"\") -> empty wstring");
    ok(WideToUtf8(std::wstring()).empty(), "WideToUtf8(L\"\") -> empty string");

    // --- ASCII round-trip ---
    {
        const std::string s = "hello, world!";
        ok(WideToUtf8(Utf8ToWide(s)) == s, "ASCII round-trips u8->w->u8");
    }

    // --- multi-byte UTF-8 round-trip (é = C3 A9 [2B], € = E2 82 AC [3B]) ---
    {
        // Split the literal so the \xAC hex escape doesn't greedily eat the '5'.
        const std::string s = "caf\xC3\xA9 costs \xE2\x82\xAC" "5";
        const std::wstring w = Utf8ToWide(s);
        ok(!w.empty() && w.size() < s.size(), "multi-byte UTF-8 decodes to fewer wchars than bytes");
        ok(WideToUtf8(w) == s, "multi-byte UTF-8 round-trips byte-for-byte");
    }

    // --- embedded NUL: length-based conversion preserves interior NULs ---
    {
        const std::string s("a\0b", 3);  // 3 bytes, interior NUL
        const std::wstring w = Utf8ToWide(s);
        ok(w.size() == 3, "embedded NUL: 3 bytes -> 3 wchars (size-based, not -1)");
        const std::string back = WideToUtf8(w);
        ok(back.size() == 3 && back == s, "embedded NUL round-trips (interior NUL preserved)");
    }

    printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
