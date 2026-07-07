// engine_reference.cpp — the reference-object render/shadow/manipulator/picking/catalog cluster of the Engine class,
// moved verbatim out of engine.cpp (Phase B translation-unit split —
// tasks/2026-07-06-heavyweight-refactor-plan.md). SAME class, same header
// (engine.h); this is a file split, not a class split. Cluster-local
// file-scope statics moved with their consumers; helpers shared across
// TUs are declared in engine_internal.h with one definition.

#include <cctype>      // tolower (hardpoint bone matching)
#include <cmath>       // fabsf (manipulator math)
#include <cstdio>      // fprintf/snprintf (shadow-leak probe, readouts)
#include <cstdlib>     // getenv (ALO_* probes)

#include "engine.h"
#include "engine_internal.h"
#include "exceptions.h"
#include "utils.h"
#include "resource.h"
#include "EmitterInstance.h"      // EmitterInstance::Vertex (world-line/tri/ribbon draws)
#include "GizmoSizing.h"          // pure screen-uniform gizmo-handle formula
#include "PlaneHandle.h"          // pure ground-plane handle math
#include "GizmoRibbon.h"          // camera-facing ribbon quad expansion
#include "RingFade.h"             // ring back-face alpha falloff
#include "SelectionBoxStyle.h"    // selection-box bracket/dash geometry
#include "ReferenceObjectWorld.h" // pure reference-object world-matrix builder

using namespace std;

// Live reference-object world = rotation then translation. The engine is
// Z-UP (m_eye.Up = (0,0,1); see the Z-up note ~engine.cpp:2213), so "yaw"
// (heading -- turning while staying upright) is rotation about world Z, NOT the
// Y axis D3DXMatrixRotationYawPitchRoll would use. Build the Z-up analogue
// explicitly: yaw->Z, pitch->X, roll->Y, with yaw applied LAST (outermost) so it
// turns the already-tilted object about world up. Wire convention is
// [yaw,pitch,roll] in degrees (schema + BridgeDispatcher). Shared by the render,
// the selection box, and the pick so all three agree on placement.
D3DXMATRIX Engine::ReferenceObjectWorldFrom(const D3DXVECTOR3& pos, const D3DXVECTOR3& rotDeg) const
{
    // Delegate to the pure header so the scale/rotation math is unit-tested
    // headlessly (tests/test_reference_world.cpp). m_referenceScaleFactor (the
    // per-object <Scale_Factor>) is applied LEFTMOST = first, about the object origin,
    // and rides through render / pick / selection-box / hardpoint mounts unchanged.
    return ReferenceObjectWorldMatrix(pos, rotDeg, m_referenceScaleFactor);
}

// Committed transform -> the PICK uses this (the exact, snapped value).
D3DXMATRIX Engine::ReferenceObjectWorld() const
{ return ReferenceObjectWorldFrom(m_referencePosition, m_referenceRotation); }

// Eased "display" transform -> the RENDER uses this (smooth motion). 
D3DXMATRIX Engine::ReferenceObjectDisplayWorld() const
{ return ReferenceObjectWorldFrom(m_displayPosition, m_displayRotation); }

