// engine_gizmo.cpp — the grid/manipulator/picking cluster of the Engine class,
// moved verbatim out of engine_reference.cpp (a translation-unit split). SAME class, same header
// (engine.h); cluster-local statics moved with their consumers.

#include <cctype>      // tolower (hardpoint bone matching)
#include <cmath>       // fabsf (manipulator math)
#include <cstdio>      // fprintf/snprintf (shadow-leak probe, readouts)
#include <cstdlib>     // getenv (ALO_* probes)

#include "engine.h"
#include "engine_internal.h"
#include "exceptions.h"
#include "utils.h"
#include "Resources/resource.h"
#include "EmitterInstance.h"      // EmitterInstance::Vertex (world-line/tri/ribbon draws)
#include "GizmoSizing.h"          // pure screen-uniform gizmo-handle formula
#include "PlaneHandle.h"          // pure ground-plane handle math
#include "GizmoRibbon.h"          // camera-facing ribbon quad expansion
#include "RingFade.h"             // ring back-face alpha falloff
#include "SelectionBoxStyle.h"    // selection-box bracket/dash geometry
#include "ReferenceObjectWorld.h" // pure reference-object world-matrix builder

using namespace std;

// Reusable fixed-function world-space line-list draw. Uses the passed
// EmitterInstance::Vertex decl (Position + diffuse Color; the FF view/proj are
// already set this frame). Depth test ON (so a placed object occludes lines
// behind it) but depth write OFF (lines never block particle sorting). Saves +
// restores every render / texture-stage state it touches (this discipline).
// File-static (not an Engine member) so the header needn't see EmitterInstance::Vertex.
static void DrawWorldLines(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                           const EmitterInstance::Vertex* verts, int lineCount,
                           bool depthTest = true)
{
    if (dev == NULL || decl == NULL || verts == NULL || lineCount <= 0) return;

    DWORD oldZEnable, oldZWrite, oldAlphaBlend, oldLighting, oldCull;
    DWORD oldColorOp, oldColorArg1, oldAlphaOp, oldAlphaArg1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    dev->GetRenderState(D3DRS_LIGHTING,         &oldLighting);
    dev->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oldColorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldAlphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
    IDirect3DBaseTexture9* oldTex0 = NULL;
    dev->GetTexture(0, &oldTex0);   // AddRef'd; released after restore
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    dev->GetVertexDeclaration(&oldDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    // depthTest=false => always-on-top (the manipulator gizmo, so handles aren't
    // hidden inside the object); true => co-planar depth-tested (the grid).
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    // Emit the vertex diffuse colour directly (no texture bound).
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    dev->DrawPrimitiveUP(D3DPT_LINELIST, lineCount, verts, sizeof(EmitterInstance::Vertex));

    dev->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    dev->SetTexture(0, oldTex0);
    if (oldTex0) oldTex0->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oldColorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldAlphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
    dev->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    dev->SetRenderState(D3DRS_LIGHTING,         oldLighting);
    dev->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Sibling of DrawWorldLines for the ground-plane handle's translucent
// fill: identical device-state bracketing, but ALPHA-BLEND ON (vertex-alpha *
// SRCALPHA/INVSRCALPHA) and a TRIANGLELIST. depthTest=false => always-on-top like
// the rest of the gizmo. triCount = number of triangles (verts = 3*triCount).
static void DrawWorldTris(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                          const EmitterInstance::Vertex* verts, int triCount,
                          bool depthTest = false)
{
    if (dev == NULL || decl == NULL || verts == NULL || triCount <= 0) return;

    DWORD oldZEnable, oldZWrite, oldAlphaBlend, oldLighting, oldCull, oldSrc, oldDst;
    DWORD oldColorOp, oldColorArg1, oldAlphaOp, oldAlphaArg1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND,         &oldSrc);
    dev->GetRenderState(D3DRS_DESTBLEND,        &oldDst);
    dev->GetRenderState(D3DRS_LIGHTING,         &oldLighting);
    dev->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oldColorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldAlphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
    IDirect3DBaseTexture9* oldTex0 = NULL;
    dev->GetTexture(0, &oldTex0);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    dev->GetVertexDeclaration(&oldDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triCount, verts, sizeof(EmitterInstance::Vertex));

    dev->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    dev->SetTexture(0, oldTex0);
    if (oldTex0) oldTex0->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oldColorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldAlphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
    dev->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND,         oldSrc);
    dev->SetRenderState(D3DRS_DESTBLEND,        oldDst);
    dev->SetRenderState(D3DRS_LIGHTING,         oldLighting);
    dev->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Camera-facing thick lines with a dark outline + per-segment colour/alpha.
// Each RibbonSeg becomes a billboarded quad (2 tris) via gizmoribbon::ExpandSegment.
// Two passes inside one state bracket: a dark underlay at (hw+outline) whose alpha
// tracks each seg (so faded ring segments fade their outline too), then the colour
// at hw. Alpha-blend ON like DrawWorldTris; depthTest=false => always-on-top.
struct RibbonSeg { D3DXVECTOR3 a, b; D3DCOLOR color; };

static void DrawWorldRibbons(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                             const RibbonSeg* segs, int n, const D3DXVECTOR3& camPos,
                             float hw, float outline, D3DCOLOR outlineRGB, bool depthTest,
                             float globalAlpha = 1.0f)
{
    if (dev == NULL || decl == NULL || segs == NULL || n <= 0) return;

    DWORD oZ,oZW,oAB,oSrc,oDst,oLit,oCull,oCop,oCa1,oAop,oAa1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oZ);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oZW);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oAB);
    dev->GetRenderState(D3DRS_SRCBLEND,         &oSrc);
    dev->GetRenderState(D3DRS_DESTBLEND,        &oDst);
    dev->GetRenderState(D3DRS_LIGHTING,         &oLit);
    dev->GetRenderState(D3DRS_CULLMODE,         &oCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oCop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oCa1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oAop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oAa1);
    IDirect3DBaseTexture9* oTex = NULL; dev->GetTexture(0, &oTex);
    IDirect3DVertexDeclaration9* oDecl = NULL; dev->GetVertexDeclaration(&oDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    auto emit = [&](float halfW, bool dark) {
        std::vector<EmitterInstance::Vertex> v; v.reserve(n * 6);
        for (int i = 0; i < n; ++i) {
            float q[4][3];
            gizmoribbon::ExpandSegment(&segs[i].a.x, &segs[i].b.x, &camPos.x, halfW, q);
            D3DCOLOR c = segs[i].color;
            // dark outline: soft grey RGB; halo alpha = seg alpha scaled by the outline
            // colour's own alpha (so the halo is gentler than the line, not a hard keyline).
            if (dark) { const BYTE A = (BYTE)((((c >> 24) & 0xFF) * ((outlineRGB >> 24) & 0xFF)) / 255);
                        c = (outlineRGB & 0x00FFFFFF) | ((DWORD)A << 24); }
            // global gizmo translucency: scale the (possibly fade/outline-set) alpha
            const BYTE ga = (BYTE)(((c >> 24) & 0xFF) * globalAlpha);
            c = (c & 0x00FFFFFF) | ((DWORD)ga << 24);
            const int tri[6] = { 0, 1, 2,  0, 2, 3 };
            for (int t = 0; t < 6; ++t) {
                EmitterInstance::Vertex vert = {};
                vert.Position = D3DXVECTOR3(q[tri[t]][0], q[tri[t]][1], q[tri[t]][2]);
                vert.Color = c;
                v.push_back(vert);
            }
        }
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n * 2, v.data(), sizeof(EmitterInstance::Vertex));
    };
    emit(hw + outline, /*dark=*/true);   // outline underlay first
    emit(hw,           /*dark=*/false);  // colour on top

    dev->SetVertexDeclaration(oDecl);
    if (oDecl) oDecl->Release();
    dev->SetTexture(0, oTex);
    if (oTex) oTex->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oCop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oCa1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oAop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oAa1);
    dev->SetRenderState(D3DRS_ZENABLE,          oZ);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oZW);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oAB);
    dev->SetRenderState(D3DRS_SRCBLEND,         oSrc);
    dev->SetRenderState(D3DRS_DESTBLEND,        oDst);
    dev->SetRenderState(D3DRS_LIGHTING,         oLit);
    dev->SetRenderState(D3DRS_CULLMODE,         oCull);
}

