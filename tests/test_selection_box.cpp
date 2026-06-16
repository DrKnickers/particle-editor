#include "../src/SelectionBoxStyle.h"
#include <cstdio>
#include <cmath>
using namespace selboxstyle;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); ++g_fail; } }while(0)

int main(){
    // Dashes: length 10, dash 2, gap 2 -> stride 4 -> dashes at 0,4,8 = 3 segs;
    // last dash clipped to the endpoint (8..10).
    {
        float a[3]={0,0,0}, b[3]={10,0,0}; std::vector<Seg> v;
        DashedSegments(a,b,2.0f,2.0f,v);
        CHECK(v.size()==3);
        CHECK(std::fabs(v[0].a[0]-0)<1e-4f && std::fabs(v[0].b[0]-2)<1e-4f);
        CHECK(std::fabs(v[2].a[0]-8)<1e-4f && std::fabs(v[2].b[0]-10)<1e-4f); // clipped
    }
    // Edge shorter than one dash -> single clipped dash.
    {
        float a[3]={0,0,0}, b[3]={1,0,0}; std::vector<Seg> v;
        DashedSegments(a,b,2.0f,2.0f,v);
        CHECK(v.size()==1 && std::fabs(v[0].b[0]-1)<1e-4f);
    }
    // Degenerate edge -> no segs.
    {
        float a[3]={2,2,2}, b[3]={2,2,2}; std::vector<Seg> v;
        DashedSegments(a,b,2.0f,2.0f,v);
        CHECK(v.empty());
    }
    // Corner brackets: 3 segs from c toward each neighbour, length 2 (clamped).
    {
        float c[3]={0,0,0}, n0[3]={5,0,0}, n1[3]={0,5,0}, n2[3]={0,0,5}; std::vector<Seg> v;
        CornerBracketSegs(c,n0,n1,n2,2.0f,v);
        CHECK(v.size()==3);
        CHECK(std::fabs(v[0].b[0]-2)<1e-4f);   // toward +X, length 2
        CHECK(std::fabs(v[1].b[1]-2)<1e-4f);   // toward +Y
        CHECK(std::fabs(v[2].b[2]-2)<1e-4f);   // toward +Z
    }
    // Negative gap -> touching dashes (stride == dashLen): 10 / dash 5 -> 2 segs, no gap.
    {
        float a[3]={0,0,0}, b[3]={10,0,0}; std::vector<Seg> v;
        DashedSegments(a,b,5.0f,-3.0f,v);
        CHECK(v.size()==2);
        CHECK(std::fabs(v[0].b[0]-5)<1e-4f && std::fabs(v[1].a[0]-5)<1e-4f); // touch at 5
    }
    // Corner bracket clamp: segLen longer than the edge -> clamped to the edge length.
    {
        float c[3]={0,0,0}, n0[3]={3,0,0}, n1[3]={0,3,0}, n2[3]={0,0,3}; std::vector<Seg> v;
        CornerBracketSegs(c,n0,n1,n2,10.0f,v);   // segLen 10 > edge 3
        CHECK(v.size()==3);
        CHECK(std::fabs(v[0].b[0]-3)<1e-4f);     // clamped to neighbour, not past it
    }
    if(g_fail){ printf("=== selection box: %d FAILED ===\n",g_fail); return 1; }
    printf("=== selection box: ALL PASS ===\n"); return 0;
}
