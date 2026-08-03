#pragma once
#include <cmath>
// Pure, dependency-free camera-facing ribbon geometry for the gizmo
// aesthetics pass. Raw float[3] (xyz) so it unit-tests without D3DX / the engine;
// engine.cpp passes &vec.x (D3DXVECTOR3 is {float x,y,z}).
namespace gizmoribbon {

inline void  sub(const float a[3], const float b[3], float o[3]) { o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
inline float len(const float a[3]) { return std::sqrt(a[0]*a[0]+a[1]*a[1]+a[2]*a[2]); }
inline void  cross(const float a[3], const float b[3], float o[3]) {
    o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0];
}
// The world axis least aligned with `dir` -> a guaranteed non-parallel reference.
inline void leastAlignedAxis(const float dir[3], float o[3]) {
    const float ax=std::fabs(dir[0]), ay=std::fabs(dir[1]), az=std::fabs(dir[2]);
    o[0]=o[1]=o[2]=0.0f;
    if (ax<=ay && ax<=az) o[0]=1.0f; else if (ay<=az) o[1]=1.0f; else o[2]=1.0f;
}
// Expand world segment [a,b] into a camera-facing quad of half-width `hw`.
// out order: a-w, a+w, b+w, b-w (consistent winding). Robust to edge-on (segment
// parallel to the view ray) and zero-length (a==b) inputs via the least-aligned
// fallback -> never NaNs.
inline void ExpandSegment(const float a[3], const float b[3], const float camPos[3],
                          float hw, float out[4][3]) {
    float seg[3]; sub(b,a,seg);
    const float sl = len(seg);
    float dir[3];
    if (sl < 1e-6f) { float va[3]; sub(camPos,a,va); leastAlignedAxis(va,dir); }
    else            { dir[0]=seg[0]/sl; dir[1]=seg[1]/sl; dir[2]=seg[2]/sl; }
    const float mid[3] = { (a[0]+b[0])*0.5f, (a[1]+b[1])*0.5f, (a[2]+b[2])*0.5f };
    float view[3]; sub(mid,camPos,view); // view ray: camera toward segment midpoint
    float w[3]; cross(dir,view,w);
    float wl = len(w);
    if (wl < 1e-6f) { float ref[3]; leastAlignedAxis(dir,ref); cross(dir,ref,w); wl=len(w); }
    const float s = hw / wl;
    w[0]*=s; w[1]*=s; w[2]*=s;
    for (int i=0;i<3;++i) { out[0][i]=a[i]-w[i]; out[1][i]=a[i]+w[i]; out[2][i]=b[i]+w[i]; out[3][i]=b[i]-w[i]; }
}
} // namespace gizmoribbon
