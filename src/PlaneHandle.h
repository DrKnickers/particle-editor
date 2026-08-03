#pragma once
#include <cmath>
// Pure, dependency-free helpers for the reference-object gizmo's ground-plane
// handle (batch 2). Raw float[3] (xyz) so this unit-tests without D3DX /
// the engine; engine.cpp passes &vec.x (D3DXVECTOR3 is {float x,y,z}).
namespace planehandle {

// Intersect the ray (origin P, direction d -- need not be normalized) with the
// axis-aligned plane through `o` whose normal is world axis `normalAxis` (0/1/2),
// then decompose (hit - o) into that plane's (normalAxis+1, normalAxis+2) world-
// axis basis -> (outU, outV). Returns false when the ray is ~parallel to the
// plane (no intersection) or the plane is behind the ray (t < 0). Mirrors the
// engine's rotate-ring intersect (engine.cpp ManipulatorRingAngle).
inline bool RayPlaneOffset(const float P[3], const float d[3], const float o[3],
                           int normalAxis, float& outU, float& outV)
{
    const int n = normalAxis;
    const float denom = d[n];
    if (std::fabs(denom) < 1e-4f) return false;       // grazing
    const float t = (o[n] - P[n]) / denom;
    if (t < 0.0f) return false;                       // behind the ray
    const int au = (n + 1) % 3, av = (n + 2) % 3;
    outU = (P[au] + d[au] * t) - o[au];
    outV = (P[av] + d[av] * t) - o[av];
    return true;
}

// True when the in-plane offset (u,v) lies inside the square band
// [innerL, outerL] on BOTH axes (the +U/+V quadrant). outScore is a "centeredness"
// metric: 0 at the square's centre, ~0.9 at its edge -- so a solid plane click
// scores below a grazing arrow/ring (which score miss/threshold), letting the
// plane win a clear hit while genuine axis ties stay with the arrows.
inline bool HandleHit(float u, float v, float innerL, float outerL, float& outScore)
{
    if (u < innerL || u > outerL || v < innerL || v > outerL) return false;
    const float mid = 0.5f * (innerL + outerL);
    const float half = 0.5f * (outerL - innerL);
    const float du = std::fabs(u - mid) / half;
    const float dv = std::fabs(v - mid) / half;
    outScore = 0.9f * (du > dv ? du : dv);
    return true;
}

// New world position = grab-time start + accumulated in-plane offsets, in the
// (normalAxis+1, normalAxis+2) basis. For the ground plane (normalAxis=2) the basis
// is (X,Y), so out[2] (Z/height) == start[2] EXACTLY -- a plane drag never changes
// height. The host calls this so its drag math is the unit-tested math.
inline void ComposePlanePos(const float start[3], int normalAxis,
                            float accumU, float accumV, float out[3])
{
    const int au = (normalAxis + 1) % 3, av = (normalAxis + 2) % 3;
    out[0] = start[0]; out[1] = start[1]; out[2] = start[2];
    out[au] += accumU;
    out[av] += accumV;
}

// The 4 corners of the square handle in the plane through `o` with normal
// `normalAxis`, spanning [innerL, outerL] on each in-plane axis (+U/+V quadrant).
// Order: (in,in),(out,in),(out,out),(in,out) -> a quad loop. The engine render uses
// this so placement is the unit-tested math (--capture can't show the gizmo).
inline void QuadCorners(const float o[3], int normalAxis, float innerL, float outerL,
                        float corners[4][3])
{
    const int au = (normalAxis + 1) % 3, av = (normalAxis + 2) % 3;
    const float su[4] = { innerL, outerL, outerL, innerL };
    const float sv[4] = { innerL, innerL, outerL, outerL };
    for (int i = 0; i < 4; ++i) {
        corners[i][0] = o[0]; corners[i][1] = o[1]; corners[i][2] = o[2];
        corners[i][au] += su[i];
        corners[i][av] += sv[i];
    }
}

} // namespace planehandle
