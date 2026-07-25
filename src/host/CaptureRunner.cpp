// CaptureRunner.cpp — the --capture / --capture-ref one-shot, extracted
// verbatim from HostWindowImpl::Run (Phase C of
// tasks/2026-07-06-heavyweight-refactor-plan.md). The alias preludes bind
// the old Run()-scope names to the Deps references so the moved segments
// below are unchanged except: the gate's `continue;` became
// `return TickResult::Running;` and each method ends with its return.

#include "CaptureRunner.h"

#include "AlphaCompositor.h"   // CaptureSnapshotToFile
#include "HostRunUtil.h"       // PerfQpcNow/PerfQpcFreq/QpcMs/DeriveSibling
#include "WindowCapture.h"     // host::CaptureWindowToPng

#include "../ModManager.h"
#include "../ParticleSystem.h"
#include "../ParticleSystemIO.h"   // LoadParticleSystem
#include "../SpawnerDriver.h"
#include "../engine.h"             // Engine, FreezePreviewClockAt, StepPreviewFrames
#include "../utils.h"              // WideToAnsi

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace host {

// printf-style forwarder to the Impl's Log sink (same _vsnprintf_s
// discipline as HostWindowImpl::Log) so moved Log(...) sites are verbatim.
void CaptureRunner::Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m_deps.log) m_deps.log(buf);
}