// Ease the render-only display transform toward the committed one once per
// frame (exponential smoothing off WallTimeF, so it stays smooth even when the
// particle preview is paused). Snaps on a discontinuity larger than any plausible
// drag/undo step (e.g. a file load that teleports the transform) -- scene-scale aware
// via the screen-uniform gizmo length. Rotation eases each Euler component along the
// shortest angular path so a 359 deg -> 1 deg change doesn't spin the long way.
void Engine::EaseReferenceDisplay()
{
    // QPC (microsecond) clock, NOT GetTickCount/WallTimeF (~15.6 ms) -- on a
    // high-refresh display the frame interval is below GetTickCount's resolution,
    // so consecutive frames would read dt==0 and collapse the ease to an instant snap.
    const long long nowQpc = EngQpcNow();
    float dt = (m_displayLastQpc == 0) ? 0.0f : (float)(EngQpcUs(m_displayLastQpc, nowQpc) * 1.0e-6);
    m_displayLastQpc = nowQpc;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    constexpr float kEaseTau = 0.09f;   // time constant (s); ~95% caught up in ~3*tau
    const float k = (dt <= 0.0f) ? 0.0f : (1.0f - expf(-dt / kEaseTau));

    D3DXVECTOR3 dp = m_referencePosition - m_displayPosition;
    const float snapGap = 8.0f * ReferenceGizmoHandleLength();
    if (k <= 0.0f || D3DXVec3Length(&dp) > snapGap) {        // first frame / paused / teleport -> snap
        m_displayPosition = m_referencePosition;
        m_displayRotation = m_referenceRotation;
        return;
    }
    m_displayPosition += dp * k;
    auto easeAngle = [k](float disp, float target) -> float {
        float d = target - disp;
        while (d >  180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return disp + d * k;
    };
    m_displayRotation.x = easeAngle(m_displayRotation.x, m_referenceRotation.x);
    m_displayRotation.y = easeAngle(m_displayRotation.y, m_referenceRotation.y);
    m_displayRotation.z = easeAngle(m_displayRotation.z, m_referenceRotation.z);
}

// Draw the imported reference object in two phases (opaque then
// transparent). Each rigid sub-mesh is placed by its bone's object-space matrix
// (sub.placement) times the live object world, and runs its OWN game shader 1:1
// with the same engine binding the particle / dome paths use. Render-state
// save/restore so the particle draw is unaffected. No-op when none loaded/resolved.
void Engine::RenderReferenceObject()
{
    if (!m_referenceObjectVisible)
        return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved())
        return;

    const D3DXMATRIX objectWorld = ReferenceObjectDisplayWorld();   // eased (render)

    // [refZ] Env-gated trace of the reference-object placement at the draw -- the
    // diagnostic that root-caused the "land unit floats above the ground" report
    // (a stale per-object transform leaking across an object swap; fixed by
    // ReferenceTransformMemory.h). Prints the committed + eased positions, the
    // per-object scale, the final world translation, and the ground Z, throttled so
    // a --record run doesn't spew. The env is read ONCE (static) so it's truly free
    // when ALO_REFZ is unset; kept as a durable probe for future placement reports.
    static int s_refzOn = -1;
    if (s_refzOn < 0) s_refzOn = (getenv("ALO_REFZ") != nullptr) ? 1 : 0;
    if (s_refzOn)
    {
        static int s_refz = 0;
        if ((s_refz++ % 30) == 0)
        {
            D3DXVECTOR3 omn(0,0,0), omx(0,0,0);
            m_referenceObjectMesh.GetBoundingBox(omn, omx);
            fprintf(stderr,
                "[refZ] f=%d name=%s vis=%d sel=%d scale=%.3f refPos=(%.3f,%.3f,%.3f) "
                "dispPos=(%.3f,%.3f,%.3f) worldT=(%.3f,%.3f,%.3f) groundZ=%.3f objAABBz=[%.3f..%.3f]\n",
                s_refz, m_referenceObjectName.c_str(),
                m_referenceObjectVisible ? 1 : 0, m_referenceObjectSelected ? 1 : 0,
                m_referenceScaleFactor,
                m_referencePosition.x, m_referencePosition.y, m_referencePosition.z,
                m_displayPosition.x, m_displayPosition.y, m_displayPosition.z,
                objectWorld._41, objectWorld._42, objectWorld._43,
                m_groundZ, omn.z, omx.z);
            fflush(stderr);
        }
    }

    DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend, oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    m_pDevice->GetRenderState(D3DRS_SRCBLEND,         &oldSrcBlend);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,        &oldDestBlend);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    D3DXVECTOR4 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z, 1.0f);

    // Two phases matching the game's Opaque-then-Transparent order: opaque
    // sub-meshes (fill + depth) first, then additive/alpha layers blended on top.
    // Each sub-mesh's render state is set here, not by the .fxo (its SB block is
    // compiled out -- ALAMO_STATE_BLOCKS 0). Opaque uses
    // CULL_CW (the editor renders RIGHT-handed -- LookAtRH/PerspectiveFovRH -- which
    // flips screen-space winding vs the game, so game-front faces present as CW
    // here; CCW culled the front faces and showed the lit hull interior, the
    // "inverted normals" report). Transparent uses CULL_NONE (glows/shields are
    // two-sided; MeshShield itself sets CullMode=NONE) + z-write OFF so the layers
    // don't occlude each other; depth TEST stays on so the opaque hull occludes
    // transparent geometry behind it.
    for (int phase = 0; phase < 2; ++phase)
    {
      const bool opaquePhase = (phase == 0);
      // Draw the unit (mi == 0) then each hardpoint attach model, both in this
      // phase. worldBase = objectWorld for the unit, or attachBone * objectWorld for an
      // attachment (mounting the attach model at the unit's named Attachment_Bone), so
      // its own additive/alpha layers (turret glows) blend over the assembled unit.
      for (size_t mi = 0; mi <= m_referenceAttachments.size(); ++mi)
      {
      ReferenceObjectMesh& refMesh = (mi == 0) ? m_referenceObjectMesh
                                               : m_referenceAttachments[mi - 1]->mesh;
      const D3DXMATRIX worldBase   = (mi == 0) ? objectWorld
                                               : (m_referenceAttachments[mi - 1]->boneMatrix * objectWorld);
      for (RefSubMeshGpu& sub : refMesh.SubMeshes())
      {
        if (sub.effect == NULL || sub.vb == NULL || sub.ib == NULL || sub.decl == NULL)
            continue;
        const bool subOpaque = (sub.renderClass == ALO_RC_OPAQUE);
        if (subOpaque != opaquePhase)
            continue;   // opaque sub-meshes in phase 0, additive/alpha in phase 1

        m_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        if (subOpaque)
        {
            m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
            m_pDevice->SetRenderState(D3DRS_CULLMODE,         D3DCULL_CW);
            m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }
        else
        {
            m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
            m_pDevice->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
            m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            if (sub.renderClass == ALO_RC_ADDITIVE)
            {
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            }
            else // ALO_RC_ALPHA
            {
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            }
        }

        D3DXMATRIX world = sub.placement * worldBase;
        D3DXMATRIX wvp   = world * m_view * m_projection;

        // Object-space eye + light for the bump shader's tangent-space lighting
        // (m_eyePosObj / m_light0ObjVector). These equal the world values only at
        // identity world; transform by inverse-world so the bump + specular stay
        // correct once the object is moved/rotated (picker spinners / gizmo).
        D3DXMATRIX invWorld;
        if (!D3DXMatrixInverse(&invWorld, NULL, &world))   // singular (a degenerate bone matrix)
            D3DXMatrixIdentity(&invWorld);                 // sane fallback: obj-space == world-space
        const D3DXVECTOR3 lightDir3(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
        D3DXVECTOR3 eyeObj3, lightObj3;
        D3DXVec3TransformCoord(&eyeObj3, &m_eye.Position, &invWorld);   // point
        D3DXVec3TransformNormal(&lightObj3, &lightDir3, &invWorld);     // direction
        const D3DXVECTOR4 eyeObj  (eyeObj3.x,   eyeObj3.y,   eyeObj3.z,   1.0f);
        const D3DXVECTOR4 lightObj(lightObj3.x, lightObj3.y, lightObj3.z, 0.0f);

        ID3DXEffect* fx = sub.effect->getD3DEffect();   // AddRef'd
        const Effect::Handles& h = sub.effect->getHandles();

        fx->SetMatrix(h.hWorld,               &world);
        fx->SetMatrix(h.hWorldViewProjection, &wvp);
        fx->SetVector(h.hEyePosition,         &eyePos);
        fx->SetVector(h.hEyeObjPosition,      &eyeObj);     // m_eyePosObj (was unbound -> wrong specular when rotated)
        fx->SetVector(h.hGlobalAmbient,       &m_ambient);
        fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
        fx->SetVector(h.hDirLightObjVec0,     &lightObj);   // object-space light dir (was world)
        fx->SetVector(h.hDirLightDiffuse,     &m_lights[0].Diffuse);
        fx->SetVector(h.hDirLightSpecular,    &m_lights[0].Specular);
        fx->SetMatrixArray(h.hSphLightAll,    m_sphLightAll,  3);
        fx->SetMatrixArray(h.hSphLightFill,   m_sphLightFill, 3);
        fx->SetFloat(h.hTime,                 GetTimeF());

        // Skinned (RSkin) sub-mesh: render in BIND POSE. The RSkin VS uses
        // m_viewProj (world->clip) + a float4x3 m_skinMatrixArray[24] bone palette
        // (P = mul(In.Pos, palette[Normal.w])), NOT m_world / m_worldViewProj. At
        // bind pose every bone's skin matrix collapses to the object world (verified
        // against the alo-viewer's invBind*current build), so bind a UNIFORM
        // objectWorld palette -> P = mul(In.Pos, objectWorld) = world. The .alo
        // skinned verts are model-space, so `world` here == objectWorld (placement
        // is identity for skinned).
        if (sub.skinned && h.hSkinMatrixArray)
        {
            D3DXMATRIX palette[24];
            for (int b = 0; b < 24; ++b) palette[b] = world;
            fx->SetMatrix(h.hViewProjection, &m_viewProjection);
            fx->SetMatrixArray(h.hSkinMatrixArray, palette, 24);
        }

        ApplyAloMaterialParams(fx, sub.params, sub.matHandles, sub.matTextures);

        m_pDevice->SetVertexDeclaration(sub.decl);
        m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
        m_pDevice->SetIndices(sub.ib);

        UINT passes = 0;
        fx->Begin(&passes, 0);
        for (UINT pass = 0; pass < passes; ++pass)
        {
            fx->BeginPass(pass);
#ifndef NDEBUG
            if (sub.primitiveCount > 0)
            {
                DWORD zw = 0, cm = 0;
                m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
                m_pDevice->GetRenderState(D3DRS_CULLMODE,     &cm);
                fprintf(stderr, "[RefObjDraw] %s pass %u/%u zwrite=%lu cull=%lu prims=%u\n",
                        sub.shaderName.c_str(), pass + 1, passes, zw, cm, sub.primitiveCount);
            }
#endif
            m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                            sub.vertexCount, 0, sub.primitiveCount);
            fx->EndPass();
        }
        fx->End();
        fx->Release();
      }   // sub-mesh loop
      }   // mesh loop (unit + attachments)
    }     // phase loop

    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,         oldSrcBlend);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,        oldDestBlend);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