// Unit grid: axis-aligned world lines on the ground plane (the engine's
// first D3DPT_LINELIST primitive). Spacing = m_gridSpacing over a fixed extent,
// with a brighter major line every 5 cells from centre. Co-planar with the
// ground (lifted by a small epsilon so it does not z-fight). No-op when hidden.
void Engine::RenderUnitGrid()
{
    if (!m_gridVisible) return;

    const float spacing = m_gridSpacing;            // > 0 (SetGridSpacing clamps)
    if (spacing <= 0.0f) return;
    const float extent  = 800.0f;                   // half-size: lines span +/-extent
    const int   cells   = (int)(extent / spacing);  // lines each side of centre
    if (cells <= 0) return;
    const float z = m_groundZ + 0.05f;              // tiny lift above the ground quad

    // Vertex cache: the build is O(cells) over 144-byte vertices
    // (~6.4k at spacing 1) and its only variable inputs are spacing and
    // groundZ (extent / colors / major cadence are the literals below) —
    // rebuilding every visible frame was pure heap+CPU churn. File-static
    // is safe: one Engine per process, render is single-threaded, and the
    // CPU-side cache survives device resets by construction.
    static std::vector<EmitterInstance::Vertex> s_gridVerts;
    static float s_gridSpacing = -1.0f;   // -1 = never built
    static float s_gridZ       = 0.0f;
    if (spacing != s_gridSpacing || z != s_gridZ)
    {
        const float span = cells * spacing;         // half-extent on a cell boundary
        const D3DCOLOR kMinor = D3DCOLOR_RGBA( 70,  70,  85, 255);
        const D3DCOLOR kMajor = D3DCOLOR_RGBA(120, 120, 150, 255);

        s_gridVerts.clear();
        s_gridVerts.reserve((size_t)(cells * 2 + 1) * 4);
        auto addLine = [&](float ax, float ay, float bx, float by, D3DCOLOR c)
        {
            EmitterInstance::Vertex v0 = {}, v1 = {};
            v0.Position = D3DXVECTOR3(ax, ay, z); v0.Color = c;
            v1.Position = D3DXVECTOR3(bx, by, z); v1.Color = c;
            s_gridVerts.push_back(v0);
            s_gridVerts.push_back(v1);
        };
        for (int i = -cells; i <= cells; ++i)
        {
            const float p = i * spacing;
            const D3DCOLOR c = (i % 5 == 0) ? kMajor : kMinor;   // major every 5 cells
            addLine(p, -span, p,  span, c);   // line parallel to Y (constant X)
            addLine(-span, p,  span, p, c);   // line parallel to X (constant Y)
        }
        s_gridSpacing = spacing;
        s_gridZ       = z;
    }

    DrawWorldLines(m_pDevice, m_pDeclaration, s_gridVerts.data(), (int)(s_gridVerts.size() / 2));
}

