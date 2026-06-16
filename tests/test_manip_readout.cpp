#include "../src/ManipReadout.h"
#include <cstdio>
#include <cmath>
#include <string>
static int g_fail = 0;
static void ok(bool c, const char* m){ printf(c?"  ok: %s\n":"  FAIL: %s\n", m); if(!c) ++g_fail; }
static bool close(float a, float b, float e=0.005f){ return std::fabs(a-b) <= e; }

int main(){
    using namespace manipreadout;
    // --- Index map (the Z-up trap: ring axis -> the m_referenceRotation index storing
    //     the rotation ABOUT that axis; rot=[.x=about Z, .y=about X, .z=about Y]) ---
    ok(RingComp(2)==0, "ring Z -> rot index 0 (.x = about Z)");
    ok(RingComp(0)==1, "ring X -> rot index 1 (.y = about X)");
    ok(RingComp(1)==2, "ring Y -> rot index 2 (.z = about Y)");
    ok(std::string(AxisName(0))=="X" && std::string(AxisName(1))=="Y" && std::string(AxisName(2))=="Z", "axis names");
    int u,v; InPlaneAxes(2,u,v); ok(u==0 && v==1, "ground plane (normal Z) -> u=X,v=Y");

    // --- Projection: identity viewProj so clip == (world,1); origin -> center ---
    D3DXMATRIX I; D3DXMatrixIdentity(&I);
    ViewportPoint c = ProjectToViewport(D3DXVECTOR3(0,0,0), I, 800, 600);
    ok(c.visible && close(c.nx,0.5f) && close(c.ny,0.5f), "origin -> viewport center");
    ViewportPoint q = ProjectToViewport(D3DXVECTOR3(0.5f,0.5f,0), I, 800, 600);
    ok(q.visible && q.nx>0.5f && q.ny<0.5f, "+x+y clip -> right & up (Y flipped)");
    D3DXMATRIX behind = I; behind._44 = 0.0f; // w = 0 for the origin point
    ViewportPoint b = ProjectToViewport(D3DXVECTOR3(0,0,0), behind, 800, 600);
    ok(!b.visible, "w<=0 -> not visible");

    // Perspective DIVIDE: a matrix giving clip.w=2 for the test point must HALVE the
    // NDC (catches a dropped /w that the identity cases (w==1) can't). For world
    // (1,0,0) with identity-but-_44=2: clip=(1,0,0,2) -> ndc=(0.5,0) -> nx=0.75, ny=0.5.
    D3DXMATRIX wdiv = I; wdiv._44 = 2.0f;
    ViewportPoint p = ProjectToViewport(D3DXVECTOR3(1,0,0), wdiv, 800, 600);
    ok(p.visible && close(p.nx,0.75f) && close(p.ny,0.5f), "perspective w-divide halves NDC");

    printf(g_fail? "\n=== manip readout: %d FAIL ===\n":"\n=== manip readout: ALL PASS ===\n", g_fail);
    return g_fail?1:0;
}
