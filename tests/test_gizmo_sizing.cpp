#include "../src/GizmoSizing.h"
#include <cstdio>
#include <cmath>
#include <initializer_list>   // range-for over the braced {50,200,600} list (MSVC needs this header)
static int g_fail = 0;
static void ok(bool c, const char* m){ printf(c?"  ok: %s\n":"  FAIL: %s\n", m); if(!c) ++g_fail; }
static bool close(float a, float b, float e=0.01f){ return std::fabs(a-b) <= e*std::fabs(b)+1e-4f; }
int main(){
    const float fov45 = 0.785398163f; // 45deg in radians
    // 1) Calibration: at the default framing the new formula reproduces today's dist*0.12.
    for (float d : {50.f, 200.f, 600.f}) {
        float now = d * 0.12f;
        float nv  = GizmoHandleLengthWorld(d, fov45, 768, true, d);
        ok(close(nv, now), "kTarget reproduces dist*0.12 at H=768/fov45");
    }
    // 2) Absolute world length at hand-computed framings — INDEPENDENT of the formula's internal
    //    expression, so a wrong tan / viewport / factor term diverges (the old px-ratio test was a
    //    tautology: it divided the output by the same worldPerPixel the formula multiplies in).
    //    A: depth=200, fovY=45deg, H=768  -> 111.25 * 2*200*tan(22.5deg)/768 = 24.00 world units.
    ok(close(GizmoHandleLengthWorld(200.f, fov45, 768, true, 200.f), 24.00f, 0.005f),
       "world length matches hand value at depth=200/fov45/H=768");
    //    B: clamped-FoV regime, depth=100, fovY=120deg, H=1536 -> 111.25 * 2*100*tan(60deg)/1536 = 25.09.
    const float fov120 = 2.0943951f; // 120 degrees in radians (the projection's FoV clamp)
    ok(close(GizmoHandleLengthWorld(100.f, fov120, 1536, true, 100.f), 25.09f, 0.005f),
       "world length matches hand value in the clamped-FoV regime");
    // 3) Fallback fires (inactive / bad H / behind camera) -> eye-distance * 0.12.
    ok(close(GizmoHandleLengthWorld(200.f,fov45,768,false,300.f), 300.f*0.12f), "inactive -> eyeDist*0.12 fallback");
    ok(close(GizmoHandleLengthWorld(200.f,fov45,0,true,300.f),     300.f*0.12f), "H<=0 -> fallback");
    ok(close(GizmoHandleLengthWorld(-5.f,fov45,768,true,300.f),    300.f*0.12f), "depth<=0 -> fallback");
    printf(g_fail? "\n=== gizmo sizing: %d FAIL ===\n":"\n=== gizmo sizing: ALL PASS ===\n", g_fail);
    return g_fail?1:0;
}
