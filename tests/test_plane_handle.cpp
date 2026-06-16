#include "../src/PlaneHandle.h"
#include <cstdio>
#include <cmath>
static int g_fail = 0;
static void ok(bool c, const char* m){ printf(c?"  ok: %s\n":"  FAIL: %s\n", m); if(!c) ++g_fail; }
static bool close(float a, float b, float e=1e-3f){ return std::fabs(a-b) <= e; }
int main(){
    // --- RayPlaneOffset: ground plane (normalAxis=2) through o; decompose into (X,Y).
    const float o[3] = {10.f, 20.f, 5.f};
    float u, v;
    // Straight-down ray over (13,24): hits ground at (13,24,5) -> offset (3,4).
    { const float P[3]={13.f,24.f,100.f}, d[3]={0.f,0.f,-1.f};
      ok(planehandle::RayPlaneOffset(P,d,o,2,u,v)&&close(u,3.f)&&close(v,4.f), "down-ray -> (3,4)"); }
    // Oblique ray (nonzero d[au],d[av]) -- the +d*t term must contribute; a down-ray
    // masks an index/sign bug there. P=(10,20,25),d=(2,3,-2): t=(5-25)/-2=10 -> (30,50,5).
    { const float P[3]={10.f,20.f,25.f}, d[3]={2.f,3.f,-2.f};
      ok(planehandle::RayPlaneOffset(P,d,o,2,u,v)&&close(u,20.f)&&close(v,30.f), "oblique ray decomposes"); }
    // Hit exactly at o -> (0,0).
    { const float P[3]={10.f,20.f,25.f}, d[3]={0.f,0.f,-1.f};
      ok(planehandle::RayPlaneOffset(P,d,o,2,u,v)&&close(u,0.f)&&close(v,0.f), "hit at o -> (0,0)"); }
    // Negative quadrant over (7,16) -> (-3,-4) (catches a flipped subtraction).
    { const float P[3]={7.f,16.f,25.f}, d[3]={0.f,0.f,-1.f};
      ok(planehandle::RayPlaneOffset(P,d,o,2,u,v)&&close(u,-3.f)&&close(v,-4.f), "negative quadrant"); }
    // General normalAxis=0 -> basis (Y,Z). P=(25,20,5),d=(-2,0,0) hits x=10 -> (0,0).
    { const float P[3]={25.f,20.f,5.f}, d[3]={-2.f,0.f,0.f};
      ok(planehandle::RayPlaneOffset(P,d,o,0,u,v)&&close(u,0.f)&&close(v,0.f), "normalAxis=0 basis (Y,Z)"); }
    // Grazing (parallel) and behind (t<0) -> false.
    { const float P[3]={13.f,24.f,100.f}, d[3]={1.f,0.f,0.f};
      ok(!planehandle::RayPlaneOffset(P,d,o,2,u,v), "grazing -> false"); }
    { const float P[3]={13.f,24.f,-100.f}, d[3]={0.f,0.f,-1.f};
      ok(!planehandle::RayPlaneOffset(P,d,o,2,u,v), "behind -> false"); }

    // --- HandleHit: square band [inner,outer]^2; centeredness score = 0.9*max(du,dv).
    const float inL = 3.f, outL = 9.f;   // mid=6, half=3
    float s;
    ok( planehandle::HandleHit(6.f,6.f,inL,outL,s)&&close(s,0.f),  "centre -> hit, score 0");
    ok( planehandle::HandleHit(3.f,6.f,inL,outL,s)&&close(s,0.9f), "on innerL edge -> hit, 0.9");
    ok( planehandle::HandleHit(9.f,9.f,inL,outL,s)&&close(s,0.9f), "on corner -> hit, 0.9");
    ok( planehandle::HandleHit(3.001f,6.f,inL,outL,s),            "just inside innerL -> hit");
    ok(!planehandle::HandleHit(2.999f,6.f,inL,outL,s),            "just outside innerL -> miss");
    ok(!planehandle::HandleHit(6.f,10.f,inL,outL,s),             "v>outer -> miss");
    // Chebyshev (max), NOT Euclidean: (7.5,6) and (7.5,7.5) both -> 0.9*0.5 = 0.45.
    ok( planehandle::HandleHit(7.5f,6.f, inL,outL,s)&&close(s,0.45f), "score 0.9*max(du,dv) [one axis]");
    ok( planehandle::HandleHit(7.5f,7.5f,inL,outL,s)&&close(s,0.45f), "score is Chebyshev, not radial");

    // --- ComposePlanePos: ground (normalAxis=2) -> Z invariant; X/Y take the accums.
    //     (This pins Risk 5: a plane drag never changes height.)
    { const float start[3]={10.f,20.f,5.f}; float out[3];
      planehandle::ComposePlanePos(start,2,3.f,-4.f,out);
      ok(close(out[0],13.f)&&close(out[1],16.f)&&close(out[2],5.f), "ComposePlanePos ground: Z invariant"); }
    { const float start[3]={10.f,20.f,5.f}; float out[3];
      planehandle::ComposePlanePos(start,0,3.f,-4.f,out);
      ok(close(out[0],10.f)&&close(out[1],23.f)&&close(out[2],1.f), "ComposePlanePos normalAxis=0 (Y,Z)"); }
    // normalAxis=1 -> basis au=(1+1)%3=2 (Z), av=(1+2)%3=0 (X): the only case where the
    // %3 wrap puts av<au -- catches a transposed/off-by-one index. accumU->Z, accumV->X.
    { const float start[3]={10.f,20.f,5.f}; float out[3];
      planehandle::ComposePlanePos(start,1,3.f,-4.f,out);
      ok(close(out[0],6.f)&&close(out[1],20.f)&&close(out[2],8.f), "ComposePlanePos normalAxis=1 (Z,X) wrap"); }

    // --- QuadCorners: ground square inner=3 outer=9 about o -> +X/+Y quadrant, Z=o.z.
    //     (Pins handle PLACEMENT without a render -- --capture can't show the gizmo.)
    { float c[4][3]; planehandle::QuadCorners(o,2,3.f,9.f,c);
      ok(close(c[0][0],13.f)&&close(c[0][1],23.f)&&close(c[0][2],5.f), "corner0 (in,in)");
      ok(close(c[2][0],19.f)&&close(c[2][1],29.f)&&close(c[2][2],5.f), "corner2 (out,out)");
      ok(close(c[1][0],19.f)&&close(c[1][1],23.f),                     "corner1 (out,in)");
      ok(close(c[3][0],13.f)&&close(c[3][1],29.f),                     "corner3 (in,out)"); }

    // --- Drag-frame STABILITY (regression for the plane-drag flicker). The per-move
    //     offset must be decomposed against the FIXED grab anchor, NOT the live (moving)
    //     object origin. If it uses the moving origin, the object's own motion feeds back
    //     into the next delta and the position oscillates frame-to-frame with a held cursor.
    //     Simulate several frames with the cursor parked over (13,24); grab was over (10,20).
    {
        const float start[3] = {10.f, 20.f, 5.f};                       // grab position == fixed anchor
        const float Pg[3]={10.f,20.f,100.f}, dg[3]={0.f,0.f,-1.f};      // grab-time cursor (over the object)
        const float Ph[3]={13.f,24.f,100.f}, dh[3]={0.f,0.f,-1.f};      // held cursor, parked at (13,24)
        // CORRECT flow: anchor stays `start` every frame.
        {
            float prevU,prevV; planehandle::RayPlaneOffset(Pg,dg,start,2,prevU,prevV);  // grab seed -> (0,0)
            float aU=0.f,aV=0.f, pos[3], last[3]={start[0],start[1],start[2]}; bool stable=true;
            for (int f=0; f<4; ++f) {
                float uN,vN; planehandle::RayPlaneOffset(Ph,dh,start,2,uN,vN);           // FIXED anchor
                aU+=(uN-prevU); aV+=(vN-prevV); prevU=uN; prevV=vN;
                planehandle::ComposePlanePos(start,2,aU,aV,pos);
                if (f>0 && !(close(pos[0],last[0])&&close(pos[1],last[1])&&close(pos[2],last[2]))) stable=false;
                last[0]=pos[0]; last[1]=pos[1]; last[2]=pos[2];
            }
            ok(stable, "fixed-anchor drag: held cursor -> stable position (no flicker)");
            ok(close(pos[0],13.f)&&close(pos[1],24.f)&&close(pos[2],5.f), "fixed-anchor drag: follows cursor, Z held");
        }
        // BUGGY flow (negative control): anchor = current object position, which moves each frame.
        {
            float cur[3]={start[0],start[1],start[2]};
            float prevU,prevV; planehandle::RayPlaneOffset(Pg,dg,cur,2,prevU,prevV);
            float aU=0.f,aV=0.f, pos[3], first[3]={0,0,0}; bool oscillates=false;
            for (int f=0; f<4; ++f) {
                float uN,vN; planehandle::RayPlaneOffset(Ph,dh,cur,2,uN,vN);             // MOVING origin
                aU+=(uN-prevU); aV+=(vN-prevV); prevU=uN; prevV=vN;
                planehandle::ComposePlanePos(start,2,aU,aV,pos);
                cur[0]=pos[0]; cur[1]=pos[1]; cur[2]=pos[2];                              // object moves -> origin moves
                if (f==0){first[0]=pos[0];first[1]=pos[1];}
                else if (!(close(pos[0],first[0])&&close(pos[1],first[1]))) oscillates=true;
            }
            ok(oscillates, "moving-origin drag DOES oscillate (negative control: proves the fix is needed)");
        }
    }
    printf(g_fail? "\n=== plane handle: %d FAIL ===\n":"\n=== plane handle: ALL PASS ===\n", g_fail);
    return g_fail?1:0;
}
