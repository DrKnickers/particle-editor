#include "../src/RingFade.h"
#include <cstdio>
#include <cmath>
using namespace ringfade;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); ++g_fail; } }while(0)

int main(){
    const float center[3]={0,0,0}, cam[3]={0,-5,0}, BACK=0.16f;
    // Near-camera point (toward cam) -> ~1.0 ; far point -> ~BACK.
    {
        float nearP[3]={0,-1,0}, farP[3]={0,1,0};
        CHECK(std::fabs(FacingAlpha(nearP,center,cam,BACK) - 1.0f) < 1e-3f);
        CHECK(std::fabs(FacingAlpha(farP,center,cam,BACK)  - BACK) < 1e-3f);
    }
    // Mirror-symmetric across the facing axis: left/right points equidistant from the
    // facing direction get equal alpha. (NOT front/back symmetry.)
    {
        // Both in the far hemisphere (sin>0), equal facing, opposite sides -> non-trivial equal alpha.
        const float s=(float)std::sin(0.7853981); // sin(pi/4)
        float L[3]={-s, s, 0}, R[3]={ s, s, 0};
        CHECK(std::fabs(FacingAlpha(L,center,cam,BACK) - FacingAlpha(R,center,cam,BACK)) < 1e-4f);
        CHECK(FacingAlpha(L,center,cam,BACK) > BACK+1e-3f && FacingAlpha(L,center,cam,BACK) < 1.0f-1e-3f);
    }
    // Sweep far-pole (θ=π/2, alpha=BACK) -> near-pole (θ=-π/2, alpha=1), passing the
    // far hemisphere where the fade actually ramps. Alpha must rise monotonically.
    {
        float prev=-1.0f;
        for(int i=0;i<=12;++i){ double th=1.5707963 - 3.14159265*i/12.0; // +pi/2 -> -pi/2
            float p[3]={ (float)std::cos(th), (float)std::sin(th), 0 };
            float al=FacingAlpha(p,center,cam,BACK);
            CHECK(al>=BACK-1e-4f && al<=1.0f+1e-4f);
            CHECK(al >= prev-1e-4f); prev=al; }
    }
    // The ramp is non-trivial: a mid-far-hemisphere point sits strictly between BACK and 1.
    {
        float midFar[3]={ (float)std::cos(1.5707963/2), (float)std::sin(1.5707963/2), 0 }; // θ=pi/4
        float al=FacingAlpha(midFar,center,cam,BACK);
        CHECK(al > BACK+1e-3f && al < 1.0f-1e-3f);
    }
    if(g_fail){ printf("=== ring fade: %d FAILED ===\n",g_fail); return 1; }
    printf("=== ring fade: ALL PASS ===\n"); return 0;
}