// Screen->world ray for the cursor. Shared with GetCursorPos3D
// (MouseCursor.h) so the manipulator pick uses the IDENTICAL unproject incl. the
// scene-viewport aspect fix. Origin = near-plane point; dir = unit toward far.
void Engine::BuildCursorRay(short screenX, short screenY,
                            D3DXVECTOR3& outOrigin, D3DXVECTOR3& outDir) const
{
    D3DVIEWPORT9 viewport;
    int sx, sy, sw, sh;
    if (GetSceneViewport(sx, sy, sw, sh))
    {
        viewport.X = (DWORD)sx; viewport.Y = (DWORD)sy;
        viewport.Width = (DWORD)sw; viewport.Height = (DWORD)sh;
        viewport.MinZ = 0.0f; viewport.MaxZ = 1.0f;
    }
    else
    {
        GetViewPort(&viewport);
    }
    D3DXMATRIX world; D3DXMatrixIdentity(&world);
    D3DXVECTOR3 front, back;
    const D3DXVECTOR3 sNear((float)screenX, (float)screenY, 0.0f);
    const D3DXVECTOR3 sFar ((float)screenX, (float)screenY, 0.9f);
    D3DXVec3Unproject(&front, &sNear, &viewport, &m_projection, &m_view, &world);
    D3DXVec3Unproject(&back,  &sFar,  &viewport, &m_projection, &m_view, &world);
    outOrigin = front;
    D3DXVECTOR3 d = back - front;
    D3DXVec3Normalize(&outDir, &d);
}

namespace
{
    // Gizmo geometry constants shared by the render and the pick so the
    // drawn handle and the grabbable region stay in lockstep. Arrow length is
    // `baseLen` (screen-uniform; Engine::ReferenceGizmoHandleLength);
    // rings sit just outside the arrowheads.
    constexpr float kAxisPickScale   = 0.18f;   // arrow pick radius = len * this
    constexpr float kHoverGrow       = 1.3f;    // hovered arrow grows (visual + pick)
    constexpr float kRingRadiusScale = 1.15f;   // ring radius   = baseLen * this
    constexpr float kRingPickBand    = 0.14f;   // ring pick band = ringRadius * this
    constexpr float kPlaneInner = 0.0f;    // ground-plane square near edge (* baseLen) -- 0 => the
                                           //         quad runs from the origin, inner edges sit on the X/Y axes
    constexpr float kPlaneOuter = 0.58f;   //          far edge -> side 0.58*baseLen, in the +X/+Y quadrant

    // aesthetics pass. Ribbon width/outline are fractions of baseLen so they
    // stay ~constant px (baseLen is screen-uniform). Palette coordinated with the
    // accent #4ea3ff; the cyan-teal box is a non-axis hue ("selection chrome").
    // kGizmoAlpha fades the whole overlay back so it reads without dominating the scene.
    constexpr float kGizmoAlpha    = 0.72f;   // global translucency on every ribbon
    constexpr float kRibbonWidth   = 0.009f;  // half-width = baseLen * this
    constexpr float kRibbonOutline = 0.010f;  // dark underlay extra half-width (kept wide; softness is in the alpha)
    constexpr float kRingBackAlpha = 0.16f;   // camera-far ring fade floor
    constexpr float kRingIdle      = 0.42f;   // rotation rings sit back at rest; full on hover/active drag
    constexpr float kBracketFrac   = 0.067f;  // corner-bracket length = boxDiagonal * this (clamped to each edge)
    const D3DCOLOR kOutlineRGB  = D3DCOLOR_RGBA(30,33,42,130);    // soft dark-grey halo; alpha = halo translucency (~0.51)
    const D3DCOLOR kSelBoxColor = D3DCOLOR_RGBA(53,210,210,255);  // #35d2d2 cyan-teal

    D3DXVECTOR3 unitAxis(int axis)
    {
        return D3DXVECTOR3(axis == 0 ? 1.0f : 0.0f,
                           axis == 1 ? 1.0f : 0.0f,
                           axis == 2 ? 1.0f : 0.0f);
    }

    // Signed distance along the unit axis `u` (anchored at A) to the point on that
    // axis closest to the ray (origin P, unit dir d). Classic two-line closest
    // point with a=c=1 (u,d unit) and w0 = A - P. False if the ray is ~parallel to
    // the axis (denom -> 0; closest point ill-defined).
    bool axisParamFromRay(const D3DXVECTOR3& P, const D3DXVECTOR3& d,
                          const D3DXVECTOR3& A, const D3DXVECTOR3& u, float& outT)
    {
        const D3DXVECTOR3 w0 = A - P;
        const float b     = D3DXVec3Dot(&u, &d);
        const float dCoef = D3DXVec3Dot(&u, &w0);
        const float eCoef = D3DXVec3Dot(&d, &w0);
        const float denom = 1.0f - b * b;   // a*c - b^2
        if (denom < 1e-5f) return false;
        outT = (b * eCoef - dCoef) / denom; // param along u from A
        return true;
    }
}