void Engine::RenderReferenceShadows()
{
    if (!m_modelShadowsEnabled || !m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;

    bool any = !m_referenceObjectMesh.ShadowSubMeshes().empty();
    for (size_t i = 0; !any && i < m_referenceAttachments.size(); ++i)
        any = !m_referenceAttachments[i]->mesh.ShadowSubMeshes().empty();
    if (!any) return;

    // [redbug] One-shot marker proving the shadow pass actually DRAWS volumes (not
    // merely that the shadow-volume shader was loaded during ref-object resolution,
    // which happens regardless of whether this pass runs). The regression guard
    // greps for this so it can't false-PASS on a
    // shader-load-without-draw. Past the early-returns above the function always
    // draws, so this fires exactly when a real shadow pass executes. Gated by
    // ALO_SHADER_DIAG + once-per-process so production stays silent.
    {
        static int  s_diag   = -1;
        static bool s_logged = false;
        if (s_diag < 0) s_diag = (getenv("ALO_SHADER_DIAG") != nullptr) ? 1 : 0;
        if (s_diag && !s_logged) {
            size_t vols = m_referenceObjectMesh.ShadowSubMeshes().size();
            for (size_t i = 0; i < m_referenceAttachments.size(); ++i)
                vols += m_referenceAttachments[i]->mesh.ShadowSubMeshes().size();
            printf("[redbug-shadow] RenderReferenceShadows drawing %zu shadow volume(s)\n", vols);
            fflush(stdout);
            s_logged = true;
        }
    }

    const D3DXMATRIX objectWorld = ReferenceObjectDisplayWorld();

    // Extrusion distance for the silhouette volume. alo-viewer uses a large fixed
    // 4000; scale up for big meshes so the volume reliably clears the ground plane.
    float extrusionDist = 4000.0f;
    {
        D3DXVECTOR3 objMin, objMax;
        if (m_referenceObjectMesh.GetBoundingBox(objMin, objMax)) {
            D3DXVECTOR3 d = objMax - objMin;
            extrusionDist = max(4000.0f, 2.0f * D3DXVec3Length(&d));
        }
    }

    // Eye-space depth push for the shadow volume (replaces the old clip-space NDC
    // bias). The volume is shoved a CONSTANT distance DEEPER in EYE space before
    // projection, so coincident near-cap faces z-fail deterministically (kills the
    // self-shadow flicker) WITHOUT the camera-distance drift the clip-Z bias caused.
    //
    // Why eye-space, not clip-space: the old `z_clip += kBias*w` was a CONSTANT
    // offset in NDC. But under this editor's fixed-near=1 / infinite-far projection
    // (z_ndc = 1 - 2/d), a constant NDC nudge maps to a WORLD-depth recession of the
    // volume of ~ (d^2/2)*kBias — QUADRATIC in camera distance d. So the shadow
    // contact held position up close but slid/peter-panned as the camera zoomed out
    // (the reported bug). A constant eye-space translate is a constant WORLD-depth
    // offset at every distance, so the contact holds position at all zooms. The push
    // does introduce a sub-percent screen-space shift of the volume (a deeper vertex
    // projects slightly toward the principal point) but it SHRINKS with distance and
    // is negligible vs the d^2 drift it removes.
    //
    // Sign/magnitude tunable: too small -> self-shadow flicker returns; too large ->
    // the shadow visibly detaches from the model base up close.
    float kShadowEyePush = 2.0f;   // world units to push the volume DEEPER (the fix)
    float kOldClipZBias  = 0.0f;   // 0 = use the eye push (default, shipped behaviour)
#ifndef NDEBUG
    // [shadow-repro] Diagnostic overrides (Debug-only, inert unless set):
    //   ALO_SHADOW_ZPUSH = eye-space push distance in world units (A/B the magnitude).
    //   ALO_SHADOW_ZBIAS = revert to the OLD clip-Z NDC bias with this value, so the
    //     pre-fix camera-distance drift can be reproduced for before/after capture.
    //     ALO_SHADOW_ZBIAS=0 disables BOTH (the no-bias baseline; flicker expected).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZPUSH", b, sizeof(b)) > 0) kShadowEyePush = (float)atof(b); }
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZBIAS", b, sizeof(b)) > 0) { kOldClipZBias = (float)atof(b); kShadowEyePush = 0.0f; } }
#endif
    // RH eye space looks down -Z, so DEEPER = more negative z_eye => translate by
    // -push. Inserted between view and projection: vpPush = view * zpush * proj.
    // (zbiasClip is identity in the shipped path; only the Debug A/B sets it.)
    D3DXMATRIX zpush; D3DXMatrixTranslation(&zpush, 0.0f, 0.0f, -kShadowEyePush);
    D3DXMATRIX zbiasClip; D3DXMatrixIdentity(&zbiasClip); zbiasClip._43 = kOldClipZBias;
    D3DXMATRIX viewProjPush = m_view * zpush * m_projection * zbiasClip;

    // Shadow tint (the multiplicative ZERO/SRCCOLOR darken colour, from m_shadow =
    // the Lighting panel's Sun Shadow Color).
    float shR = m_shadow.x, shG = m_shadow.y, shB = m_shadow.z;
#ifndef NDEBUG
    // [shadow-repro] Debug override so the faint default tint can be forced dark
    // enough to MEASURE the edge feather / contact line in headless captures.
    // ALO_SHADOW_TINT = a single grey level [0..1] (0 = black, fully dark shadow).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_TINT", b, sizeof(b)) > 0) { float v=(float)atof(b); shR=shG=shB=v; } }
#endif

    // --- save every state we touch ---
    DWORD oCW,oZF,oZW,oZE,oSE,oTSS,oSR,oSM,oSWM,oCull,oSFn,oSP,oSZF,oSFa,
          oCcwFn,oCcwP,oCcwZF,oCcwFa,oAB,oSB,oDB,oLit,oFVF,oATE;
    DWORD oTexCOP,oTexCA1,oTexAOP,oTexAA1;
    m_pDevice->GetRenderState(D3DRS_COLORWRITEENABLE,&oCW); m_pDevice->GetRenderState(D3DRS_ZFUNC,&oZF);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,&oZW); m_pDevice->GetRenderState(D3DRS_ZENABLE,&oZE);
    m_pDevice->GetRenderState(D3DRS_STENCILENABLE,&oSE); m_pDevice->GetRenderState(D3DRS_TWOSIDEDSTENCILMODE,&oTSS);
    m_pDevice->GetRenderState(D3DRS_STENCILREF,&oSR); m_pDevice->GetRenderState(D3DRS_STENCILMASK,&oSM);
    m_pDevice->GetRenderState(D3DRS_STENCILWRITEMASK,&oSWM); m_pDevice->GetRenderState(D3DRS_CULLMODE,&oCull);
    m_pDevice->GetRenderState(D3DRS_STENCILFUNC,&oSFn); m_pDevice->GetRenderState(D3DRS_STENCILPASS,&oSP);
    m_pDevice->GetRenderState(D3DRS_STENCILZFAIL,&oSZF); m_pDevice->GetRenderState(D3DRS_STENCILFAIL,&oSFa);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILFUNC,&oCcwFn); m_pDevice->GetRenderState(D3DRS_CCW_STENCILPASS,&oCcwP);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILZFAIL,&oCcwZF); m_pDevice->GetRenderState(D3DRS_CCW_STENCILFAIL,&oCcwFa);
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE,&oAB); m_pDevice->GetRenderState(D3DRS_SRCBLEND,&oSB);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,&oDB); m_pDevice->GetRenderState(D3DRS_LIGHTING,&oLit);
    m_pDevice->GetRenderState(D3DRS_ALPHATESTENABLE,&oATE);
    m_pDevice->GetFVF(&oFVF);
    m_pDevice->GetTextureStageState(0,D3DTSS_COLOROP,&oTexCOP); m_pDevice->GetTextureStageState(0,D3DTSS_COLORARG1,&oTexCA1);
    m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAOP,&oTexAOP); m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAARG1,&oTexAA1);
    IDirect3DVertexDeclaration9* oDecl=NULL; m_pDevice->GetVertexDeclaration(&oDecl);
    IDirect3DVertexShader9* oVS=NULL; m_pDevice->GetVertexShader(&oVS);
    IDirect3DPixelShader9*  oPS=NULL; m_pDevice->GetPixelShader(&oPS);
    IDirect3DBaseTexture9*  oTex0=NULL; m_pDevice->GetTexture(0,&oTex0);
    IDirect3DVertexBuffer9* oStream0=NULL; UINT oStreamOffset=0, oStreamStride=0;
    m_pDevice->GetStreamSource(0, &oStream0, &oStreamOffset, &oStreamStride);
    IDirect3DIndexBuffer9*  oIndices=NULL; m_pDevice->GetIndices(&oIndices);

    // [soft-shadows] Soft path is available only when the toggle is on AND the
    // blur effect + mask RT both came up. Otherwise fall back to the shipped hard
    // darken quad (no regression). Decided once, up front, so the save/restore of
    // the extra resources (RT / depth-stencil / viewport / samplers 0-3) is paired.
    const bool soft = m_softShadowsEnabled && m_shadowBlurReady
                   && m_pShadowBlurEffect != NULL && m_pShadowMask != NULL;

    // Extra save for the soft path's RT detour + sampler binds. AddRef'd handles
    // released in the restore tail; sampler filter/address states restored too.
    IDirect3DSurface9* oRT0 = NULL;  IDirect3DSurface9* oDS = NULL;
    D3DVIEWPORT9 oViewport;
    IDirect3DBaseTexture9* oTexS[4] = { NULL,NULL,NULL,NULL };
    DWORD oMinF[4], oMagF[4], oMipF[4], oAddrU[4], oAddrV[4];
    if (soft)
    {
        m_pDevice->GetRenderTarget(0, &oRT0);          // AddRef'd
        m_pDevice->GetDepthStencilSurface(&oDS);       // AddRef'd (may be NULL)
        m_pDevice->GetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->GetTexture(s, &oTexS[s]);       // AddRef'd
            m_pDevice->GetSamplerState(s, D3DSAMP_MINFILTER, &oMinF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MAGFILTER, &oMagF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MIPFILTER, &oMipF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSU, &oAddrU[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSV, &oAddrV[s]);
        }
    }

    // ============ VOLUME PASS — write stencil (single-pass two-sided z-fail) ============
    // Mirrors alo-viewer: one CULLMODE=NONE pass with TWOSIDEDSTENCILMODE so CW faces
    // INCR and CCW faces DECR the stencil on z-fail in a single draw.
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
    m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,TRUE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,1);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_INCR);   // CW faces incr on zfail
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,D3DSTENCILOP_DECR);// CCW faces decr on zfail
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,D3DSTENCILOP_KEEP);

    // Lambda draws every shadow sub-mesh for the main mesh + all attachments.
    // Technique t0_zfail is kept for its VS (extrudes silhouette to infinity);
    // the technique's own stencil state-block is overridden by our hand-coded ops.
    auto drawVolumes = [&]()
    {
        for (size_t mi = 0; mi <= m_referenceAttachments.size(); ++mi)
        {
            ReferenceObjectMesh& refMesh = (mi==0) ? m_referenceObjectMesh
                                                   : m_referenceAttachments[mi-1]->mesh;
            const D3DXMATRIX worldBase   = (mi==0) ? objectWorld
                                                   : (m_referenceAttachments[mi-1]->boneMatrix * objectWorld);
            for (RefSubMeshGpu& sub : refMesh.ShadowSubMeshes())
            {
                if (sub.effect==NULL || sub.vb==NULL || sub.ib==NULL || sub.decl==NULL) continue;
                if (!sub.effect->isShadowVolume()) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' resolved to a non-shadow effect - skipped\n", sub.shaderName.c_str());
#endif
                    continue;
                }
                D3DXMATRIX world = sub.placement * worldBase;
                D3DXMATRIX invWorld;
                if (!D3DXMatrixInverse(&invWorld,NULL,&world)) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': singular world matrix — identity used for light direction (shadow direction will be wrong)\n", sub.shaderName.c_str());
#endif
                    D3DXMatrixIdentity(&invWorld);
                }
                const D3DXVECTOR3 lightDir3(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
                D3DXVECTOR3 lightObj3; D3DXVec3TransformNormal(&lightObj3,&lightDir3,&invWorld);
                const D3DXVECTOR4 lightObj(lightObj3.x,lightObj3.y,lightObj3.z,0.0f);

                ID3DXEffect* fx = sub.effect->getD3DEffect();
                const Effect::Handles& h = sub.effect->getHandles();
                if (FAILED(fx->SetTechnique("t0_zfail"))) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' has no t0_zfail technique - skipped\n", sub.shaderName.c_str());
#endif
                    fx->Release(); continue;
                }
                // Eye-space-pushed transforms (constant world-depth recession — see
                // the kShadowEyePush note above; replaces the old clip-Z bias that
                // drifted ~d^2 with camera distance). Rigid VS reads WorldViewProjection,
                // skinned VS reads ViewProjection; both ride the same pushed projection.
                D3DXMATRIX wvpB = world * viewProjPush;
                D3DXMATRIX vpB  = viewProjPush;
                fx->SetMatrix(h.hWorldViewProjection, &wvpB);
                fx->SetMatrix(h.hViewProjection,      &vpB);
                fx->SetVector(h.hDirLightObjVec0,     &lightObj);
                fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
                if (sub.skinned && h.hSkinMatrixArray) {
                    D3DXMATRIX palette[24]; for (int b=0;b<24;++b) palette[b]=world;
                    fx->SetMatrixArray(h.hSkinMatrixArray, palette, 24);
                }
                D3DXHANDLE hExtr = fx->GetParameterBySemantic(NULL, "SHADOW_EXTRUSION_DISTANCE");
                if (hExtr) { D3DXVECTOR4 ex(extrusionDist,extrusionDist,extrusionDist,extrusionDist); fx->SetVector(hExtr, &ex); }

                m_pDevice->SetVertexDeclaration(sub.decl);
                m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
                m_pDevice->SetIndices(sub.ib);
                UINT passes=0;
                if (FAILED(fx->Begin(&passes,0)) || passes==0) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': fx->Begin failed or returned 0 passes — skipping sub-mesh\n", sub.shaderName.c_str());
#endif
                    fx->End(); fx->Release(); continue;
                }
                for (UINT p=0;p<passes;++p) {
                    fx->BeginPass(p);
                    m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,sub.vertexCount,0,sub.primitiveCount);
                    fx->EndPass();
                }
                fx->End(); fx->Release();
            }
        }
    };

    // Single two-sided pass: CW faces INCR, CCW faces DECR on z-fail (states set above).
    drawVolumes();

    if (soft)
    {
        // ============ SOFT PATH — mask-to-alpha + blurred multiply composite ============
        // Port of FoC's doSoftShadows: bake the stencil region into a screen-space
        // mask's ALPHA, then run StencilDarkenFinalBlur (4-tap blur + tint) as a
        // ZERO/SRCCOLOR multiply onto the scene. NO depth-bias states anywhere.
        const DWORD tint = D3DCOLOR_COLORVALUE(shR, shG, shB, 1.0f);

        // The mask RT to render the stencil mask into: the matching-MSAA surface
        // when MSAA is active (the stencil test needs the multisampled depth still
        // bound), else the non-MS mask texture's top surface.
        IDirect3DSurface9* pMaskTop  = NULL;       // m_pShadowMask L0, AddRef'd
        m_pShadowMask->GetSurfaceLevel(0, &pMaskTop);
        IDirect3DSurface9* pRenderInto = NULL;     // borrowed (no extra ref to release)
        if (m_msaaActive && m_pShadowMaskMsaa) { pRenderInto = m_pShadowMaskMsaa; }
        else                                    { pRenderInto = pMaskTop; }

        // --- mask-to-alpha (hand-coded StencilDarkenToAlpha state block) ---
        // Bind the mask RT but KEEP the current depth-stencil (the stencil written
        // by the volume pass must persist for the NOTEQUAL test).
        m_pDevice->SetRenderTarget(0, pRenderInto);
        D3DVIEWPORT9 maskVp = { 0, 0,
            m_presentationParameters.BackBufferWidth,
            m_presentationParameters.BackBufferHeight, 0.0f, 1.0f };
        m_pDevice->SetViewport(&maskVp);
        // Clear ALPHA to white (=1, "no shadow") everywhere; the gated quad punches
        // alpha=0 ("shadow") into the stencil region. RGB is irrelevant (blur reads
        // only .a) but a full white clear keeps the target well-defined.
        m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);

        m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);   // shadow where stencil != 0
        m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
        m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
        m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
        m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_ALWAYS);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA); // ALPHA only
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

        // Full-screen XYZRHW quad whose diffuse ALPHA = 0 -> writes alpha 0 into the
        // shadow region (stencil != 0), leaving the cleared white (1) elsewhere.
        const float mx0 = -0.5f, my0 = -0.5f;
        const float mx1 = (float)m_presentationParameters.BackBufferWidth  - 0.5f;
        const float my1 = (float)m_presentationParameters.BackBufferHeight - 0.5f;
        struct PTVtx { float x,y,z,rhw; DWORD c; };
        const DWORD shadowAlpha0 = 0x00000000;   // RGBA, alpha 0
        const PTVtx maskQuad[4] = {
            {mx0,my0,0,1,shadowAlpha0},{mx1,my0,0,1,shadowAlpha0},
            {mx0,my1,0,1,shadowAlpha0},{mx1,my1,0,1,shadowAlpha0} };
        m_pDevice->SetVertexShader(NULL);
        m_pDevice->SetPixelShader(NULL);
        m_pDevice->SetTexture(0,NULL);
        m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
        m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,maskQuad,sizeof(PTVtx));

        // --- MSAA resolve: multisampled mask surface -> the sampled texture ---
        if (m_msaaActive && m_pShadowMaskMsaa && pMaskTop)
        {
            m_pDevice->StretchRect(m_pShadowMaskMsaa, NULL, pMaskTop, NULL, D3DTEXF_NONE);
        }

        // --- restore scene RT + depth-stencil + viewport before the composite ---
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);

        // --- blur composite (StencilDarkenFinalBlur, ZERO/SRCCOLOR multiply) ---
        // Bind the resolved mask to sampler stages 0-3 (the .fx's sampler0..3 all
        // read the same mask; the VS spreads 4 tap offsets across them). LINEAR
        // filtering + clamp: the mask alpha is a HARD 0/1 step, so POINT-sampled
        // taps land on discrete texels and stair-step the edge (reads crisp/hard
        // even with a wide blurAmt). Bilinear lets each tap straddle the edge texel
        // so the 4-tap cross resolves into a smooth feather (bug-1 fix; pairs with
        // the wider blurAmt below). MinF/MagF are saved/restored in the tail.
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->SetTexture(s, m_pShadowMask);
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        }

        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,FALSE);
        // Depth-test the composite so the shadow only darkens SCENE GEOMETRY, never
        // the cleared-far background. The blurred mask bleeds the shadow a few texels
        // past the model/ground silhouette; without this, that bleed darkened the
        // empty background (black bands in the sky around the model). The quad sits at
        // NDC z=1.0 (far) with ZFUNC=GREATER, so it passes only where the depth buffer
        // holds geometry (depth < 1.0) and is rejected on the cleared background
        // (depth == 1.0). ZWRITE stays off (the depth-stencil oDS is bound here).
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_GREATER);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
        m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);

        // Clip-space full-screen quad (the VS multiplies by m_worldViewProj, which
        // we drive to identity). Vertex COLOR0 = tint (the ps lerps tint<->white by
        // the mask alpha). UV v increases downward (top of screen = v 0).
        //
        // CRITICAL: the shadow mask was rendered into the FULL backbuffer (the
        // mask-to-alpha viewport is 0,0..W,H), but this composite draws into the
        // SCENE VIEWPORT (oViewport) — a SUB-RECT of the backbuffer in the live
        // editor (the React panels inset the 3D view). So the quad's UVs must span
        // the scene viewport's sub-region of the mask, NOT 0..1, or the shadow is
        // scaled+offset off the object (the "floating silhouette" bug). With an
        // edge-to-edge clip(±1)→viewport and UV→sub-rect mapping, pixel centers land
        // on texel centers exactly — no separate half-texel nudge needed. In
        // --capture the viewport IS the full backbuffer, so this reduces to 0..1
        // (which is why the bug never showed in headless capture).
        const float Wbb = (float)m_presentationParameters.BackBufferWidth;
        const float Hbb = (float)m_presentationParameters.BackBufferHeight;
        float u0 = (float)oViewport.X / Wbb;
        float u1 = (float)(oViewport.X + oViewport.Width)  / Wbb;
        float v0 = (float)oViewport.Y / Hbb;
        float v1 = (float)(oViewport.Y + oViewport.Height) / Hbb;