void CaptureRunner::Init()
{
    // Old Run()-scope names -> Deps references (verbatim-move aliases).
    auto& engine         = m_deps.engine;
    auto& modManager     = m_deps.modManager;
    auto& particleSystem = m_deps.particleSystem;
    auto& spawnerDriver  = m_deps.spawnerDriver;

    if (!engine)
    {
        Log("[capture] no engine available — cannot capture\n");
        captureFailed = true;
    }
    else if (!m_captureRef.empty())
    {
        // [reference-model-shadows] --capture-ref: build the GameObject
        // catalog SYNCHRONOUSLY (no UI thread to freeze headlessly, and
        // no concurrent FileManager access to race), then select the
        // reference object so it resolves INLINE — the normal async path
        // defers SetReferenceObject until Update() harvests the worker
        // build, which a one-shot headless run would exit before. The
        // active mod was already restored at startup (WM_CREATE), so the
        // catalog builds against the user's active content automatically.
        engine->BuildCatalogSync();
        engine->SetReferenceObject(WideToAnsi(m_captureRef));
        engine->SetReferenceObjectVisible(true);
        // Deterministic sim/shader time for --capture-ref too: freeze the
        // preview clock at a fixed anchor (stepped 1/60 per counted frame in
        // the capture loop) so m_time-consuming mesh shaders render
        // identically every run. Mirrors the --capture .alo path (#481).
        FreezePreviewClockAt(0.0f);
        {
            const ReferenceObjectStatus refStatus = engine->GetReferenceObjectStatus();
            if (refStatus == ReferenceObjectStatus::Ok)
            {
                // Status Ok but GetReferenceObjectBounds returning false
                // means no sub-mesh actually resolved renderable geometry
                // (e.g. device was NULL during Resolve, or every sub-mesh
                // failed the shader step). The capture would be a blank
                // image — fail with exit 2 instead.
                D3DXVECTOR3 wmin, wmax;
                if (!engine->GetReferenceObjectBounds(wmin, wmax))
                {
                    Log("[capture] reference object '%ls' resolved status Ok but no renderable geometry (device/resolve issue)\n",
                        m_captureRef.c_str());
                    captureFailed = true;
                }
                else
                {
                    Log("[capture] reference object '%ls' resolved ok; rendering %d frames -> %ls\n",
                        m_captureRef.c_str(), m_captureFrames, m_capturePng.c_str());

                    // [capture] Frame the whole object: fit the camera to the
                    // world-space AABB so the captured image shows the entire
                    // model (+ surrounding ground), not the zoomed-in default.
                    // The capture render uses the full-RT projection at 45deg
                    // FOV (D3DXToRadian(45), see engine.cpp SetSceneViewport /
                    // ResetParameters), so size dist off that same fovY.
                    {
                        D3DXVECTOR3 center((wmin.x + wmax.x) * 0.5f,
                                           (wmin.y + wmax.y) * 0.5f,
                                           (wmin.z + wmax.z) * 0.5f);
                        D3DXVECTOR3 diag = wmax - wmin;
                        float radius = 0.5f * D3DXVec3Length(&diag);
                        if (radius < 1.0f) radius = 1.0f;   // degenerate-bounds guard

                        const float fovY = D3DXToRadian(45.0f);
                        float dist = radius / tanf(0.5f * fovY) * 1.6f;  // 1.6 = framing margin

                        // 3/4 view in the editor's Z-up RH space: look down at an
                        // angle from -Y so the ground plane (z=0) is visible.
                        D3DXVECTOR3 dir(0.7f, -0.7f, 0.5f);
                        D3DXVec3Normalize(&dir, &dir);

                        Engine::Camera cam;
                        cam.Position = center + dir * dist;
                        cam.Target   = center;
                        cam.Up       = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
                        engine->SetCamera(cam);
                        Log("[capture] fit camera: center=(%.1f,%.1f,%.1f) radius=%.1f dist=%.1f eye=(%.1f,%.1f,%.1f)\n",
                            center.x, center.y, center.z, radius, dist,
                            cam.Position.x, cam.Position.y, cam.Position.z);
                    }
                    const size_t shadowCount = engine->ReferenceShadowSubMeshCount();
                    if (shadowCount > 0)
                    {
                        Log("[capture] reference object '%ls' has %zu shadow-volume sub-mesh(es)\n",
                            m_captureRef.c_str(), shadowCount);
                    }
                    else
                    {
                        Log("[capture] WARNING: reference object '%ls' has NO shadow-volume sub-meshes - captured image will show no model shadow\n",
                            m_captureRef.c_str());
                    }
                }  // else (bounds resolved)
            }
            else
            {
                const char* reason =
                    (refStatus == ReferenceObjectStatus::Skinned)      ? "skinned mesh — not supported" :
                    (refStatus == ReferenceObjectStatus::ModelMissing) ? "model file not found in the active mod/base" :
                    (refStatus == ReferenceObjectStatus::LoadFailed)   ? "model failed to load (corrupt or non-mesh .alo)" :
                                                                         "unknown name / mod not active";
                Log("[capture] ERROR: reference object '%ls' did not resolve (%s)\n",
                    m_captureRef.c_str(), reason);
                captureFailed = true;
            }
        }
        if (m_captureSkydomeSlot > 0)
        {
            const bool sok = engine->SetSkydomeSlot(m_captureSkydomeSlot);
            Log("[capture] skydome slot %d -> %s\n",
                m_captureSkydomeSlot, sok ? "ok" : "FAILED");
        }
    }
    else
    {
        // Select the mod that owns this .alo BEFORE loading, so its
        // texture overrides (Mod etc.) resolve instead
        // of base-game art. The editor does this on mod-select; a
        // direct --capture load must do it explicitly or particles
        // render with the wrong textures. Match the .alo path against
        // discovered mods by case-insensitive path prefix; SelectMod
        // swaps FileManager's mod path + reloads textures (engine is
        // already bound via SetEngine in WM_CREATE).
        if (modManager)
        {
            bool matched = false;
            for (const auto& mod : modManager->GetMods())
            {
                const size_t n = mod.path.size();
                if (n > 0 && _wcsnicmp(m_captureAlo.c_str(), mod.path.c_str(), n) == 0
                    && (m_captureAlo.size() == n
                        || m_captureAlo[n] == L'\\' || m_captureAlo[n] == L'/'))
                {
                    modManager->SelectMod(mod.path);
                    Log("[capture] selected mod for .alo: %ls\n", mod.path.c_str());
                    matched = true;
                    break;
                }
            }
            if (!matched)
                Log("[capture] no mod matched .alo path — using base-game/restored textures\n");
        }

        std::string err;
        std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(m_captureAlo, &err);
        if (!loaded)
        {
            Log("[capture] LoadParticleSystem(%ls) failed: %s\n",
                m_captureAlo.c_str(), err.c_str());
            captureFailed = true;
        }
        else
        {
            particleSystem = std::move(loaded);
            engine->Clear();
            engine->OnParticleSystemChanged(-1);
            engine->ReloadTextures();
            // Loading only populates the effect *definition*; nothing
            // emits until a live instance is spawned (the editor does
            // this via the SpawnerDriver, default Auto+disabled). Fire
            // one manual burst at the origin with no lifetime cap so
            // the system's emitters keep filling for the whole capture.
            if (spawnerDriver)
            {
                SpawnerConfig cfg;
                cfg.mode           = SpawnerConfig::Mode::Manual;
                cfg.burstSize      = 1;
                cfg.position       = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
                cfg.maxLifetimeSec = 0.0f;  // no cap — emit through the capture
                spawnerDriver->SetConfig(cfg);
                // Deterministic sim for the render-golden lane, same recipe as
                // the --record path (ClipRunner):
                //  (a) fixed PRNG seed — seeded HERE (after load/mod-scan)
                //      because expat re-seeds the CRT PRNG with entropy on the
                //      first XML_Parse per thread (GameObjectCatalog.cpp
                //      footgun), so an earlier seed doesn't survive;
                //  (b) frozen preview clock, stepped exactly 1/60 s per COUNTED
                //      frame in the capture loop below. Without the freeze the
                //      sim runs on wall time, and the layout gate holds the
                //      frame counter for a machine-load-dependent while — so
                //      the sim time in the captured frame varied run-to-run
                //      for any time-evolving fixture (bit-identical was only
                //      ever true for time-stable scenes). Begin the spawner
                //      explicitly at the frozen anchor; zero-delta render ticks
                //      are true no-ops so paused interactive scenes stay paused.
                srand(0x5EEDu);
                FreezePreviewClockAt(0.0f);   // fixed anchor: m_time-consuming shaders render identical every run
                spawnerDriver->Trigger(particleSystem.get(), engine.get());
                spawnerDriver->Tick(1.0f / 60.0f, particleSystem.get(), engine.get());
            }
            // Apply the requested skydome slot so a --capture run can render
            // (and verify) particles over a background skydome. Slot 0
            // (default) leaves the solid-colour background untouched.
            if (m_captureSkydomeSlot > 0)
            {
                const bool sok = engine->SetSkydomeSlot(m_captureSkydomeSlot);
                Log("[capture] skydome slot %d -> %s\n",
                    m_captureSkydomeSlot, sok ? "ok" : "FAILED");
            }
            // Honor the persisted ShowGround setting in headless --capture too.
            // The host path (unlike main.cpp startup) never read it, so the
            // ground was always drawn; a clean background (registry ShowGround=0)
            // lets a capture isolate the sprite — e.g. the spin test, where
            // terrain-through-transparency otherwise contaminates the read.
            {
                HKEY hKey; DWORD gval = 1, gsz = sizeof(gval), gtype = 0;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AloParticleEditor",
                                  0, KEY_READ, &hKey) == ERROR_SUCCESS)
                {
                    if (RegQueryValueExW(hKey, L"ShowGround", NULL, &gtype,
                                         (LPBYTE)&gval, &gsz) == ERROR_SUCCESS
                        && gtype == REG_DWORD)
                    {
                        engine->SetGround(gval != 0);
                        Log("[capture] ShowGround=%lu (from registry)\n", gval);
                    }
                    RegCloseKey(hKey);
                }
            }
            // Harness: orbit the capture camera by CaptureCamYaw (about Up) then
            // CaptureCamPitch (about camera-right), and optionally scale distance by
            // CaptureCamDist (x1000). Yaw/pitch are signed centidegrees stored as the
            // raw uint32 bit pattern (read back via (int)). Absent keys = no change.
            // Lets a headless sweep render R(theta) for the de-flicker metric.
            {
                HKEY hKey; DWORD raw, gsz, gtype;
                int yawC = 0, pitchC = 0; DWORD distR = 0;
                bool haveYaw = false, havePitch = false, haveDist = false;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AloParticleEditor",
                                  0, KEY_READ, &hKey) == ERROR_SUCCESS)
                {
                    gsz = sizeof(raw);
                    if (RegQueryValueExW(hKey, L"CaptureCamYaw",   NULL, &gtype, (LPBYTE)&raw, &gsz) == ERROR_SUCCESS && gtype == REG_DWORD) { yawC   = (int)raw; haveYaw   = true; }
                    gsz = sizeof(raw);
                    if (RegQueryValueExW(hKey, L"CaptureCamPitch", NULL, &gtype, (LPBYTE)&raw, &gsz) == ERROR_SUCCESS && gtype == REG_DWORD) { pitchC = (int)raw; havePitch = true; }
                    gsz = sizeof(raw);
                    if (RegQueryValueExW(hKey, L"CaptureCamDist",  NULL, &gtype, (LPBYTE)&raw, &gsz) == ERROR_SUCCESS && gtype == REG_DWORD) { distR  = raw;      haveDist  = true; }
                    RegCloseKey(hKey);
                }
                if (haveYaw || havePitch || haveDist)
                {
                    Engine::Camera cam = engine->GetCamera();
                    D3DXVECTOR3 up = cam.Up; D3DXVec3Normalize(&up, &up);
                    D3DXVECTOR3 diff = cam.Position - cam.Target;
                    if (haveDist && distR > 0) diff *= (float)distR / 1000.0f;
                    if (haveYaw)
                    {
                        D3DXMATRIX r; D3DXMatrixRotationAxis(&r, &up, D3DXToRadian((float)yawC / 100.0f));
                        D3DXVec3TransformCoord(&diff, &diff, &r);
                    }
                    if (havePitch)
                    {
                        // right = up x diff; degenerate (zero-length) when the view is
                        // (near-)parallel to Up, i.e. looking straight down/up -- pitch is
                        // then undefined, so skip it rather than normalize a garbage/NaN axis.
                        D3DXVECTOR3 right; D3DXVec3Cross(&right, &up, &diff);
                        if (D3DXVec3Length(&right) > 1e-4f)
                        {
                            D3DXVec3Normalize(&right, &right);
                            D3DXMATRIX r; D3DXMatrixRotationAxis(&r, &right, D3DXToRadian((float)pitchC / 100.0f));
                            D3DXVec3TransformCoord(&diff, &diff, &r);
                        }
                        else
                            Log("[capture] pitch skipped: view parallel to Up (gimbal singularity)\n");
                    }
                    cam.Position = cam.Target + diff;
                    engine->SetCamera(cam);
                    Log("[capture] cam yaw=%.2f pitch=%.2f dist=x%.3f\n",
                        (float)yawC / 100.0f, (float)pitchC / 100.0f, haveDist ? distR / 1000.0f : 1.0f);
                }
            }
            // [world-lit] Drive scene lighting from the --ambient / --sun /
            // --sun-intensity flags. Mirrors the registry [lighting-restore]
            // shape: ambient pushes w=1; the sun Light folds intensity into
            // Diffuse/Specular and derives Position from a fixed z/tilt
            // (z=0, tilt=45 — same default the restore block uses), so a lit
            // shader's per-vertex response can be verified offline.
            if (m_captureHasAmbient)
            {
                engine->SetAmbient(D3DXVECTOR4(m_captureAmbient[0],
                                               m_captureAmbient[1],
                                               m_captureAmbient[2], 1.0f));
                Log("[capture] ambient %.3f,%.3f,%.3f\n",
                    m_captureAmbient[0], m_captureAmbient[1], m_captureAmbient[2]);
            }
            if (m_captureHasSun)
            {
                const float intensity = m_captureHasSunI ? m_captureSunIntensity : 1.0f;
                const float zr = D3DXToRadian(0.0f);
                const float tr = D3DXToRadian(45.0f);
                const float c  = cosf(tr);
                Engine::Light L = {};
                L.Position  = D3DXVECTOR4(c * cosf(zr), c * sinf(zr), sinf(tr), 0.0f);
                L.Direction = D3DXVECTOR4(0, 0, 0, 0);
                L.Diffuse   = D3DXVECTOR4(m_captureSun[0] * intensity,
                                          m_captureSun[1] * intensity,
                                          m_captureSun[2] * intensity, 1.0f);
                L.Specular  = D3DXVECTOR4(m_captureSun[0] * intensity,
                                          m_captureSun[1] * intensity,
                                          m_captureSun[2] * intensity, 1.0f);
                engine->SetLight(Engine::LT_SUN, L);
                Log("[capture] sun %.3f,%.3f,%.3f intensity=%.3f\n",
                    m_captureSun[0], m_captureSun[1], m_captureSun[2], intensity);
            }
            Log("[capture] loaded %ls; spawned instance; rendering %d frames -> %ls\n",
                m_captureAlo.c_str(), m_captureFrames, m_capturePng.c_str());
        }
    }

    // [redbug-diag] Optionally compose a CATALOG reference object + its cast shadow with
    // the particle system, so the particle-over-reference-shadow interaction can be
    // measured headlessly. ALO_CAPTURE_REFOBJECT=<GameObject name> (e.g. AT_AT_Walker);
    // ALO_CAPTURE_SHADOWS=0/1 toggles model shadows (default leaves engine default = on).
    {
        char refName[256];
        if (engine && GetEnvironmentVariableA("ALO_CAPTURE_REFOBJECT", refName, sizeof(refName)) > 0)
        {
            engine->BuildCatalogSync();
            engine->SetReferenceObject(refName);
            engine->SetReferenceObjectVisible(true);
            const ReferenceObjectStatus rs = engine->GetReferenceObjectStatus();
            Log("[redbug] ref object '%s' status=%d shadowSubMeshes=%zu\n",
                refName, (int)rs, engine->ReferenceShadowSubMeshCount());
            D3DXVECTOR3 wmin, wmax;
            if (rs == ReferenceObjectStatus::Ok && engine->GetReferenceObjectBounds(wmin, wmax))
            {
                // Fit camera to the ref-object AABB (mirrors the --capture-ref framing) so
                // both the model+shadow and the origin-spawned particles are in view.
                D3DXVECTOR3 center((wmin.x+wmax.x)*0.5f,(wmin.y+wmax.y)*0.5f,(wmin.z+wmax.z)*0.5f);
                D3DXVECTOR3 diag = wmax - wmin;
                float radius = 0.5f * D3DXVec3Length(&diag); if (radius < 1.0f) radius = 1.0f;
                const float fovY = D3DXToRadian(45.0f);
                float dist = radius / tanf(0.5f * fovY) * 1.6f;
                D3DXVECTOR3 dir(0.7f,-0.7f,0.5f); D3DXVec3Normalize(&dir,&dir);
                Engine::Camera cam; cam.Position = center + dir*dist; cam.Target = center; cam.Up = D3DXVECTOR3(0,0,1);
                engine->SetCamera(cam);
                Log("[redbug] fit camera center=(%.1f,%.1f,%.1f) radius=%.1f dist=%.1f\n",
                    center.x,center.y,center.z,radius,dist);
            }
            else
            {
                // Startup-time status only: the catalog mesh resolves a frame or more later
                // (deferred), so a non-Ok status HERE is not necessarily a failure — the
                // camera fit is skipped (default camera) and the object resolves during the
                // render loop. A truly-bogus ref that NEVER resolves is caught downstream:
                // RenderReferenceShadows never draws, so its [redbug-shadow] marker is absent
                // and the durable guard fails. Don't
                // hard-fail here — that would also kill a valid-but-still-resolving object.
                Log("[redbug] ref object '%s' not resolved at startup (status=%d); camera fit "
                    "skipped, will resolve during the render loop\n", refName, (int)rs);
            }
        }
        char shadowBuf[8];
        if (engine && GetEnvironmentVariableA("ALO_CAPTURE_SHADOWS", shadowBuf, sizeof(shadowBuf)) > 0)
        {
            const bool on = atoi(shadowBuf) != 0;
            engine->SetModelShadows(on);
            Log("[redbug] model shadows forced %s\n", on ? "ON" : "OFF");
        }
    }

    // [shadow-repro] Optional camera-distance override for headless bug
    // characterisation: ALO_CAPTURE_CAM_DIST_MULT scales a fit-camera distance
    // so the SAME reference object can be rendered at near/mid/far — the axis
    // the camera-distance shadow drift lives on. Inert unless the env is set
    // AND a reference object resolved renderable bounds. Reuses the
    // --capture-ref 3/4 fit framing so the shadow contact is clearly visible.
    if (!captureFailed && engine)
    {
        char mbuf[64];
        if (GetEnvironmentVariableA("ALO_CAPTURE_CAM_DIST_MULT", mbuf, sizeof(mbuf)) > 0)
        {
            float mult = (float)atof(mbuf);
            D3DXVECTOR3 wmin, wmax;
            if (mult > 0.0f && engine->GetReferenceObjectBounds(wmin, wmax))
            {
                D3DXVECTOR3 center((wmin.x + wmax.x) * 0.5f,
                                   (wmin.y + wmax.y) * 0.5f,
                                   (wmin.z + wmax.z) * 0.5f);
                D3DXVECTOR3 diag = wmax - wmin;
                float radius = 0.5f * D3DXVec3Length(&diag);
                if (radius < 1.0f) radius = 1.0f;
                const float fovY = D3DXToRadian(45.0f);
                float dist = radius / tanf(0.5f * fovY) * 1.6f * mult;
                D3DXVECTOR3 dir(0.7f, -0.7f, 0.5f);
                D3DXVec3Normalize(&dir, &dir);
                Engine::Camera cam;
                cam.Position = center + dir * dist;
                cam.Target   = center;
                cam.Up       = D3DXVECTOR3(0.0f, 0.0f, 1.0f);
                engine->SetCamera(cam);
                Log("[shadow-repro] cam-dist-mult=%.2f -> dist=%.1f eye=(%.1f,%.1f,%.1f)\n",
                    mult, dist, cam.Position.x, cam.Position.y, cam.Position.z);
            }
        }
    }