// Screen-uniform gizmo sizing (was eye-distance*0.12, "v1"). See GizmoSizing.h.
float Engine::ReferenceGizmoHandleLength() const
{
    const D3DXVECTOR3 eye = m_eye.Position;
    D3DXVECTOR3 fwd = m_eye.Target - eye;                 // view forward (matches LookAtRH)
    D3DXVec3Normalize(&fwd, &fwd);
    D3DXVECTOR3 toRef = m_referencePosition - eye;
    const float depth   = D3DXVec3Dot(&toRef, &fwd);      // VIEW-SPACE depth
    const float eyeDist = D3DXVec3Length(&toRef);
    return GizmoHandleLengthWorld(depth, m_sceneFovY, m_sceneViewportH, m_sceneViewportActive, eyeDist);
}

// Ray-pick the manipulator handle under the cursor: 3 translate arrows + 3
// rotate rings. Each candidate is scored by its miss distance divided by its own
// pick threshold (so the arrow's ray-to-segment gap and the ring's |rho-R| band are
// comparable); the smallest score < 1 wins, and an arrow wins ties (it's tested
// first with a strict `<`). Returns kind=NONE on a miss. Same visible+resolved gate
// as the render so a pick is only attempted on a clickable object.
Engine::ManipHandle Engine::PickManipulatorHandle(short screenX, short screenY) const
{
    ManipHandle hit;   // NONE / -1
    if (!m_referenceObjectSelected) return hit;   // gizmo only grabbable when selected
    if (!m_referenceObjectVisible) return hit;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return hit;
    if (m_pDevice == NULL) return hit;

    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    const D3DXVECTOR3 A = m_referencePosition;
    const float baseLen = ReferenceGizmoHandleLength();

    float bestScore = 1.0f;   // miss/threshold; a candidate must beat 1 to pick

    // --- 3 translate arrows: ray-to-segment gap, threshold = len * kAxisPickScale.
    // The hovered arrow grows by kHoverGrow, so its pick uses the grown extent too
    // (hover was set on the prior mouse-move, so the grown extent is the live one --
    // the outer/arrowhead region of the hovered axis becomes grabbable).
    for (int i = 0; i < 3; ++i)
    {
        const bool  hot = (m_hoverManip.kind == ManipHandle::TRANSLATE && m_hoverManip.axis == i);
        const float len = hot ? baseLen * kHoverGrow : baseLen;
        const float threshold = len * kAxisPickScale;
        const D3DXVECTOR3 u = unitAxis(i);
        D3DXVECTOR3 axisPt;
        float t;
        if (axisParamFromRay(P, d, A, u, t))
        {
            if (t < 0.0f)      t = 0.0f;
            else if (t > len)  t = len;     // clamp to the handle segment [0,len]
            axisPt = A + u * t;
        }
        else
        {
            axisPt = A;                     // ray ~parallel: test the anchor
        }
        // Closest point on the ray to axisPt, then the gap between them.
        D3DXVECTOR3 toAxis = axisPt - P;
        float tc = D3DXVec3Dot(&toAxis, &d);
        if (tc < 0.0f) tc = 0.0f;
        D3DXVECTOR3 rayPt = P + d * tc;
        D3DXVECTOR3 gap   = axisPt - rayPt;
        const float score = D3DXVec3Length(&gap) / threshold;
        if (score < bestScore) { bestScore = score; hit.kind = ManipHandle::TRANSLATE; hit.axis = i; }
    }

    // --- 3 rotate rings: intersect the ray with the ring's world-axis plane, then
    // score |in-plane radius - R| against the band. A ray grazing the plane
    // (denom ~0) or hitting it behind the camera is skipped.
    const float R    = baseLen * kRingRadiusScale;
    const float band = R * kRingPickBand;
    for (int i = 0; i < 3; ++i)
    {
        const D3DXVECTOR3 n = unitAxis(i);
        const float denom = D3DXVec3Dot(&d, &n);
        if (fabsf(denom) < 1e-4f) continue;          // grazing: edge-on ring, skip
        D3DXVECTOR3 oToP = A - P;
        const float t = D3DXVec3Dot(&oToP, &n) / denom;
        if (t < 0.0f) continue;                       // plane behind the ray
        const D3DXVECTOR3 H = P + d * t;              // hit point (lies in the plane)
        D3DXVECTOR3 g = H - A;
        const float rho   = D3DXVec3Length(&g);
        const float score = fabsf(rho - R) / band;
        if (score < bestScore) { bestScore = score; hit.kind = ManipHandle::ROTATE; hit.axis = i; }
    }

    // --- ground-plane (XY) handle: STRICT FALLBACK. Considered only when no arrow or
    // ring was picked above (hit.kind still NONE). The square sits in the +X/+Y
    // diagonal, spatially clear of the on-axis arrows and the 1.15*baseLen rings, so a
    // genuine square click never puts an axis candidate within threshold -- which is
    // exactly why "axes always win, the plane catches the rest" can neither steal an
    // axis click nor be stolen from. No score contest: inside the square == picked.
    // (A score-based contest mis-fires because the pick ray pierces the INFINITE
    // ground plane: an oblique click on an arrow tip / ring arc can land inside the
    // square footprint and a centred hit would win wrongly. Fallback avoids that.)
    if (hit.kind == ManipHandle::NONE)
    {
        float pu, pv, pscore;
        // hit-test against the CURRENT object position (A) -- no drag in progress here.
        if (ManipulatorPlaneOffset(screenX, screenY, /*normalAxis=*/2, A, pu, pv) &&
            planehandle::HandleHit(pu, pv, baseLen * kPlaneInner, baseLen * kPlaneOuter, pscore))
        {
            hit.kind = ManipHandle::PLANE; hit.axis = 2;   // pscore unused for selection (sole candidate)
        }
    }

    return hit;
}