#ifndef NDEBUG
        // [shadow-repro] ALO_SHADOW_VPFIX=0 reverts to the old full-mask 0..1 UVs
        // (the pre-fix "floating silhouette" behaviour) for a before/after A/B.
        { char b[8]; if (GetEnvironmentVariableA("ALO_SHADOW_VPFIX", b, sizeof(b)) > 0 && atof(b) == 0.0) { u0=0.0f; u1=1.0f; v0=0.0f; v1=1.0f; } }
#endif
        struct BlurVtx { float x,y,z; float nx,ny,nz; DWORD c; float u,v; };
        // z = 1.0 (NDC far) so the ZFUNC=GREATER depth test above darkens only
        // geometry pixels (depth < 1.0), not the cleared-far background.
        const BlurVtx blurQuad[4] = {
            { -1.0f,  1.0f, 1.0f, 0,0,1, tint, u0, v0 },  // top-left
            {  1.0f,  1.0f, 1.0f, 0,0,1, tint, u1, v0 },  // top-right
            { -1.0f, -1.0f, 1.0f, 0,0,1, tint, u0, v1 },  // bottom-left
            {  1.0f, -1.0f, 1.0f, 0,0,1, tint, u1, v1 },  // bottom-right
        };

        ID3DXEffect* bfx = m_pShadowBlurEffect->getD3DEffect();   // AddRef'd
        D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
        if (m_hShadowBlurWvp)  bfx->SetMatrix(m_hShadowBlurWvp, &ident);
        // blurAmt is the 4-tap cross half-spread in UV. The game's authored 0.0015
        // is only ~2 texels at the editor's RT size — reads HARD with POINT sampling.
        // The shader is only a 4-tap cross, so a WIDE spread (e.g. 0.009 ≈ 11 texels)
        // makes the 4 taps read as discrete offset copies — a plus-shaped GHOST/halo.
        // ~0.003 (≈4 texels) keeps the taps blending; paired with the LINEAR sampling
        // above that gives a soft edge without ghosting. (For a genuinely WIDE soft
        // shadow the 4-tap must become multi-pass / more taps — a separate change.)
        // Tunable via ALO_SHADOW_BLURAMT.
        float blurAmt = 0.003f;
