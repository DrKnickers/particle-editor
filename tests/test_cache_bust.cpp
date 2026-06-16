// Unit tests for the app.local cache-bust URL helpers (src/host/CacheBust.h).
// Pure string logic — guards the empty-token (missing/unbuilt dist) fallback
// and the URL composition so a rebuilt bundle is always navigated fresh.
#include "../src/host/CacheBust.h"
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
    using host::CacheBustToken;
    using host::AppendCacheBustQuery;
    const std::wstring base = L"https://app.local/index.html";

    // --- CacheBustToken: hex formatting + non-positive fallback ---
    ok(CacheBustToken(0).empty(),               "ticks 0 -> empty (mtime unavailable)");
    ok(CacheBustToken(-12345).empty(),          "negative ticks -> empty");
    ok(CacheBustToken(255) == L"ff",            "255 -> 'ff'");
    ok(CacheBustToken(16) == L"10",             "16 -> '10'");
    ok(CacheBustToken(1) == L"1",               "1 -> '1'");
    // Multi-digit lowercase hex (0xABCDEF = 11259375) — guards casing + ordering.
    ok(CacheBustToken(11259375) == L"abcdef",   "11259375 -> 'abcdef' (lowercase, MSB first)");

    // --- AppendCacheBustQuery: empty token leaves the URL untouched ---
    ok(AppendCacheBustQuery(base, L"") == base,
       "empty token -> url unchanged (no trailing ?v=)");
    ok(AppendCacheBustQuery(base, L"ff") == base + L"?v=ff",
       "token -> appends ?v=<token>");

    // --- End-to-end: distinct builds bust, identical builds don't ---
    std::wstring a = AppendCacheBustQuery(base, CacheBustToken(1000));
    std::wstring b = AppendCacheBustQuery(base, CacheBustToken(2000));
    std::wstring a2 = AppendCacheBustQuery(base, CacheBustToken(1000));
    ok(a != b,  "different mtimes -> different nav URLs (rebuild busts)");
    ok(a == a2, "same mtime -> identical nav URL (relaunch stays cache-warm)");
    ok(a.rfind(base + L"?v=", 0) == 0, "nav URL keeps the app.local/index.html base");

    printf(g_fail ? "\n=== cache bust: %d FAIL ===\n" : "\n=== cache bust: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