#ifndef NDEBUG
    // [shadow-repro] ALO_CAPTURE_SUBVIEWPORT=1 insets the scene viewport to a
    // sub-rect of the backbuffer, mimicking the live editor's panel-inset 3D view
    // — the condition under which the soft-shadow composite's mask-UV mapping
    // matters (the "floating silhouette" bug is invisible at a full-RT viewport).
    if (!captureFailed && engine)
    {
        char sv[8];
        if (GetEnvironmentVariableA("ALO_CAPTURE_SUBVIEWPORT", sv, sizeof(sv)) > 0 && atoi(sv) != 0)
        {
            D3DVIEWPORT9 fv = {};
            engine->GetViewPort(&fv);
            const int ix = fv.X + (int)(0.27f * fv.Width);
            const int iy = fv.Y + (int)(0.10f * fv.Height);
            const int iw = (int)(0.66f * fv.Width);
            const int ih = (int)(0.62f * fv.Height);
            engine->SetSceneViewport(ix, iy, iw, ih);
            Log("[shadow-repro] sub-viewport x=%d y=%d w=%d h=%d (was %ux%u)\n",
                ix, iy, iw, ih, fv.Width, fv.Height);
        }
    }
#endif
}

CaptureRunner::TickResult CaptureRunner::Tick()
{
    // Old Run()-scope / Impl-member names -> Deps references (verbatim-move
    // aliases; m_uiReady / m_sceneRectSeen stay owned by HostWindowImpl).
    auto&       alphaCompositor = m_deps.alphaCompositor;
    const HWND  hMain           = m_deps.hMain;
    const bool& m_uiReady       = m_deps.uiReady;
    const bool& m_sceneRectSeen = m_deps.sceneRectSeen;
    bool&       quit            = m_quit;

            // Pace the sim with a fixed ~16 ms wall-clock step so
            // RenderD3D9's real-time dt advances particles a useful
            // amount per frame (the uncapped pump would otherwise run
            // dozens of frames in a few ms, leaving particles bunched
            // at the spawn point and never overlapping — which is
            // exactly the additive-over-smoke case we need to see).
            Sleep(16);
            // Layout-determinism gate: hold the frame counter until React's
            // first paint AND first layout/scene-rect have landed — the
            // scene-rect resizes the engine RT, so counting from process
            // start raced it and captures came out at the pre- OR
            // post-layout size depending on system load. 10 s cap so a
            // changed UI degrades to the old (ungated) behavior, loudly.
            if (!(m_uiReady && m_sceneRectSeen))
            {
                if (m_captureGateStartQpc == 0) m_captureGateStartQpc = PerfQpcNow();
                const LONGLONG gqf = PerfQpcFreq();
                const double heldMs = gqf > 0
                    ? QpcMs(PerfQpcNow() - m_captureGateStartQpc, gqf)
                    : 10000.0;
                if (heldMs < 10000.0)
                    return TickResult::Running;
                if (!m_captureGateWarned)
                {
                    m_captureGateWarned = true;
                    Log("[capture] layout gate timed out (uiReady=%d sceneRect=%d) — proceeding ungated\n",
                        (int)m_uiReady, (int)m_sceneRectSeen);
                    // Also on stdout: an ungated capture is racy-sized, so
                    // golden consumers (scripts/render-goldens.mjs) must be
                    // able to SEE the degradation and fail the scene rather
                    // than flake against a fixed-size golden.
                    printf("[capture] layout-gate-timeout — capture size may be pre-layout\n");
                    fflush(stdout);
                }
            }
            // Advance the frozen sim clock by exactly one 60 Hz frame per
            // COUNTED frame (no-op unless the capture spawn path paused the
            // preview clock above). Placed after the layout gate so gate-held
            // pump frames render the frozen scene without advancing sim time —
            // the captured frame is then always at sim time capturedFrames/60,
            // independent of UI cold-start duration. Consumed by the NEXT
            // RenderD3D9 at the top of the loop.
            StepPreviewFrames(1);
            if (++capturedFrames >= m_captureFrames)
            {
                // (1) engine RT — UNCHANGED: the engine's own pre-composite
                // pixels, captured at the exact frame target via the exact
                // method. Only the composite below is gated on the UI.
                const bool ok = alphaCompositor &&
                                alphaCompositor->CaptureSnapshotToFile(m_capturePng);
                if (!ok) captureFailed = true;

                // (2) composite — the final DWM/DComp-composited window
                // (engine viewport framed by React chrome). Gate it on the
                // app/ready first-paint signal so it captures real chrome,
                // not a blank WebView surface. The per-PID-isolated WebView2
                // profile makes every run a genuine browser cold start, so
                // the wait is real: pump + render while waiting; cap at 30s
                // so a hung UI still yields a best-effort (clearly-named) image.
                const LONGLONG qf = PerfQpcFreq();          // cache once (freq==0 guard)
                const LONGLONG waitStart = PerfQpcNow();
                const double   kUiTimeoutMs = 30000.0;
                bool timedOut = false;
                int  waitIters = 0;
                while (!m_uiReady && !quit)
                {
                    // Drain FIRST — DispatchMessage is what delivers WebView2's
                    // WebMessageReceived → OnWebMessage → m_uiReady.
                    MSG mw;
                    while (PeekMessage(&mw, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&mw);
                        DispatchMessage(&mw);
                        if (mw.message == WM_QUIT) quit = true;
                    }
                    if (m_uiReady || quit) break;
                    const double elapsedMs = qf > 0
                        ? QpcMs(PerfQpcNow() - waitStart, qf)
                        : static_cast<double>(++waitIters) * 16.0;  // QPC-dead fallback
                    if (elapsedMs >= kUiTimeoutMs) { timedOut = true; break; }
                    RenderD3D9();  // keep the composed surface coherent while waiting
                    MsgWaitForMultipleObjectsEx(0, nullptr, 16,
                                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }

                // Settle: after a positive signal, pump + render ~150 ms so
                // DComp has committed the WebView visual AND the deferred
                // scene-rect crop (SetEngineVisualTransform immediate=false,
                // applied at the next CompositeEngineFrame) has landed before
                // the snapshot. app/ready proves React painted, not that the
                // host-side composition has caught up.
                if (m_uiReady && !quit)
                {
                    const LONGLONG settleStart = PerfQpcNow();
                    for (int i = 0; !quit; ++i)
                    {
                        MSG mw;
                        while (PeekMessage(&mw, nullptr, 0, 0, PM_REMOVE))
                        {
                            TranslateMessage(&mw);
                            DispatchMessage(&mw);
                            if (mw.message == WM_QUIT) quit = true;
                        }
                        if (quit) break;
                        const double settleMs = qf > 0
                            ? QpcMs(PerfQpcNow() - settleStart, qf)
                            : static_cast<double>(i) * 16.0;
                        if (settleMs >= 150.0) break;
                        RenderD3D9();
                        Sleep(16);
                    }
                }

                const double waitedMs = qf > 0
                    ? QpcMs(PerfQpcNow() - waitStart, qf)
                    : static_cast<double>(waitIters) * 16.0;
                // Success name only when React actually signalled first
                // paint; a timeout OR an external WM_QUIT before the signal
                // yields a degraded image under a DISTINCT name so it can
                // never be mistaken for a good one (the harness greps for the
                // non-TIMEOUT name + requires ui-ready=1).
                const wchar_t* suffix = m_uiReady ? L"-composite" : L"-composite-TIMEOUT";
                const char*    state  = m_uiReady ? "" : (timedOut ? " TIMEOUT" : " ABORTED");
                const std::wstring compPath = DeriveSibling(m_capturePng, suffix);
                // Composite is UNCONDITIONAL (attempted even if engine-RT
                // failed) — the diagnostic composite is most valuable exactly
                // when a render broke. quit is set AFTER it so the loop exits
                // via `if (quit) break;` before the captureFailed bail.
                const bool okc = host::CaptureWindowToPng(hMain, compPath);
                Log("[capture] frame %d: engine-RT %ls -> %s; composite %ls -> %s "
                    "(ui-ready=%d waited=%.0fms%s)\n",
                    capturedFrames, m_capturePng.c_str(), ok ? "ok" : "FAILED",
                    compPath.c_str(), okc ? "ok" : "FAILED",
                    m_uiReady ? 1 : 0, waitedMs, state);
                if (!m_uiReady)
                    Log("[capture] WARNING: app/ready not received (%s) — composite "
                        "may show an unpainted React surface\n",
                        timedOut ? "30s timeout" : "window closed mid-wait");
                // Exit code stays engine-RT-driven (captureFailed set above);
                // a UI timeout is a host.log WARNING, not a process failure.
                quit = true;
            }

    return quit ? TickResult::Done : TickResult::Running;
}

}  // namespace host
