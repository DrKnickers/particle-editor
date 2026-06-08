#ifndef MOUSECURSOR_H
#define MOUSECURSOR_H

// Cursor-bound particle anchor used by the shift-click-to-spawn feature.
// `MouseCursor` is an `Object3D` whose position is driven from screen-space
// mouse moves (via `GetCursorPos3D` unproject + z=0 plane intersection)
// and whose velocity is derived from `QueryPerformanceCounter` deltas in
// `UpdateVelocity()` (called once per frame from the render loop).
//
// Both `--legacy-ui` and `--new-ui` use this header. Factored out of
// `src/main.cpp` (was a TU-local static class) so the host's
// `ViewportWndProc` can wire the same object into `Engine::SpawnParticleSystem`.
// Class is small + entirely inline; no .cpp.

#include "engine.h"

class MouseCursor : public Object3D
{
    D3DXVECTOR3   m_oldPosition;
    LARGE_INTEGER m_updated;
    LARGE_INTEGER m_frequency;

public:
    void SetPosition(const D3DXVECTOR3& position)
    {
        m_position = position;
    }

    void UpdateVelocity()
    {
        LARGE_INTEGER time;
        QueryPerformanceCounter(&time);

        D3DXVECTOR3 dx = m_position - m_oldPosition;
        float       dt = (float)(time.QuadPart - m_updated.QuadPart) / (float)m_frequency.QuadPart;
        m_velocity = dx / dt;

        m_oldPosition = m_position;
        m_updated     = time;
    }

    MouseCursor() : Object3D(NULL, D3DXVECTOR3(0,0,0))
    {
        QueryPerformanceFrequency(&m_frequency);
        m_oldPosition = D3DXVECTOR3(0,0,0);
    }
};

// Calculates the 3D position of the intersection of the cursor with Z = 0.
// Unproject the screen-space (x, y) at two depths (near + far) to get a
// world-space ray, then intersect with the z=0 plane. Used by:
//   - legacy WM_MOUSEMOVE / WM_KEYDOWN VK_SHIFT in src/main.cpp,
//   - new-UI host's ViewportWndProc.
//
// / handoff item 14] Under architecture C, the engine renders into
// a SCENE sub-rect of the popup HWND and m_projection is built at
// scene-rect aspect (per-pixel FoV referenced to scene-H, see
// src/engine.cpp:1540 SetSceneViewport). But Engine::Render restores the
// D3D9 device viewport to FULL-RT before returning (src/engine.cpp:687-
// 699), so reading the device viewport here produces a viewport /
// projection mismatch — D3DXVec3Unproject normalises (x - 0) / RT_W to
// NDC and feeds it into a projection that expected (x - sceneX) /
// sceneW. The world ray lands at the wrong NDC point, off by the
// scene-rect offset. Symptom: cursor-bound particles spawn visibly
// offset from the click point.
//
// Fix: when the engine reports an active scene viewport, build a
// D3DVIEWPORT9 from that rect and pass it to D3DXVec3Unproject. The
// function subtracts viewport.X / viewport.Y internally so the input
// (x, y) stays in popup-client coords — no caller-side translate.
// Architecture A (legacy popup) never activates the scene viewport
// (LayoutBroker's SetSceneViewport call is composition-only), so
// GetSceneViewport returns false and the device-viewport fallback runs
// unchanged.
inline void GetCursorPos3D(Engine* engine, short x, short y, D3DXVECTOR3& position)
{
    D3DXVECTOR3  front, back;
    D3DVIEWPORT9 viewport;
    D3DXMATRIX   world;
    D3DXMatrixIdentity(&world);

    int sx, sy, sw, sh;
    if (engine->GetSceneViewport(sx, sy, sw, sh))
    {
        viewport.X      = static_cast<DWORD>(sx);
        viewport.Y      = static_cast<DWORD>(sy);
        viewport.Width  = static_cast<DWORD>(sw);
        viewport.Height = static_cast<DWORD>(sh);
        viewport.MinZ   = 0.0f;
        viewport.MaxZ   = 1.0f;
    }
    else
    {
        engine->GetViewPort(&viewport);
    }

    D3DXVec3Unproject(&front, &D3DXVECTOR3(x, y, 0.0f), &viewport, &engine->GetProjectionMatrix(), &engine->GetViewMatrix(), &world);
    D3DXVec3Unproject(&back,  &D3DXVECTOR3(x, y, 0.9f), &viewport, &engine->GetProjectionMatrix(), &engine->GetViewMatrix(), &world);

    D3DXPLANE plane(0,0,1,0);
    D3DXPlaneIntersectLine(&position, &plane, &front, &back);
}

#endif // MOUSECURSOR_H
