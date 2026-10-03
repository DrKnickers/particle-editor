// Contents (search for the quoted text):
//   "Object placement" - World transforms and display easing.
//   "Object rendering" - Opaque and transparent imported geometry.
//   "Object catalog" - Prefetch, background build and picker listing.
//   "Object selection and bounds" - Selection and geometry bounds.
//   "Object loading" - Render-state teardown and mesh rebuilding.
//
// engine_reference.cpp — the reference-object render/shadow/manipulator/picking/catalog cluster of the Engine class,
// moved verbatim out of engine.cpp (a translation-unit split). SAME class, same header
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
#include "Resources/resource.h"
#include "EmitterInstance.h"      // EmitterInstance::Vertex (world-line/tri/ribbon draws)
#include "GizmoSizing.h"          // pure screen-uniform gizmo-handle formula
#include "PlaneHandle.h"          // pure ground-plane handle math
#include "GizmoRibbon.h"          // camera-facing ribbon quad expansion
#include "RingFade.h"             // ring back-face alpha falloff
#include "SelectionBoxStyle.h"    // selection-box bracket/dash geometry
#include "ReferenceObjectWorld.h" // pure reference-object world-matrix builder

using namespace std;

// ---------- Object placement ----------
// Live reference-object world = rotation then translation. The engine is
// Z-UP (m_eye.Up = (0,0,1), set in the Engine constructor), so "yaw"
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

    // During a live gizmo drag the committed transform already tracks the cursor
    // 1:1 at input cadence, so easing the RENDER transform only makes the object
    // (and the gizmo, drawn from m_displayPosition) TRAIL the cursor -- the
    // "floaty / laggy" report. Snap display -> committed while a handle is held.
    // The ease exists for DISCRETE jumps -- spinners, keyboard nudge, undo, file
    // load -- all of which leave m_activeManip NONE. The QPC clock is refreshed
    // above, so the first post-release frame eases from the true rest pose.
    if (m_activeManip.kind != ManipHandle::NONE) {
        m_displayPosition = m_referencePosition;
        m_displayRotation = m_referenceRotation;
        return;
    }

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

// ---------- Object rendering ----------
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
        // Distance fade: mirror the particle path (EmitterInstance::Render). The
        // editor never authors real DISTANCE_FADE_VALS, so a mesh shader that
        // strips the baked default reads (0,0) -> Compute_Distance_Fade == 0 and
        // the mesh previews at reduced alpha. Force (0,1) = no fade -> full alpha;
        // a no-op on stock shaders whose baked default already resolves to 1.0.
        if (h.hDistanceFadeVals)
        {
            D3DXVECTOR4 distanceFade(0.0f, 1.0f, 0.0f, 0.0f);   // (0,1)=no fade; editor-only, engine sets real vals in-game
            fx->SetVector(h.hDistanceFadeVals, &distanceFade);
        }

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

// ---------- Object catalog ----------
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

// ---------- Object selection and bounds ----------
// Select a reference object by its in-game Name; clears it when empty.
// A fresh selection is shown by default (reset visibility) so a previously-hidden
// object doesn't make a newly-picked one silently invisible.
void Engine::SetReferenceObject(const std::string& name)
{
    if (DeviceCallsBlocked()) return;

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

// ---------- Object loading ----------
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