#ifndef NDEBUG
        // [shadow-repro] Diagnostic blur-width override (A/B the feather width
        // without recompiling). Inert unless ALO_SHADOW_BLURAMT is set; Debug-only.
        { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_BLURAMT", b, sizeof(b)) > 0) blurAmt = (float)atof(b); }
#endif
        if (m_hShadowBlurAmt)  bfx->SetFloat(m_hShadowBlurAmt, blurAmt);
        bfx->SetTechnique(m_hShadowBlurTech);
        // The blur uses a vertex shader (vs_1_1) reading POSITION/COLOR0/TEXCOORD0,
        // so emit via an FVF with a NON-transformed float4 POSITION (the VS applies
        // the identity m_worldViewProj). D3DFVF_XYZRHW would skip the VS entirely.
        // The NORMAL slot is required: the game's compiled vs_1_1 was authored
        // against the VERTEX_MESH_NU2C layout (pos/normal/uv/color), so its input
        // declaration expects a normal between POSITION and TEXCOORD0. Omitting it
        // shifts the texcoord register the VS reads, so every pixel sampled one
        // mask texel -> the whole frame got the shadow tint instead of just the
        // cast region. (Matches alo-viewer's m_sceneQuad FVF.)
        m_pDevice->SetVertexShader(NULL);   // FVF path -> fixed-function VS slot; effect's VS binds in BeginPass
        m_pDevice->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1);
        UINT bpasses = 0;
        if (SUCCEEDED(bfx->Begin(&bpasses, 0)) && bpasses > 0)
        {
            for (UINT bp = 0; bp < bpasses; ++bp)
            {
                bfx->BeginPass(bp);
                m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, blurQuad, sizeof(BlurVtx));
                bfx->EndPass();
            }
        }
        bfx->End();
        bfx->Release();

        SAFE_RELEASE(pMaskTop);
    }
    else
    {
    // ============ DARKEN PASS (hard fallback) ============
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
    m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

    D3DVIEWPORT9 vp; m_pDevice->GetViewport(&vp);
    const float x0=(float)vp.X-0.5f, y0=(float)vp.Y-0.5f;
    const float x1=(float)(vp.X+vp.Width)-0.5f, y1=(float)(vp.Y+vp.Height)-0.5f;
    const DWORD tint = D3DCOLOR_COLORVALUE(shR,shG,shB,1.0f);
    struct PTVtx { float x,y,z,rhw; DWORD c; };
    const PTVtx quad[4] = { {x0,y0,0,1,tint},{x1,y0,0,1,tint},{x0,y1,0,1,tint},{x1,y1,0,1,tint} };
    m_pDevice->SetVertexShader(NULL);
    m_pDevice->SetPixelShader(NULL);
    m_pDevice->SetTexture(0,NULL);
    m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(PTVtx));
    }

    // --- restore ---
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,oSE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,oTSS);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,oCW); m_pDevice->SetRenderState(D3DRS_ZFUNC,oZF);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,oZW); m_pDevice->SetRenderState(D3DRS_ZENABLE,oZE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,oSR); m_pDevice->SetRenderState(D3DRS_STENCILMASK,oSM);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,oSWM); m_pDevice->SetRenderState(D3DRS_CULLMODE,oCull);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,oSFn); m_pDevice->SetRenderState(D3DRS_STENCILPASS,oSP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,oSZF); m_pDevice->SetRenderState(D3DRS_STENCILFAIL,oSFa);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,oCcwFn); m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,oCcwP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,oCcwZF); m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,oCcwFa);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,oAB); m_pDevice->SetRenderState(D3DRS_SRCBLEND,oSB);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,oDB); m_pDevice->SetRenderState(D3DRS_LIGHTING,oLit);
    m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,oATE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,oTexCOP); m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,oTexCA1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,oTexAOP); m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,oTexAA1);
    // [red-bug fix] FVF and an explicit vertex declaration share ONE device slot in
    // D3D9, and GetFVF reports the SAME code (0x252) for BOTH the engine's explicit
    // ParticleElements decl (COLOR at offset 40) and the FVF-canonical layout
    // (COLOR at offset 24) because they share a component set (XYZ|NORMAL|DIFFUSE|TEX2).
    // The old unconditional SetFVF(oFVF) therefore overwrote the just-restored explicit
    // particle decl with the FVF-canonical layout, whose offsets do NOT match the
    // 44-byte EmitterInstance::Vertex. The inheriting particle DrawIndexedPrimitiveUP
    // (EmitterInstance::Render binds no decl/FVF of its own) then read the particle
    // COLOR out of the UV0 bytes -> additive source ~= 0 -> the additive emitter
    // vanished, and it persisted until a device reset because nothing rebinds
    // m_pDeclaration. Restore whichever vertex format was actually active, never both:
    // at the shadow-pass entry this engine always has an explicit decl bound, so oDecl
    // is non-null in practice; the SetFVF branch is a fallback for a pure-FVF device
    // (where GetVertexDeclaration returns null). Guarded headless by a
    // regression check (ALO_DUMP_RSTATE decl-element dump).
    if (oDecl) { m_pDevice->SetVertexDeclaration(oDecl); oDecl->Release(); }
    else       { m_pDevice->SetFVF(oFVF); }
    m_pDevice->SetVertexShader(oVS); if (oVS) oVS->Release();
    m_pDevice->SetPixelShader(oPS);  if (oPS) oPS->Release();
    m_pDevice->SetTexture(0, oTex0); if (oTex0) oTex0->Release();
    m_pDevice->SetStreamSource(0, oStream0, oStreamOffset, oStreamStride);
    if (oStream0) oStream0->Release();
    m_pDevice->SetIndices(oIndices);
    if (oIndices) oIndices->Release();

    // [soft-shadows] Restore the extra resources the soft path detoured through:
    // RT(0) + depth-stencil + viewport (already re-bound before the composite, but
    // re-assert here for the case the composite was skipped), sampler stages 0-3
    // textures + filter/address states. Stage-0 texture is owned by the existing
    // tail above; here we restore stages 1-3 + every stage's sampler states, and
    // release every AddRef'd handle. No-op when the hard path ran.
    if (soft)
    {
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            if (s != 0) m_pDevice->SetTexture(s, oTexS[s]);   // stage 0 done above
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, oMinF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, oMagF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, oMipF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU, oAddrU[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV, oAddrV[s]);
            if (oTexS[s]) oTexS[s]->Release();
        }
        if (oRT0) oRT0->Release();
        if (oDS)  oDS->Release();
    }
}

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

    // [D5] Vertex cache: the build is O(cells) over 144-byte vertices
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

