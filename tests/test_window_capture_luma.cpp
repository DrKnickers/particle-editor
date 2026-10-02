// Unit test for host::MaxLumaInRect (src/host/WindowCapture.cpp), the pure
// half of the --drive assert-viewport-nonblack probe (ProbeWindowMaxLuma =
// GrabWindowPixels + MaxLumaInRect). No window or GDI+ is involved.

#include "../src/host/WindowCapture.h"

#include <cstdio>
#include <vector>

static int g_fail = 0;
static void ok(bool c, const char* m)
{
    std::printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m);
    if (!c) ++g_fail;
}

int main()
{
    std::printf("test_window_capture_luma\n");

    // 10x4 black image (alpha left 0: it must not count) with a few lit pixels.
    const int w = 10, h = 4;
    std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4, 0);
    auto set = [&](int x, int y, unsigned char b, unsigned char g, unsigned char r) {
        unsigned char* p = px.data() + (static_cast<size_t>(y) * w + x) * 4;
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    };
    set(1, 1, 10, 20, 30);     // sum 60, left half
    set(8, 2, 255, 255, 255);  // sum 765, right half
    set(9, 3, 0, 0, 100);      // sum 100, bottom-right corner

    ok(host::MaxLumaInRect(px.data(), w, h, 0, 0, 1, 1) == 765,
       "whole image: the brightest pixel wins");
    ok(host::MaxLumaInRect(px.data(), w, h, 0, 0, 0.5, 1) == 60,
       "left half excludes the right-half pixels");
    ok(host::MaxLumaInRect(px.data(), w, h, 0.5, 0, 1, 0.5) == 0,
       "a lit-free region reads 0 (alpha is not counted)");
    ok(host::MaxLumaInRect(px.data(), w, h, 0.9, 0.75, 1, 1) == 100,
       "the corner region [0.9,0.75)-(1,1) holds only (9,3)");
    ok(host::MaxLumaInRect(px.data(), w, h, 0.8, 0.5, 0.9, 0.75) == 765,
       "a one-pixel region is half-open on its far edges");
    ok(host::MaxLumaInRect(px.data(), w, h, -1, -1, 2, 2) == 765,
       "out-of-range fractions clamp to the image");
    ok(host::MaxLumaInRect(px.data(), w, h, 0.5, 0.5, 0.5, 1) == -1,
       "a zero-width region is -1");
    ok(host::MaxLumaInRect(px.data(), w, h, 0.7, 0, 0.2, 1) == -1,
       "an inverted region is -1");
    ok(host::MaxLumaInRect(nullptr, w, h, 0, 0, 1, 1) == -1,
       "a null buffer is -1");
    ok(host::MaxLumaInRect(px.data(), 0, h, 0, 0, 1, 1) == -1,
       "an empty image is -1");

    std::printf("%s\n", g_fail ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_fail ? 1 : 0;
}
