// Unit test for the reference-object world matrix builder
// (src/ReferenceObjectWorld.h). Pure-header test, links only d3dx9.lib (the
// GizmoSizing.h / ManipReadout.h precedent) -- no engine TU.
//
// Proves the per-object <Scale_Factor> render multiplier is applied as a uniform
// scale ABOUT THE ORIGIN (leftmost, before rotation/translation): a basis vector
// maps to length sf, rotation stays length-preserving, the origin is fixed (so an
// off-origin mesh translates as it grows -- Risk-2), and translation is NOT scaled.
#include "../src/ReferenceObjectWorld.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void ok(bool c, const char* m) { printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m); if (!c) ++g_fail; }
static bool close(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

// Length of a DIRECTION (w=0) after the linear part of m -- ignores translation.
static float axisLen(const D3DXMATRIX& m, const D3DXVECTOR3& axis)
{
    D3DXVECTOR3 out;
    D3DXVec3TransformNormal(&out, &axis, &m);
    return D3DXVec3Length(&out);
}

int main()
{
    const D3DXVECTOR3 X(1, 0, 0), Y(0, 1, 0), Z(0, 0, 1);
    const D3DXVECTOR3 origin(0, 0, 0), noRot(0, 0, 0);

    // 1) Default (sf=1.0): identity scale -> every basis axis keeps length 1.
    {
        D3DXMATRIX m = ReferenceObjectWorldMatrix(origin, noRot, 1.0f);
        ok(close(axisLen(m, X), 1.0f), "sf=1.0 -> X axis length 1.0");
        ok(close(axisLen(m, Y), 1.0f), "sf=1.0 -> Y axis length 1.0");
        ok(close(axisLen(m, Z), 1.0f), "sf=1.0 -> Z axis length 1.0");
    }

    // 2) sf=1.5 (trooper) -> uniform 1.5x on every axis.
    {
        D3DXMATRIX m = ReferenceObjectWorldMatrix(origin, noRot, 1.5f);
        ok(close(axisLen(m, X), 1.5f), "sf=1.5 -> X axis length 1.5");
        ok(close(axisLen(m, Y), 1.5f), "sf=1.5 -> Y axis length 1.5");
        ok(close(axisLen(m, Z), 1.5f), "sf=1.5 -> Z axis length 1.5");
    }

    // 3) sf=1.8 (AT-AT).
    {
        D3DXMATRIX m = ReferenceObjectWorldMatrix(origin, noRot, 1.8f);
        ok(close(axisLen(m, X), 1.8f), "sf=1.8 -> X axis length 1.8");
    }

    // 4) Rotation is length-preserving at sf=1.0 (no smear), and uniform scale
    //    commutes with rotation for length at sf=2.0.
    {
        const D3DXVECTOR3 rot(30.0f, 45.0f, 60.0f);
        D3DXMATRIX m1 = ReferenceObjectWorldMatrix(origin, rot, 1.0f);
        ok(close(axisLen(m1, X), 1.0f), "rotated sf=1.0 -> X axis length 1.0 (length-preserving)");
        D3DXMATRIX m2 = ReferenceObjectWorldMatrix(origin, rot, 2.0f);
        ok(close(axisLen(m2, X), 2.0f), "rotated sf=2.0 -> X axis length 2.0");
        ok(close(axisLen(m2, Z), 2.0f), "rotated sf=2.0 -> Z axis length 2.0");
    }

    // 5) Scale is ABOUT THE ORIGIN: the model origin maps to pos unchanged
    //    (point transform, w=1), regardless of sf -- so an off-origin mesh
    //    translates as it grows but the placement point does not move (Risk-2).
    {
        const D3DXVECTOR3 pos(100.0f, 50.0f, -7.0f);
        D3DXMATRIX m = ReferenceObjectWorldMatrix(pos, noRot, 1.5f);
        D3DXVECTOR4 p;
        D3DXVec3Transform(&p, &origin, &m);   // (0,0,0,1) * m
        ok(close(p.x, 100.0f) && close(p.y, 50.0f) && close(p.z, -7.0f),
           "sf=1.5 leaves the model origin at pos (scale about origin)");
    }

    // 6) Translation is NOT scaled: a unit point on +X with sf=1.5 and pos.x=10
    //    lands at 10 + 1.5 = 11.5 (scale applied to the point first, then translate).
    {
        const D3DXVECTOR3 pos(10.0f, 0.0f, 0.0f);
        D3DXMATRIX m = ReferenceObjectWorldMatrix(pos, noRot, 1.5f);
        D3DXVECTOR4 p;
        D3DXVec3Transform(&p, &X, &m);
        ok(close(p.x, 11.5f) && close(p.y, 0.0f) && close(p.z, 0.0f),
           "sf=1.5 scales the point then translates (11.5, not 11.0 or 15.0)");
    }

    printf(g_fail ? "\n=== reference world: %d FAIL ===\n" : "\n=== reference world: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