// Signed distance along `axis` from `anchor` to the cursor ray's
// closest point. The host snapshots this at grab time, then accumulates
// precision-scaled per-move deltas of this param (sum of (now - prevMove) * factor)
// and applies new position = anchor + axis*accumulated. (With factor==1 the sum
// telescopes to (now - grab); a Shift-held factor<1 only rescales later deltas.)
// False when degenerate.
bool Engine::ManipulatorAxisParam(short screenX, short screenY, int axis,
                                  const D3DXVECTOR3& anchor, float& outParam) const
{
    if (axis < 0 || axis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);
    return axisParamFromRay(P, d, anchor, unitAxis(axis), outParam);
}

// The cursor's angle (radians) around rotate ring `axis`, measured in that
// ring's world-axis plane (centred at the object origin). The plane normal is the
// world axis; the in-plane basis (a,b) is chosen so increasing angle matches the
// positive Euler rotation that axis drives: ring Z=yaw basis (X,Y), ring X=pitch
// basis (Y,Z), ring Y=roll basis (Z,X) -- i.e. a=axis+1, b=axis+2 (mod 3). The
// host snapshots this at grab and accumulates wrapped per-move deltas (no-jump,
// multi-turn). False when the ray grazes the plane or hits it behind the camera.
bool Engine::ManipulatorRingAngle(short screenX, short screenY, int axis,
                                  float& outAngleRad) const
{
    if (axis < 0 || axis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    const D3DXVECTOR3 n = unitAxis(axis);
    const float denom = D3DXVec3Dot(&d, &n);
    if (fabsf(denom) < 1e-4f) return false;          // ray ~parallel to the plane
    const D3DXVECTOR3 o = m_referencePosition;
    D3DXVECTOR3 oToP = o - P;
    const float t = D3DXVec3Dot(&oToP, &n) / denom;
    if (t < 0.0f) return false;                       // plane behind the ray
    const D3DXVECTOR3 H = P + d * t;
    D3DXVECTOR3 g = H - o;                             // in-plane vector from centre
    const D3DXVECTOR3 a = unitAxis((axis + 1) % 3);
    const D3DXVECTOR3 b = unitAxis((axis + 2) % 3);
    outAngleRad = atan2f(D3DXVec3Dot(&g, &b), D3DXVec3Dot(&g, &a));
    return true;
}

// See header. BuildCursorRay (engine-space ray) then the pure
// planehandle::RayPlaneOffset against the EXPLICIT `anchor` (NOT m_referencePosition --
// a drag must anchor to the fixed grab position, else the moving origin flickers).
// D3DXVECTOR3 is {float x,y,z} so &v.x is a float[3].
bool Engine::ManipulatorPlaneOffset(short screenX, short screenY, int normalAxis,
                                    const D3DXVECTOR3& anchor, float& outU, float& outV) const
{
    if (normalAxis < 0 || normalAxis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);
    return planehandle::RayPlaneOffset(&P.x, &d.x, &anchor.x,
                                       normalAxis, outU, outV);
}

// Draw the combined manipulator (X=red/Y=green/Z=blue), ALWAYS-ON-TOP
// (depth-test off) so it is never hidden inside the object. Per axis: a translate
// arrow (shaft + 4-sided head) from the object origin, plus a rotate ring (a
// world-axis circle just outside the arrowheads). The hovered handle brightens;
// a hovered ARROW also grows (its pick uses the grown extent); a hovered RING
// brightens only (no radius pop). Only shown when the object is selected.
void Engine::RenderReferenceManipulator()
{
    if (!m_referenceObjectSelected) return;
    if (!m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;

    const D3DXVECTOR3 o = m_displayPosition;   // eased origin so the gizmo glides with the object
    const float baseLen = ReferenceGizmoHandleLength();
    const bool dragging = (m_activeManip.kind != ManipHandle::NONE);
    const D3DCOLOR axisCol[3]  = { D3DCOLOR_RGBA(255,  91,  91, 255),   // X #ff5b5b
                                   D3DCOLOR_RGBA( 73, 227,  95, 255),   // Y #49e35f
                                   D3DCOLOR_RGBA( 78, 163, 255, 255) }; // Z #4ea3ff (accent)
    const D3DCOLOR hoverCol[3] = { D3DCOLOR_RGBA(255, 150, 150, 255),
                                   D3DCOLOR_RGBA(150, 255, 170, 255),
                                   D3DCOLOR_RGBA(150, 200, 255, 255) };
    const D3DXVECTOR3 camPos = m_eye.Position;          // for ribbon billboarding + ring fade
    const float ribHalf = baseLen * kRibbonWidth;
    const float ribOut  = baseLen * kRibbonOutline;
    std::vector<RibbonSeg> rib;                          // accumulate all always-on-top ribbons
    auto rseg = [&](const D3DXVECTOR3& a, const D3DXVECTOR3& b, D3DCOLOR c){ rib.push_back({a,b,c}); };

    constexpr int kRingSegs = 48;
    std::vector<EmitterInstance::Vertex> v;
    v.reserve((4 + 2) * 2);   // up to 2 drag-guide lines (PLANE); arrows/rings/sweep radials/plane-border are ribbons
    auto line = [&](const D3DXVECTOR3& a, const D3DXVECTOR3& b, D3DCOLOR c)
    {
        EmitterInstance::Vertex v0 = {}, v1 = {};
        v0.Position = a; v0.Color = c;
        v1.Position = b; v1.Color = c;
        v.push_back(v0);
        v.push_back(v1);
    };
    // During a drag, FADE the non-active handles via alpha (hue kept) so
    // they ghost out softly instead of darkening toward black. The alpha-blended
    // ribbon/tri paths make alpha the right lever now (the old RGB*0.4 darkened).
    auto dim = [&](D3DCOLOR c) -> D3DCOLOR {
        const BYTE A=(BYTE)(((c>>24)&0xFF)*0.30f);
        return (c & 0x00FFFFFF) | ((DWORD)A<<24);
    };

    // Translate arrows.
    for (int i = 0; i < 3; ++i)
    {
        const bool hot = (m_hoverManip.kind == ManipHandle::TRANSLATE && m_hoverManip.axis == i);
        const float    len = hot ? baseLen * kHoverGrow : baseLen;   // hovered arrow grows
        D3DCOLOR c         = hot ? hoverCol[i] : axisCol[i];         // ... and brightens
        const bool isActive = (m_activeManip.kind == ManipHandle::TRANSLATE && m_activeManip.axis == i);
        if (dragging && !isActive) c = dim(c);
        const D3DXVECTOR3 dir = unitAxis(i);
        const D3DXVECTOR3 p1  = unitAxis((i + 1) % 3);   // perpendiculars for the head
        const D3DXVECTOR3 p2  = unitAxis((i + 2) % 3);
        const D3DXVECTOR3 tip  = o + dir * len;
        const D3DXVECTOR3 base = o + dir * (len * 0.82f);
        const float r = len * 0.06f;
        rseg(o, tip, c);                 // shaft
        rseg(tip, base + p1 * r, c);     // 4-sided arrowhead
        rseg(tip, base - p1 * r, c);
        rseg(tip, base + p2 * r, c);
        rseg(tip, base - p2 * r, c);
    }

    {   // faint neutral reference ring (ground plane), behind the rotate arcs
        const D3DCOLOR refc = D3DCOLOR_RGBA(200,205,210, 80);
        const float ringR = baseLen * kRingRadiusScale;
        const D3DXVECTOR3 ax = unitAxis(0), ay = unitAxis(1);
        D3DXVECTOR3 prevp = o + ax * ringR;
        for (int s = 1; s <= kRingSegs; ++s)
        {
            const float a = (2.0f * D3DX_PI) * s / kRingSegs;
            const D3DXVECTOR3 cur = o + (ax * cosf(a) + ay * sinf(a)) * ringR;
            rseg(prevp, cur, refc);
            prevp = cur;
        }
    }

    // Rotate rings: a closed world-axis circle of radius R in the plane perpendicular
    // to each axis, built from the same (a,b) in-plane basis the angle pick uses.
    // Each segment goes through rseg with camera-facing alpha fade so the
    // back-facing half of each ring fades to kRingBackAlpha instead of full opacity.
    const float R = baseLen * kRingRadiusScale;
    for (int i = 0; i < 3; ++i)
    {
        const bool hot = (m_hoverManip.kind == ManipHandle::ROTATE && m_hoverManip.axis == i);
        D3DCOLOR base = hot ? hoverCol[i] : axisCol[i];
        const bool isActive = (m_activeManip.kind == ManipHandle::ROTATE && m_activeManip.axis == i);
        if (dragging && !isActive) base = dim(base);
        // idle rings sit back; the hovered (or actively-dragged) ring comes
        // forward to full clarity. Arrows/plane keep full presence -- rings only.
        const float idleK = (!dragging && !hot) ? kRingIdle : 1.0f;
        const D3DXVECTOR3 a = unitAxis((i+1)%3), b = unitAxis((i+2)%3);
        D3DXVECTOR3 prev = o + a * R;
        for (int s = 1; s <= kRingSegs; ++s)
        {
            const float ang = (2.0f*D3DX_PI)*(float)s/(float)kRingSegs;
            const D3DXVECTOR3 cur = o + (a*cosf(ang)+b*sinf(ang))*R;
            const D3DXVECTOR3 midp = (prev+cur)*0.5f;
            const float fa = ringfade::FacingAlpha(&midp.x, &o.x, &camPos.x, kRingBackAlpha);
            const BYTE A=(BYTE)(((base>>24)&0xFF)*fa*idleK);
            rseg(prev, cur, (base & 0x00FFFFFF) | ((DWORD)A<<24));
            prev = cur;
        }
    }

    // Ground-plane (XY) handle: filled translucent quad (alpha-blended,
    // its own draw call) + a ribbon border (4 segments via rseg, drawn in the always-on-top ribbon pass).
    // Sits in the +X/+Y quadrant, [kPlaneInner,kPlaneOuter]*baseLen on each axis.
    {
        const int planeN = 2;   // normal = world Z (ground); only this plane ships now
        const bool hot      = (m_hoverManip.kind  == ManipHandle::PLANE && m_hoverManip.axis  == planeN);
        const bool isActive = (m_activeManip.kind == ManipHandle::PLANE && m_activeManip.axis == planeN);
        const float inL = baseLen * kPlaneInner, outL = baseLen * kPlaneOuter;
        float qc[4][3];
        planehandle::QuadCorners(&o.x, planeN, inL, outL, qc);   // the unit-tested placement math
        const D3DXVECTOR3 c00(qc[0][0], qc[0][1], qc[0][2]);     // (in,in)
        const D3DXVECTOR3 c10(qc[1][0], qc[1][1], qc[1][2]);     // (out,in)
        const D3DXVECTOR3 c11(qc[2][0], qc[2][1], qc[2][2]);     // (out,out)
        const D3DXVECTOR3 c01(qc[3][0], qc[3][1], qc[3][2]);     // (in,out)
        const BYTE fillA = hot ? 75 : 40;                               // softer now the quad reaches the origin
        D3DCOLOR fill   = D3DCOLOR_RGBA(120, 200, 255, fillA);          // cool translucent blue (Z-normal family)
        D3DCOLOR border = hot ? D3DCOLOR_RGBA(170, 220, 255, 255)
                              : D3DCOLOR_RGBA(120, 200, 255, 255);
        if (dragging && !isActive) { fill = dim(fill); border = dim(border); }
        // Fill: 2 triangles in their own buffer, alpha-blended, always-on-top.
        EmitterInstance::Vertex q[6];
        const D3DXVECTOR3 tri[6] = { c00, c10, c11,  c00, c11, c01 };
        for (int i = 0; i < 6; ++i) { q[i] = EmitterInstance::Vertex{}; q[i].Position = tri[i]; q[i].Color = fill; }
        DrawWorldTris(m_pDevice, m_pDeclaration, q, 2, /*depthTest=*/false);
        // Border: 4 ribbon segments (routed through rseg so they match arrow/ring stroke width).
        rseg(c00, c10, border); rseg(c10, c11, border); rseg(c11, c01, border); rseg(c01, c00, border);
    }

    // Active-drag guides. faint() dims the RGB (lines draw alpha-blend
    // OFF, so alpha is a no-op -- scale the colour, as dim() does). TRANSLATE: one
    // faint axis line. PLANE: both in-plane (X,Y) axis lines, faint. ROTATE sweep
    // unchanged (full colour).
    constexpr float kGuideExtent = 700.0f;   // readability, not a clip bound (infinite far plane)
    auto faint = [](D3DCOLOR c, float k) -> D3DCOLOR {
        const BYTE A=(c>>24)&0xFF, R=(BYTE)(((c>>16)&0xFF)*k), G=(BYTE)(((c>>8)&0xFF)*k), B=(BYTE)((c&0xFF)*k);
        return D3DCOLOR_RGBA(R,G,B,A);
    };
    if (dragging && m_activeManip.kind == ManipHandle::TRANSLATE) {
        const D3DXVECTOR3 dir = unitAxis(m_activeManip.axis);
        line(o - dir * kGuideExtent, o + dir * kGuideExtent, faint(axisCol[m_activeManip.axis], 0.55f));
    }
    else if (dragging && m_activeManip.kind == ManipHandle::PLANE) {
        const int n = m_activeManip.axis;                       // normal axis (2 = ground)
        for (int k = 1; k <= 2; ++k) {                          // the two in-plane axes
            const int ax = (n + k) % 3;
            const D3DXVECTOR3 dir = unitAxis(ax);
            line(o - dir * kGuideExtent, o + dir * kGuideExtent, faint(axisCol[ax], 0.55f));
        }
    }
    else if (dragging && m_activeManip.kind == ManipHandle::ROTATE) {
        const int ax = m_activeManip.axis;
        const D3DXVECTOR3 a = unitAxis((ax + 1) % 3);
        const D3DXVECTOR3 b = unitAxis((ax + 2) % 3);
        // translucent "pie slice" of the swept angle (grab -> applied): a
        // triangle fan from the origin, alpha-blended (DrawWorldTris) UNDER the radial
        // lines, which flush later via the ribbon batch.
        const float delta = m_activeAppliedAngle - m_activeGrabAngle;
        int segn = (int)(fabsf(delta) / (D3DX_PI / 90.0f)); if (segn < 1) segn = 1;   // ~2deg steps
        const float Rf = R * 0.92f;
        const D3DCOLOR pie = (axisCol[ax] & 0x00FFFFFF) | ((DWORD)(BYTE)(70.0f * kGizmoAlpha) << 24);
        std::vector<EmitterInstance::Vertex> fan; fan.reserve(segn * 3);
        for (int s = 0; s < segn; ++s) {
            const float t0 = m_activeGrabAngle + delta * (float)s / (float)segn;
            const float t1 = m_activeGrabAngle + delta * (float)(s + 1) / (float)segn;
            const D3DXVECTOR3 p0 = o + (a*cosf(t0) + b*sinf(t0)) * Rf;
            const D3DXVECTOR3 p1 = o + (a*cosf(t1) + b*sinf(t1)) * Rf;
            EmitterInstance::Vertex v0 = {}, v1 = {}, v2 = {};
            v0.Position = o;  v0.Color = pie;
            v1.Position = p0; v1.Color = pie;
            v2.Position = p1; v2.Color = pie;
            fan.push_back(v0); fan.push_back(v1); fan.push_back(v2);
        }
        if (!fan.empty())
            DrawWorldTris(m_pDevice, m_pDeclaration, fan.data(), (int)fan.size() / 3, /*depthTest=*/false);
        auto radial = [&](float ang){ rseg(o, o + (a*cosf(ang)+b*sinf(ang))*R, axisCol[ax]); };
        radial(m_activeGrabAngle);
        radial(m_activeAppliedAngle);
    }

    if (!rib.empty())
        DrawWorldRibbons(m_pDevice, m_pDeclaration, rib.data(), (int)rib.size(),
                         camPos, ribHalf, ribOut, kOutlineRGB, /*depthTest=*/false, kGizmoAlpha);

    DrawWorldLines(m_pDevice, m_pDeclaration, v.data(), (int)(v.size() / 2), /*depthTest=*/false);
}

// Selection box: the object's object-space AABB (over the kept/drawn
// geometry) transformed by the live world, drawn as dashed edges plus bright corner
// brackets via the camera-facing ribbon renderer (DrawWorldRibbons), depth-tested.
// Depth-tested (it's part of the scene -- the object's near
// faces occlude the far box edges), drawn before the always-on-top gizmo. Shown
// only when the object is selected. The SAME AABB + world drive PickReferenceObject
// below, so the same AABB drives the pick region.
void Engine::RenderReferenceSelectionBox()
{
    if (!m_referenceObjectSelected) return;
    if (!m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;
    D3DXVECTOR3 mn, mx;
    if (!m_referenceObjectMesh.GetBoundingBox(mn, mx)) return;

    const D3DXMATRIX world = ReferenceObjectDisplayWorld();   // eased (render); pick uses committed
    D3DXVECTOR3 c[8];   // corner i: bit0=x, bit1=y, bit2=z (min/max)
    for (int i = 0; i < 8; ++i)
    {
        D3DXVECTOR3 o((i & 1) ? mx.x : mn.x,
                      (i & 2) ? mx.y : mn.y,
                      (i & 4) ? mx.z : mn.z);
        D3DXVec3TransformCoord(&c[i], &o, &world);
    }
    static const int edges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},   // min-z face loop
        {4,5},{5,7},{7,6},{6,4},   // max-z face loop
        {0,4},{1,5},{2,6},{3,7},   // verticals
    };
    const D3DXVECTOR3 camPos = m_eye.Position;
    const float ribHalf = ReferenceGizmoHandleLength() * kRibbonWidth;
    const float ribOut  = ReferenceGizmoHandleLength() * kRibbonOutline;
    // box scale for dash/bracket sizing: the AABB diagonal (corner 0 -> corner 7)
    D3DXVECTOR3 diag = c[7] - c[0]; const float dlen = D3DXVec3Length(&diag);
    std::vector<RibbonSeg> rib;

    // faint SOLID edges (full box outline at the faded opacity the dashes used)
    const D3DCOLOR edgec = D3DCOLOR_RGBA(53,210,210, 70);
    for (int e=0;e<12;++e)
        rib.push_back({ c[edges[e][0]], c[edges[e][1]], edgec });

    // bright corner brackets: each corner's 3 neighbours differ in exactly one bit
    for (int i=0;i<8;++i) {
        D3DXVECTOR3 nb[3]; int k=0;
        for (int bit=0;bit<3;++bit) nb[k++] = c[i ^ (1<<bit)];
        std::vector<selboxstyle::Seg> br;
        selboxstyle::CornerBracketSegs(&c[i].x, &nb[0].x, &nb[1].x, &nb[2].x, dlen*kBracketFrac, br);
        for (size_t s=0;s<br.size();++s)
            rib.push_back({ D3DXVECTOR3(br[s].a[0],br[s].a[1],br[s].a[2]),
                            D3DXVECTOR3(br[s].b[0],br[s].b[1],br[s].b[2]), kSelBoxColor });
    }

    if (!rib.empty())
        DrawWorldRibbons(m_pDevice, m_pDeclaration, rib.data(), (int)rib.size(),
                         camPos, ribHalf, ribOut, kOutlineRGB, /*depthTest=*/true, kGizmoAlpha);
}

// Body pick for click-to-select: ray vs the object's object-space AABB (the
// same box RenderReferenceSelectionBox draws), so the visual box == the clickable
// region. Ray is transformed into object space (inverse world) then slab-tested.
// Not gated on selection (you click an unselected object to select it), but DOES
// gate on visibility (a hidden object draws nothing, so it must not be clickable --
// matches the render/box/gizmo gates). False when no object / hidden / no device /
// no bounds.
bool Engine::PickReferenceObject(short screenX, short screenY) const
{
    if (!m_referenceObjectVisible) return false;   // hidden -> nothing drawn -> not pickable
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return false;
    if (m_pDevice == NULL) return false;
    D3DXVECTOR3 bmin, bmax;
    if (!m_referenceObjectMesh.GetBoundingBox(bmin, bmax)) return false;

    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    // Ray into object space (inverse world). The direction is transformed as a
    // vector (not renormalized) so the slab params stay consistent with the box.
    D3DXMATRIX world = ReferenceObjectWorld(), invWorld;
    D3DXMatrixInverse(&invWorld, NULL, &world);
    D3DXVECTOR3 Po, Do;
    D3DXVec3TransformCoord(&Po, &P, &invWorld);
    D3DXVec3TransformNormal(&Do, &d, &invWorld);

    // Slab method. Components near zero are parallel to that slab -> only a hit if
    // the origin is already inside the slab.
    const float origin[3] = { Po.x, Po.y, Po.z };
    const float dir[3]    = { Do.x, Do.y, Do.z };
    const float lo[3]     = { bmin.x, bmin.y, bmin.z };
    const float hi[3]     = { bmax.x, bmax.y, bmax.z };
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
        if (fabsf(dir[i]) < 1e-8f)
        {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return false;
        }
        else
        {
            const float inv = 1.0f / dir[i];
            float t1 = (lo[i] - origin[i]) * inv;
            float t2 = (hi[i] - origin[i]) * inv;
            if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;   // box is ahead of (or around) the eye
}

// Grid spacing must stay positive (the line loop steps by it).
void Engine::SetGridSpacing(float spacing)
{
    m_gridSpacing = (spacing > 0.0f) ? spacing : 1.0f;
}
