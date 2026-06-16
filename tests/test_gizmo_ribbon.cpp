#include "../src/GizmoRibbon.h"
#include <cstdio>
#include <cmath>
using namespace gizmoribbon;
static int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ printf("FAIL %s:%d  %s\n",__FILE__,__LINE__,#c); ++g_fail; } }while(0)
static bool finite3(const float v[3]){ return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]); }
static float dot(const float a[3],const float b[3]){ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }

int main(){
    // Normal case: segment along X, camera back on -Z. Width axis must be perpendicular
    // to BOTH the segment and the view ray, and width must scale with halfWidth.
    {
        float a[3]={0,0,0}, b[3]={10,0,0}, cam[3]={5,0,-20}, q[4][3];
        ExpandSegment(a,b,cam,2.0f,q);
        for(int i=0;i<4;++i) CHECK(finite3(q[i]));
        float w[3]={ q[1][0]-q[0][0], q[1][1]-q[0][1], q[1][2]-q[0][2] }; // 2*halfWidth vector
        float wlen=std::sqrt(dot(w,w));
        CHECK(std::fabs(wlen-4.0f) < 1e-3f);                 // |a+w - (a-w)| = 2*hw = 4
        float seg[3]={10,0,0};
        CHECK(std::fabs(dot(w,seg)) < 1e-3f);                // perpendicular to segment
        float view[3]={ 5-cam[0], 0-cam[1], 0-cam[2] };
        CHECK(std::fabs(dot(w,view)) < 1e-3f);               // perpendicular to view ray
        // q[2],q[3] must be at the b-end (b±w), not the a-end -> guards against a bowtie quad.
        auto distTo=[&](const float p[3], float X,float Y,float Z){ float dx=p[0]-X,dy=p[1]-Y,dz=p[2]-Z; return std::sqrt(dx*dx+dy*dy+dz*dz); };
        CHECK(std::fabs(distTo(q[2],10,0,0) - 2.0f) < 1e-3f);  // b + w
        CHECK(std::fabs(distTo(q[3],10,0,0) - 2.0f) < 1e-3f);  // b - w
        CHECK(distTo(q[0],10,0,0) > 9.0f);                     // q[0] is at the a-end, far from b
    }
    // Edge-on: segment points straight at the camera (dir parallel to view) -> guard.
    // a=(0,0,0) b=(0,0,1) cam=(0,0,-5): seg dir = (0,0,1).
    // Verify finite output AND that the width vector remains perpendicular to the segment.
    {
        float a[3]={0,0,0}, b[3]={0,0,1}, cam[3]={0,0,-5}, q[4][3];
        ExpandSegment(a,b,cam,1.0f,q);
        for(int i=0;i<4;++i) CHECK(finite3(q[i]));
        float w[3]={ q[1][0]-q[0][0], q[1][1]-q[0][1], q[1][2]-q[0][2] };
        float seg[3]={0,0,1}; // b - a
        CHECK(std::fabs(dot(w,seg)) < 1e-3f);  // width must still be perp to segment
    }
    // Zero-length: a==b, camera elsewhere -> guard, finite output.
    {
        float a[3]={3,3,3}, b[3]={3,3,3}, cam[3]={0,0,0}, q[4][3];
        ExpandSegment(a,b,cam,1.0f,q);
        for(int i=0;i<4;++i) CHECK(finite3(q[i]));
    }
    // Zero-length degenerate: a==b==camPos (worst case: no view ray at all) -> finite output.
    {
        float a[3]={3,3,3}, b[3]={3,3,3}, cam[3]={3,3,3}, q[4][3];
        ExpandSegment(a,b,cam,1.0f,q);
        for(int i=0;i<4;++i) CHECK(finite3(q[i]));
    }
    if(g_fail){ printf("=== gizmo ribbon: %d FAILED ===\n",g_fail); return 1; }
    printf("=== gizmo ribbon: ALL PASS ===\n"); return 0;
}
