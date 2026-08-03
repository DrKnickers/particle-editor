#pragma once
#include <d3dx9math.h>
// Pure helpers for the in-viewport gizmo readout pill. Projection +
// label maps live here (not in the engine) so the math is unit-tested headlessly
// and the engine/host CALL it (the GizmoSizing.h / PlaneHandle.h / RefLock.h precedent).
namespace manipreadout {

struct ViewportPoint { float nx, ny; bool visible; };

// World -> NORMALIZED viewport coords in [0,1], origin TOP-LEFT. viewProj is the
// combined view*projection. vpW/vpH are the scene-viewport size (only their ratio
// via the matrix matters here; passed for clarity / possible pixel use). visible
// is false when the point is at/behind the camera (clip w <= 0). nx/ny may fall
// outside [0,1] for in-front off-screen points (caller clamps).
inline ViewportPoint ProjectToViewport(const D3DXVECTOR3& world,
                                       const D3DXMATRIX& viewProj,
                                       int /*vpW*/, int /*vpH*/)
{
    D3DXVECTOR4 clip;
    D3DXVec3Transform(&clip, &world, &viewProj);   // [x y z w] = (world,1) * viewProj
    if (clip.w <= 1e-6f) return { 0.f, 0.f, false };
    const float ndcX = clip.x / clip.w;            // [-1,1]
    const float ndcY = clip.y / clip.w;            // [-1,1], +Y = up
    return { ndcX * 0.5f + 0.5f, 1.0f - (ndcY * 0.5f + 0.5f), true };
}

// Ring world axis (0=X,1=Y,2=Z) -> the m_referenceRotation index storing the
// rotation ABOUT that axis. Engine is Z-up: rot=[.x=about Z, .y=about X, .z=about Y]
// (see the m_referenceRotation decl in engine.h). Mirrors the `comp` mapping in
// HostWindow.cpp's MANIPULATE ROTATE branch. Used to read the readout VALUE; the
// readout LABEL is the world axis itself (AxisName(ringAxis)), so a rotate pill
// reads e.g. "Z 45" = 45deg about Z.
inline int RingComp(int ringAxis) { return ringAxis == 2 ? 0 : ringAxis == 0 ? 1 : 2; }

inline const char* AxisName(int axis) { return axis == 0 ? "X" : axis == 1 ? "Y" : "Z"; }

// Plane normal axis -> the two in-plane axes. Ground plane normal Z -> (X,Y).
inline void InPlaneAxes(int normal, int& u, int& v)
{ if (normal == 2) { u = 0; v = 1; } else if (normal == 0) { u = 1; v = 2; } else { u = 0; v = 2; } }

} // namespace manipreadout