// The host calls this once at startup to ARM the eager reference-object
// catalog prefetch: set the persistent m_catalogWanted latch and the next
// Update()->StartCatalogBuildIfNeeded() kicks a background build immediately --
// so the catalog is likely ready before the user ever opens the picker, and
// every later mod/submod switch rebuilds eagerly (the switch invalidates the
// catalog, the latch stays set, Update() re-kicks) with no picker-open needed.
//
// Ordering invariant (HostWindow): ModManager::RestoreLastLayerStack() runs in the
// host impl ctor and applies the saved layer stack to the FileManager
// SYNCHRONOUSLY, BEFORE the host calls ArmCatalogPrefetch(). So the first build
// this arms snapshots the FileManager already reflecting the then-selected mod +
// submods -- it prioritizes the active content, not a wasted base-game pass. If a
// future refactor moves the mod restore AFTER this call, the startup build would
// target base game and a later ReloadTextures would rebuild for the mod (correct,
// just one wasted pass) -- keep restore before ArmCatalogPrefetch().
void Engine::ArmCatalogPrefetch()
{
    m_catalogWanted = true;
}

// [reference-model-shadows] Synchronous catalog build for headless --capture-ref.
// Unlike StartCatalogBuildIfNeeded (which builds on a worker with an ISOLATED
// FileManager to avoid freezing the UI thread / racing its MEG handles), a
// one-shot headless run has no UI thread and no concurrent FileManager access,
// so we build directly against m_fileManager on the calling thread. No-op once
// built. Lets SetReferenceObject resolve INLINE rather than deferring to a later
// Update() that the one-shot run exits before reaching.
void Engine::BuildCatalogSync()
{
    if (m_referenceCatalogBuilt) return;
    BuildGameObjectCatalog(m_fileManager, m_referenceCatalog);
    m_referenceCatalogBuilt = true;
    m_catalogWanted         = false;
}

// Kick a background catalog (re)build when one is wanted and not already
// built or in flight. BuildGameObjectCatalog parses every object XML the active
// content exposes (O(content)); on a big mod that froze the whole window when run
// synchronously on the WebView2 UI thread. Snapshot the FileManager's content roots
// on THIS (UI) thread, then build on a worker with an ISOLATED FileManager (its own
// MEG handles -> no seek-race against the UI thread's FileManager). Update() harvests
// the finished catalog. Safe to call every frame; early-returns when built/building.
void Engine::StartCatalogBuildIfNeeded()
{
    if (!m_catalogWanted || m_referenceCatalogBuilt || m_catalogBuilding.load())
        return;

    const std::vector<std::wstring> basepaths = m_fileManager.GetBasepaths();
    const std::vector<std::wstring> roots     = m_fileManager.GetContentRoots();
    const uint64_t                  gen       = m_catalogGeneration;

    // Record the content-root stack this build reflects, so a later
    // texture-only ReloadTextures (F5 / file open) sees an unchanged context and
    // does NOT invalidate.
    m_catalogContextRoots = roots;

    if (m_catalogThread.joinable()) m_catalogThread.join();   // a prior build, already harvested
    m_catalogBuilding.store(true);
    m_catalogThread = std::thread([this, basepaths, roots, gen]()
    {
        auto cat = std::make_unique<GameObjectCatalog>();
        try
        {
            FileManager isoFm(basepaths);          // own MEG handles; ctor throws on empty MEGs
            isoFm.SetLayers(roots);                // replicate the FULL content-root stack
            BuildGameObjectCatalog(isoFm, *cat);   // O(content) XML parse, OFF the UI thread
        }
        catch (...)
        {
            // Isolated-FM construction / parse failed -> publish an empty catalog
            // (the picker shows an empty list, never a crash in the worker).
        }
        {
            std::lock_guard<std::mutex> lock(m_catalogMutex);
            m_pendingCatalog    = std::move(cat);
            m_pendingCatalogGen = gen;
        }
        m_catalogBuilding.store(false);
    });
}

// Host calls this right after Update(): true once when a finished catalog was
// just swapped in, so the host fires engine/state/changed (the picker re-queries).
// Only ever set when ArmCatalogPrefetch() ran (a worker built a catalog); a harmless
// one-shot bool even if unconsumed (only re-set to true on the next install -- no
// accumulation).
bool Engine::ConsumeCatalogReadyFlag()
{
    if (!m_catalogJustReady) return false;
    m_catalogJustReady = false;
    return true;
}

// Enumerate selectable game objects (Name + category) for the picker.
// Filtered to FIELDABLE units + structures via IsPickerListed (profile role !=
// Excluded && fieldable; heroes exempt) -- backdrops / props / projectiles / templates /
// non-fieldable variants are dropped so the list isn't thousands of entries. The catalog
// itself still holds every object (so a hardpoint / variant lookup against the full set is
// unaffected); only the picker payload is trimmed.
void Engine::EnumerateReferenceObjects(std::vector<GameObjectRef>& out)
{
    // Mark the catalog wanted; the actual (re)build is launched ONLY by Update()
    // -- AFTER it harvests any finished build -- so a build is never started outside the
    // harvest path (which would let a second worker spawn and the next harvest's join()
    // block the UI thread on it, reintroducing the freeze). Return whatever's ready:
    // while still building, out is empty + IsReferenceCatalogReady() is false, so the
    // bridge reports "building" and the picker shows "Loading objects…".
    m_catalogWanted = true;
    out.clear();
    for (const GameObjectRef& r : m_referenceCatalog.objects)
        if (IsPickerListed(r))   // profile role != Excluded && fieldable (heroes exempt)
            out.push_back(r);
}

