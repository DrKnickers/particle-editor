// Unit tests for the shared GDI+ image-encoder helpers (src/host/GdiplusEncode.h),
// consolidated from 4 CLSID lookups + 2 byte-identical Base64Encode copies by
// the DRY-audit cpp-host-1 work. Pins:
//   - Base64Encode against the RFC 4648 §10 known-answer vectors, which exercise
//     all three padding tails (0/1/2 trailing bytes) — the part most likely to
//     regress in a hand-rolled encoder.
//   - GdiplusEncoderClsid: png/jpeg resolve to distinct CLSIDs, the per-MIME
//     cache returns the same CLSID on a repeat call, and a bogus MIME fails soft
//     (false, out untouched) rather than throwing.
#include "../src/host/GdiplusEncode.h"
#include <gdiplus.h>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "gdiplus.lib")

static int g_fail = 0;
static void ok(bool c, const char* m)
{
    printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m);
    if (!c) ++g_fail;
}

static std::string b64(const char* s)
{
    return host::Base64Encode(reinterpret_cast<const uint8_t*>(s), std::strlen(s));
}

int main()
{
    // --- Base64: RFC 4648 §10 test vectors (every padding tail) ---
    ok(b64("")       == "",          "\"\" -> \"\"");
    ok(b64("f")      == "Zg==",      "\"f\" -> Zg== (2 pad)");
    ok(b64("fo")     == "Zm8=",      "\"fo\" -> Zm8= (1 pad)");
    ok(b64("foo")    == "Zm9v",      "\"foo\" -> Zm9v (no pad)");
    ok(b64("foob")   == "Zm9vYg==",  "\"foob\" -> Zm9vYg==");
    ok(b64("fooba")  == "Zm9vYmE=",  "\"fooba\" -> Zm9vYmE=");
    ok(b64("foobar") == "Zm9vYmFy",  "\"foobar\" -> Zm9vYmFy");
    // A byte that sets the high bit — guards signed-char sloppiness.
    {
        const uint8_t hi[] = { 0xFF, 0x00, 0x80 };
        ok(host::Base64Encode(hi, 3) == "/wCA", "high-bit bytes encode unsigned");
    }

    // --- CLSID lookup (needs GDI+ initialized) ---
    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gdiIn, nullptr);

    CLSID png = {}, jpeg = {};
    ok(host::GdiplusEncoderClsid(L"image/png",  png),  "image/png encoder found");
    ok(host::GdiplusEncoderClsid(L"image/jpeg", jpeg), "image/jpeg encoder found");
    ok(png != jpeg, "png and jpeg resolve to distinct CLSIDs");

    // Unknown MIME fails soft: returns false AND leaves `out` untouched (the
    // documented contract). Seed a sentinel and assert it survives.
    const CLSID kSentinel = { 0xDEADBEEF, 0x1234, 0x5678,
                              { 1, 2, 3, 4, 5, 6, 7, 8 } };
    CLSID bogus = kSentinel;
    ok(!host::GdiplusEncoderClsid(L"image/does-not-exist", bogus),
       "unknown MIME -> false (fail soft)");
    ok(bogus == kSentinel, "unknown MIME leaves out untouched");

    CLSID png2 = {};
    ok(host::GdiplusEncoderClsid(L"image/png", png2) && png2 == png,
       "cached repeat lookup returns the same CLSID");

    Gdiplus::GdiplusShutdown(token);

    printf(g_fail ? "\nFAILED (%d)\n" : "\nPASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
