// Unit test for src/SceneOverscan.h pure helper (Tranche D / finding #10).
// Header-only, no engine/Win32 deps — links only this TU.
#include <cstdio>
#include "SceneOverscan.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static bool eq(const SceneViewport& v, int x, int y, int w, int h)
{ return v.x == x && v.y == y && v.w == w && v.h == h; }

int main()
{
    // GBx floored at 12 when w/64 < 12. w=100 -> GBx=12; GBy=(12*200+50)/100=24.
    CHECK(eq(ComputeOverscanViewport(0, 0, 100, 200), -12, -24, 124, 248));

    // Large w -> GBx = w/64. w=1280 -> GBx=20; GBy=(20*720+640)/1280=11 (w/2 rounding, int trunc).
    CHECK(eq(ComputeOverscanViewport(10, 20, 1280, 720), -10, 9, 1320, 742));

    // w==0 guard -> GBy=GBx=12.
    CHECK(eq(ComputeOverscanViewport(5, 5, 0, 50), -7, -7, 24, 74));

    // Determinism: the SAME true rect (whether from apply or reemit) yields the
    // SAME engine viewport — the two LayoutBroker call sites can't diverge.
    // w=640 -> 640/64=10, which is NOT > 12, so GBx floors to 12; GBy=(12*480+320)/640=9.
    SceneViewport apply  = ComputeOverscanViewport(0, 0, 640, 480);
    SceneViewport reemit = ComputeOverscanViewport(0, 0, 640, 480);
    CHECK(eq(apply, -12, -9, 664, 498));
    CHECK(apply.x == reemit.x && apply.y == reemit.y && apply.w == reemit.w && apply.h == reemit.h);

    if (g_fail == 0) std::printf("=== SceneOverscan: ALL PASS ===\n");
    else             std::printf("=== SceneOverscan: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