// Select a reference object by its in-game Name; clears it when empty.
// A fresh selection is shown by default (reset visibility) so a previously-hidden
// object doesn't make a newly-picked one silently invisible.
void Engine::SetReferenceObject(const std::string& name)
{
    // Per-object transform memory. The reference transform (m_referencePosition /
    // m_referenceRotation) is otherwise ONE global state that leaks across object
    // swaps: a Z set while framing object A (e.g. lifting a space unit) stays put and
    // floats a land unit B picked after it. Here, on a real swap to a DIFFERENT
    // object, stash the outgoing object's transform under its name and load the
    // incoming object's remembered transform (origin if it was never moved), so each
    // object keeps its own placement and a freshly-picked unit starts grounded.
    //
    // Edge handling (see the unit test tests/test_reference_transform_memory.cpp):
    //  * Initial load ("" -> A), incl. the startup restore which sets the transform
    //    THEN the name: no memory for A and no outgoing object -> KEEP the current
    //    (restored) transform rather than zeroing it.
    //  * Leaving to None (A -> ""): remember A, then clear the live transform so a
    //    later None -> X can't inherit A's Z.
    //  * Re-select of the SAME object: no-op.
    //
    // The outgoing key is the DESIRED name, NOT the resolved m_referenceObjectName:
    // a deferred catalog rebuild (mod/submod switch) clears m_referenceObjectName to
    // "" while a moved transform is still live, and keying off that empty resolved
    // name would mis-read the next pick as a startup "keep current" and re-float the
    // new object. m_referenceDesiredName holds the last picked object through the
    // defer (it is only cleared by entering None) and is set AFTER this block, so it
    // still names the object the live transform belongs to -- and is genuinely empty
    // only at true startup, where keeping the restored transform is correct.
    {
        auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
        const std::string oldKey = lower(m_referenceDesiredName);  // object the live transform belongs to
        const std::string newKey = lower(name);                    // incoming pick
        bool changed = false;
        const reftransform::Xform next = reftransform::OnSwap(
            m_referenceTransforms, oldKey, newKey,
            reftransform::Xform{ m_referencePosition, m_referenceRotation }, changed);
        if (changed)
        {
            m_referencePosition = next.pos;
            m_referenceRotation = next.rot;
            // Snap the eased display transform to the (possibly restored) committed
            // value so the incoming object appears AT its placement instead of gliding
            // in from the previous object's position.
            m_displayPosition = m_referencePosition;
            m_displayRotation = m_referenceRotation;
        }
    }

    m_referenceDesiredName = name;   // the INTENT (persisted); shown value is resolved below
    if (!name.empty())
        m_referenceObjectVisible = true;
    // Auto-select on an explicit pick so the manipulator gizmo appears immediately. A
    // desired name that resolves present keeps this true (ResolveDesiredReference's
    // present branch doesn't touch selection); an absent/None/deferred desired is
    // force-deselected inside ResolveDesiredReference. The lock is intentionally STICKY +
    // persisted (do NOT clear m_referenceLocked here, or the startup restore is wiped).
    m_referenceObjectSelected = RefLockResolveSelected(!name.empty(), m_referenceLocked);
    m_hoverManip = ManipHandle();
    ResolveDesiredReference();
}

// [capture] Sum of shadow-volume sub-meshes across the primary mesh and all
// hardpoint attachments. Used by --capture-ref to warn when the loaded object
// carries no shadow geometry (so the operator knows the image will show no shadow).
size_t Engine::ReferenceShadowSubMeshCount() const
{
    size_t count = m_referenceObjectMesh.ShadowSubMeshes().size();
    for (const auto& att : m_referenceAttachments)
        count += att->mesh.ShadowSubMeshes().size();
    return count;
}

// [capture] World-space AABB of the loaded reference object. The mesh's
// GetBoundingBox is OBJECT-space; transform all 8 corners by ReferenceObjectWorld()
// (scale + rotation + translation) and take the enclosing min/max so a fit camera
// can frame the actual on-screen extent (rotation/scale-aware).
bool Engine::GetReferenceObjectBounds(D3DXVECTOR3& outMin, D3DXVECTOR3& outMax) const
{
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved())
        return false;
    D3DXVECTOR3 omin, omax;
    if (!m_referenceObjectMesh.GetBoundingBox(omin, omax)) return false;

    const D3DXMATRIX world = ReferenceObjectWorld();
    const D3DXVECTOR3 corners[8] = {
        D3DXVECTOR3(omin.x, omin.y, omin.z), D3DXVECTOR3(omax.x, omin.y, omin.z),
        D3DXVECTOR3(omin.x, omax.y, omin.z), D3DXVECTOR3(omax.x, omax.y, omin.z),
        D3DXVECTOR3(omin.x, omin.y, omax.z), D3DXVECTOR3(omax.x, omin.y, omax.z),
        D3DXVECTOR3(omin.x, omax.y, omax.z), D3DXVECTOR3(omax.x, omax.y, omax.z),
    };
    D3DXVECTOR3 wmin( 1e30f,  1e30f,  1e30f);
    D3DXVECTOR3 wmax(-1e30f, -1e30f, -1e30f);
    for (const auto& c : corners)
    {
        D3DXVECTOR3 w;
        D3DXVec3TransformCoord(&w, &c, &world);
        wmin.x = (std::min)(wmin.x, w.x); wmin.y = (std::min)(wmin.y, w.y); wmin.z = (std::min)(wmin.z, w.z);
        wmax.x = (std::max)(wmax.x, w.x); wmax.y = (std::max)(wmax.y, w.y); wmax.z = (std::max)(wmax.z, w.z);
    }
    outMin = wmin;
    outMax = wmax;
    return true;
}

// Full teardown of the render-state fields RebuildReferenceObjectMesh clears on its
// empty-name / deferred exits: mesh, hardpoint attachments, render scale, status. Does
// NOT touch m_referenceMeshDeferred -- that flag stays owned by the caller
// (ResolveDesiredReference), which sets it per branch -- so a clear path never leaves a
// stale attachment node or a stale render scale behind.
void Engine::ResetReferenceRenderState()
{
    m_referenceObjectMesh.Clear();
    m_referenceAttachments.clear();
    m_referenceScaleFactor  = 1.0f;
    m_referenceObjectStatus = ReferenceObjectStatus::None;
}

// Resolve the desired (intended/persisted) reference name into the shown selection,
// existence-gated against the catalog. Clears to None + deselects when the object is
// absent or nothing is desired; defers (shown None now) while the catalog rebuilds and
// is retried by Update() on catalog-ready. A successful resolve does NOT auto-select --
// a restore is inert (gizmo appears only on a user click); an explicit pick keeps its
// gizmo because SetReferenceObject sets m_referenceObjectSelected before calling here and
// the present branch leaves that flag untouched.
void Engine::ResolveDesiredReference()
{
    // Not built yet (startup, or just-invalidated by a mod switch) AND something is
    // desired: show None NOW (no stale id during the async build) and arm the retry.
    if (!m_referenceCatalogBuilt && !m_referenceDesiredName.empty())
    {
        m_referenceObjectName.clear();
        SetReferenceObjectSelected(false);   // deselect: hides gizmo + aborts any in-flight drag
        ResetReferenceRenderState();
        m_catalogWanted         = true;
        m_referenceMeshDeferred = true;      // Update() retries this fn on catalog-ready
        return;
    }

    m_referenceMeshDeferred = false;
    const std::string resolved = m_referenceCatalogBuilt
        ? ResolveReferenceName(m_referenceCatalog.objects, m_referenceDesiredName)
        : std::string();                     // desired empty -> "" regardless
    m_referenceObjectName = resolved;

    if (resolved.empty())                    // explicit None, or absent-from-this-stack
    {
        SetReferenceObjectSelected(false);   // honest None: deselect
        ResetReferenceRenderState();
        return;
    }
    RebuildReferenceObjectMesh();            // present: load it (may set LoadFailed for a bad .alo)
}

