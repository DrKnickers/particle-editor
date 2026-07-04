// Unit tests for src/host/HeadlessComposite.h — host::CompositeUiOverEngine,
// the CPU straight-alpha composite for the headless --record path. Pure pixel
// math, no host / WebView2 / D3D9. The full path is covered end-to-end by
// scripts/clip-verify/headless-golden.mjs (real frames vs the legacy render).
#include <cstdio>
#include <cstdint>
#include <vector>
#include "HeadlessComposite.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// One BGRA pixel.
static std::vector<unsigned char> px(int b, int g, int r, int a) { return {(unsigned char)b,(unsigned char)g,(unsigned char)r,(unsigned char)a}; }

int main()
{
    using host::CompositeUiOverEngine;
    std::vector<unsigned char> out; int ow = 0, oh = 0;

    // A. Straight-alpha blend at a=128: out = (ui*128 + eng*127 + 127)/255.
    //    ui=(200,200,200,128) over eng=(0,0,0) -> (25600+0+127)/255 = 100.
    {
        auto ui = px(200,200,200,128), eng = px(0,0,0,255);
        CompositeUiOverEngine(ui,1,1, eng,1,1,0,0, out, ow, oh);
        CHECK(ow == 1 && oh == 1);
        CHECK(out[0] == 100 && out[1] == 100 && out[2] == 100);
    }

    // B. Fully-transparent UI (a=0) shows the engine exactly.
    {
        auto ui = px(50,60,70,0), eng = px(10,20,30,255);
        CompositeUiOverEngine(ui,1,1, eng,1,1,0,0, out, ow, oh);
        CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30);
    }

    // C. Fully-opaque UI (a=255) is untouched — engine hidden.
    {
        auto ui = px(111,122,133,255), eng = px(1,2,3,255);
        CompositeUiOverEngine(ui,1,1, eng,1,1,0,0, out, ow, oh);
        CHECK(out[0] == 111 && out[1] == 122 && out[2] == 133);
    }

    // D. Registration: a 2x2 engine placed at (3,3) in a 4x4 transparent UI.
    //    Only engine(0,0) lands in-bounds at UI(3,3); the other 3 clip (no OOB).
    {
        std::vector<unsigned char> ui(4*4*4, 0);          // all (0,0,0,a=0)
        std::vector<unsigned char> eng(2*2*4, 0);
        // engine(0,0) = (9,9,9); others distinct so a mis-place would show.
        eng[0]=9; eng[1]=9; eng[2]=9; eng[3]=255;
        eng[4]=1; eng[5]=1; eng[6]=1; eng[7]=255;         // (1,0) -> would land OOB
        CompositeUiOverEngine(ui,4,4, eng,2,2,3,3, out, ow, oh);
        CHECK(ow == 4 && oh == 4 && out.size() == 4*4*4u);
        const size_t p33 = (size_t)(3*4 + 3) * 4;         // UI(3,3)
        CHECK(out[p33] == 9 && out[p33+1] == 9 && out[p33+2] == 9);
        const size_t p00 = 0;                             // UI(0,0): no engine, stays ui
        CHECK(out[p00] == 0 && out[p00+1] == 0 && out[p00+2] == 0);
    }

    // E. Empty engine -> out == ui (dimensions preserved).
    {
        auto ui = px(7,8,9,0); std::vector<unsigned char> eng;
        CompositeUiOverEngine(ui,1,1, eng,0,0,0,0, out, ow, oh);
        CHECK(ow == 1 && oh == 1 && out[0] == 7 && out[1] == 8 && out[2] == 9);
    }

    // F. Negative offset clips the top-left of the engine (no OOB, no crash).
    {
        std::vector<unsigned char> ui(2*2*4, 0);
        std::vector<unsigned char> eng(2*2*4, 0);
        for (int i = 0; i < 4; ++i) { eng[i*4]=eng[i*4+1]=eng[i*4+2]=200; eng[i*4+3]=255; }
        // engine at (-1,-1): only engine(1,1) lands at UI(0,0).
        CompositeUiOverEngine(ui,2,2, eng,2,2,-1,-1, out, ow, oh);
        CHECK(out[0] == 200 && out[1] == 200 && out[2] == 200);   // UI(0,0)
        const size_t p11 = (size_t)(1*2 + 1) * 4;                 // UI(1,1): engine(2,2) OOB -> stays ui
        CHECK(out[p11] == 0);
    }

    if (g_fail == 0) std::printf("all headless-composite tests passed\n");
    else             std::printf("%d headless-composite test(s) FAILED\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