// Resolve the selected Name -> model path -> load. The probe (only on the
// load-failure path, so the common case parses the .alo once) distinguishes a
// skinned/unsupported object from a missing/corrupt one for the picker status.
// Resolve + CreateBuffers no-op until the device is valid; Load (CPU) always runs.
void Engine::RebuildReferenceObjectMesh()
{
    m_referenceAttachments.clear();   // rebuilt below iff the unit mounts hardpoint models
    // Reset the render scale at the TOP so every exit path (empty-name clear,
    // deferred catalog-not-built return, LoadFailed, successful resolve) leaves no
    // stale scale from a previously-selected object. (A mod/submod switch now clears via
    // ResetReferenceRenderState through ResolveDesiredReference, so Rebuild's deferred
    // return is a fallback.) Overwritten from the catalog only on a successful resolve below.
    m_referenceScaleFactor = 1.0f;

    if (m_referenceObjectName.empty())
    {
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::None;
        m_referenceMeshDeferred = false;
        return;
    }

    // The catalog resolves Name -> model path; if it isn't built yet (startup
    // restore, or just-invalidated on a mod/submod switch), DEFER until the background
    // build finishes -- Update() retries this once the catalog is ready. Never build
    // synchronously here, or a submod switch with a selected object would freeze the UI.
    if (!m_referenceCatalogBuilt)
    {
        m_catalogWanted         = true;   // Update() launches the build after its harvest
        m_referenceMeshDeferred = true;
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::None;   // nothing renders until ready
        return;
    }
    m_referenceMeshDeferred = false;

    // Case-INSENSITIVE match: the catalog folds Names to lower-case keys (the
    // Alamo engine resolves Names case-insensitively) but stores original casing
    // for display, so a persisted/cross-mod name with different casing must still
    // resolve. Adopt the catalog's canonical casing so the snapshot converges.
    std::string modelPath;
    const GameObjectRef* selected = nullptr;
    for (const GameObjectRef& r : m_referenceCatalog.objects)
        if (_stricmp(r.name.c_str(), m_referenceObjectName.c_str()) == 0)
        {
            modelPath = r.modelPath;
            m_referenceObjectName = r.name;
            selected = &r;
            // Successful catalog resolve -> adopt the per-object render scale.
            m_referenceScaleFactor = r.scaleFactor;
#ifndef NDEBUG
            fprintf(stderr, "[refscale] '%s' Scale_Factor=%.3f\n", r.name.c_str(), r.scaleFactor);
#endif
            break;
        }
    if (modelPath.empty())
    {
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::LoadFailed;
        return;
    }

    // Gather this unit's hardpoint geometry from the catalog: bones whose meshes
    // are damaged-state (hide them so the unit renders intact) + the attach models to
    // mount. Bone names match the .alo CASE-INSENSITIVELY -> fold to lower for the
    // ReferenceObjectMesh hide-set + bone lookup.
    auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
    std::set<std::string> hideBones;
    struct AttachReq { std::string model; std::string bone; };
    std::vector<AttachReq> attach;
    if (selected)
        for (const std::string& hpName : selected->hardpointNames)
        {
            auto it = m_referenceCatalog.hardpoints.find(lower(hpName));
            if (it == m_referenceCatalog.hardpoints.end()) continue;
            const HardPointDef& d = it->second;
            if (!d.damageDecalBone.empty())     hideBones.insert(lower(d.damageDecalBone));
            if (!d.damageParticlesBone.empty()) hideBones.insert(lower(d.damageParticlesBone));
            if (!d.collisionMeshBone.empty())   hideBones.insert(lower(d.collisionMeshBone));
            // A hardpoint with no Model_To_Attach mounts nothing; one with no
            // Attachment_Bone has nowhere to mount (GetBoneObjectMatrix("") would
            // also alias an unnamed .alo bone) -- skip both rather than push a useless
            // or mis-placeable request.
            if (!d.modelToAttach.empty() && !d.attachmentBone.empty())
                attach.push_back({ d.modelToAttach, d.attachmentBone });
        }
    // A bone that is a live mount point (some hardpoint's Attachment_Bone) must
    // never be hidden -- even when another hardpoint lists the SAME bone as its
    // Collision_Mesh/Damage_* bone. Vanilla FoC does exactly this: the Star Destroyer
    // fighter-bay/tractor hardpoints set Collision_Mesh == Attachment_Bone (SPAWN_00 /
    // HP_trac_bone). Hiding a mount bone would drop the hull geometry the attach model
    // sits on. Mount points win, so subtract them from the hide-set after gathering.
    for (const AttachReq& a : attach) hideBones.erase(lower(a.bone));

    const std::string aloPath = "Data\\Art\\Models\\" + modelPath;
    if (m_referenceObjectMesh.Load(m_fileManager, aloPath, hideBones))
    {
        if (m_pDevice != NULL)
        {
            // Resolve degrades per-sub-mesh on a throwing/missing shader (see
            // ReferenceObjectMesh::Resolve, which catches the wexception getShader
            // can raise), so it returns false only when NOTHING resolved. Report
            // that as LoadFailed rather than a silent "Ok" that renders nothing
            // (HasResolved() would be false -> RenderReferenceObject draws nothing,
            // no error shown). No try/catch here: the per-sub-mesh guard already
            // contains the throw, so this call can't throw a wexception.
            if (!m_referenceObjectMesh.Resolve(m_shaderManager, m_pDevice))
            {
                m_referenceObjectMesh.Clear();
                m_referenceObjectStatus = ReferenceObjectStatus::LoadFailed;
                return;
            }
            m_referenceObjectMesh.CreateBuffers(m_pDevice, m_fileManager);
        }

        // Mount each hardpoint's attach model at its Attachment_Bone on the unit.
        // Graceful: skip a hardpoint whose bone isn't in the unit skeleton, or whose
        // attach .alo is missing / corrupt / skinned-only / unresolvable (the unit still
        // renders; that attachment just doesn't). childPlacement * boneMatrix * objectWorld
        // places each attach sub-mesh (RenderReferenceObject).
        for (const AttachReq& a : attach)
        {
            D3DXMATRIX boneMat;
            if (!m_referenceObjectMesh.GetBoneObjectMatrix(a.bone, boneMat))
            {
                // The single highest-value diagnostic: a bone that SHOULD match but
                // doesn't (case/whitespace/encoding skew, or a stale XML bone name)
                // leaves the unit silently weaponless. Surface it in Debug feel-tests.
#ifndef NDEBUG
                fprintf(stderr, "[RefObj] attach '%s': Attachment_Bone '%s' not in unit skeleton -- skipped\n",
                        a.model.c_str(), a.bone.c_str());
#endif
                continue;
            }
            auto att = std::make_unique<ReferenceAttachment>();
            if (!att->mesh.Load(m_fileManager, "Data\\Art\\Models\\" + a.model))
            {
#ifndef NDEBUG
                fprintf(stderr, "[RefObj] attach '%s' failed to load (missing/corrupt/skinned-only) -- skipped\n",
                        a.model.c_str());
#endif
                continue;
            }
            if (m_pDevice != NULL)
            {
                if (!att->mesh.Resolve(m_shaderManager, m_pDevice))
                    continue;   // nothing resolved -> don't mount an invisible attachment (Resolve logs per-sub-mesh in Debug)
                att->mesh.CreateBuffers(m_pDevice, m_fileManager);
            }
            att->boneMatrix = boneMat;
            m_referenceAttachments.push_back(std::move(att));
        }
        // Aggregate signal for feel-testing: a unit that should be fully armed but
        // mounted fewer than expected models is visible at a glance without per-skip noise.
#ifndef NDEBUG
        if (!attach.empty())
            fprintf(stderr, "[RefObj] '%s': mounted %zu of %zu hardpoint attach model(s)\n",
                    m_referenceObjectName.c_str(), m_referenceAttachments.size(), attach.size());
#endif

        // Load-time warning: if the primary object has shadow sub-meshes but
        // none resolved to a real shadow-volume effect, the stencil pass will silently
        // draw nothing. Emit once here (not per-frame) so it appears in host.log /
        // OutputDebugStringA without spamming. Primary object only; attachments are a
        // follow-up if needed. Uses OutputDebugStringA + printf — the same Release-visible
        // pair BloomLog uses (printf reaches the Debug console; OutputDebugStringA reaches
        // any attached debugger / DebugView in Release).
        {
            const auto& shadowSubs = m_referenceObjectMesh.ShadowSubMeshes();
            if (!shadowSubs.empty())
            {
                size_t resolved = 0;
                for (const RefSubMeshGpu& s : shadowSubs)
                    if (s.effect && s.effect->isShadowVolume()) ++resolved;
                if (resolved == 0)
                {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "[shadow] reference object '%s': %zu shadow-volume sub-mesh(es) but none "
                        "resolved (MeshShadowVolume.fx/RSkinShadowVolume.fx not found in active "
                        "content) - no model shadow will render\n",
                        m_referenceObjectName.c_str(), shadowSubs.size());
                    OutputDebugStringA(buf);
                    printf("%s", buf);
                }
            }
        }

        m_referenceObjectStatus = ReferenceObjectStatus::Ok;
        return;
    }

    // Load failed (skinned-only / collision-only / missing / corrupt) -> probe to
    // tell the user which, so the picker shows the right message: a genuinely
    // absent file (NotFound) reads "model file not found", a skinned-only object
    // reads "not supported", and anything else (corrupt / non-mesh) "couldn't load".
    m_referenceObjectMesh.Clear();
    const ModelProbeResult probe = ProbeModelSkinned(m_fileManager, modelPath);
    m_referenceObjectStatus =
        (probe == ModelProbeResult::SkinnedUnsupported) ? ReferenceObjectStatus::Skinned
      : (probe == ModelProbeResult::NotFound)           ? ReferenceObjectStatus::ModelMissing
      :                                                   ReferenceObjectStatus::LoadFailed;
}

// Grid spacing must stay positive (the line loop steps by it).
void Engine::SetGridSpacing(float spacing)
{
    m_gridSpacing = (spacing > 0.0f) ? spacing : 1.0f;
}
