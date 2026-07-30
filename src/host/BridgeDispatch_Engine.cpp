// Kind handlers for the engine/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "StringConv.h"           // host::Utf8ToWide / WideToUtf8
#include "../ModManager.h"      // activeModPath in the state snapshot
#include "../Rescale.h"              // DoRescaleEmitter (engine/action/rescale-*)
#include "../RefTransformUndoKey.h"  // RefTransformCoalesceKey (set/reference-object-transform)

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchEngine(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // Internal native-test seam: hold the real Engine device-work gate while
    // production actions queue work, then release through the normal frame
    // preparation door. It intentionally exposes no direct replay/counter
    // mutation.
    if (kind == "debug/device-recovery-work")
    {
        if (!m_testHost)
        {
            ctx.SendErr("debug/device-recovery-work requires --test-host");
            return true;
        }
        if (!ctx.RequireEngine(kind.c_str())) return true;

        const std::string action =
            params.value("action", std::string("query"));
        bool frameReady = !m_engine->DeviceCallsBlocked();
        if (action == "arm")
        {
            if (!m_engine->SetDeviceRecoveryWorkHoldForTesting(true))
            {
                ctx.SendErr("device recovery work hold requires a healthy engine");
                return true;
            }
            frameReady = false;
        }
        else if (action == "release")
        {
            m_engine->SetDeviceRecoveryWorkHoldForTesting(false);
            frameReady = m_engine->PrepareDeviceForFrame();
        }
        else if (action == "inject-query-result")
        {
            const std::string result =
                params.value("result", std::string());
            const int repeatCount = params.value("repeatCount", 1);
            if (repeatCount < 1 || repeatCount > 100001)
            {
                ctx.SendErr("query result repeatCount must be 1..100001");
                return true;
            }

            HRESULT queryResult = S_OK;
            if (result == "s-ok")
                queryResult = S_OK;
            else if (result == "s-false")
                queryResult = S_FALSE;
            else if (result == "device-lost")
                queryResult = D3DERR_DEVICELOST;
            else
            {
                ctx.SendErr("unknown end-frame query result");
                return true;
            }
            m_engine->InjectEndFrameQueryResultForTesting(
                queryResult,
                static_cast<uint32_t>(repeatCount));
        }
        else if (action == "clear-query-result")
        {
            m_engine->InjectEndFrameQueryResultForTesting(S_OK, 0);
        }
        else if (action != "query")
        {
            ctx.SendErr("unknown device recovery work action");
            return true;
        }

        ctx.SendOk({
            {"pending", m_engine->TextureReloadPendingForTesting()},
            {"reloadCount", m_engine->TextureReloadApplyCountForTesting()},
            {"authoredApplyCount",
             m_engine->ParticleSystemChangeApplyCountForTesting()},
            {"deviceProbeCount",
             m_engine->DeviceStateProbeCountForTesting()},
            {"composedFramePrepareCount",
             m_engine->ComposedFramePrepareCountForTesting()},
            {"endFrameQueryCreateCount",
             m_engine->EndFrameQueryCreateCountForTesting()},
            {"endFrameQueryFailureCount",
             m_engine->EndFrameQueryFailureCountForTesting()},
            {"endFrameQueryTimeoutCount",
             m_engine->EndFrameQueryTimeoutCountForTesting()},
            {"endFrameQueryOverrideConsumedCount",
             m_engine->EndFrameQueryOverrideConsumedCountForTesting()},
            {"endFrameQueryOverrideRemaining",
             m_engine->EndFrameQueryOverrideRemainingForTesting()},
            {"frameReady", frameReady},
        });
        return true;
    }

    // -------- engine/state/snapshot --------
    if (kind == "engine/state/snapshot")
    {
        if (!ctx.RequireEngine("snapshot")) return true;
        // Spawner field: prefer the live driver config (host-state
        // plumbing), fall back to the JSON cache
        // when no driver is bound (e.g. unit tests, partial
        // wiring during construction).
        json spawnerJson = m_spawnerDriver
            ? SpawnerConfigToJson(m_spawnerDriver->GetConfig())
            : m_spawnerConfig;
        const std::wstring activeModPath = m_modManager ? m_modManager->GetPrimaryLayerPath() : std::wstring();
        const bool leaveParticles = (m_pParticleSystem != nullptr && *m_pParticleSystem)
            ? (*m_pParticleSystem)->getLeaveParticles()
            : true;
        const bool canUndo = ComputeCanUndo();
        const bool canRedo = m_undo ? m_undo->CanRedo() : false;
        ctx.SendOk(BuildEngineStateSnapshot(m_engine, m_currentFilePath, m_dirty, spawnerJson, m_selectedEmitterId, activeModPath, leaveParticles, canUndo, canRedo));
        return true;
    }


    // -------- engine/set/* (17 handlers) --------
    if (kind == "engine/set/ground")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        // Ground visibility is a GLOBAL VIEW preference (registry-persisted,
        // restored on launch; not part of the .alo document) — so, like the
        // sibling view toggles (paused / overload-guard / msaa / model-shadows),
        // it must NOT mark the document dirty. Persist it so a toggled-off
        // ground survives restart (#617).
        const bool enabled = params.value("enabled", false);
        m_engine->SetGround(enabled);
        ctx.SendOk(json::object());
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistShowGround(enabled);
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/ground-z")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetGroundZ(params.value("z", 0.0f));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/ground-texture")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        // SetGroundTexture refuses an unavailable slot (no game install) and stays put.
        // Report the truth: echo the slot actually in effect + whether it applied, so a
        // caller awaiting this reply isn't told "ok" for a selection that bounced to dirt.
        const bool applied = m_engine->SetGroundTexture(params.value("slot", 0));
        ctx.SendOk({ {"slot", m_engine->GetGroundTexture()}, {"applied", applied} });
        if (applied) ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/ground-solid-color")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        unsigned int rgb = params.value("rgb", 0u);
        m_engine->SetGroundSolidColor(static_cast<COLORREF>(rgb));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/ground-slot-custom-path")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        int slot = params.value("slot", -1);
        std::string p = params.value("path", std::string{});
        // The setter's bool was dropped here, so a refused path still answered
        // {ok:true} — the an-audit-finding shape, telling the user it took when it did not
        // (2026-07 audit, an-audit-finding).
        if (!m_engine->SetGroundSlotCustomPath(slot, Utf8ToWide(p)))
        {
            ctx.SendErr("ground slot path rejected: bad slot, or a network/device path");
            return true;
        }
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/skydome-slot")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        const int slot = params.value("slot", 0);
        m_engine->SetSkydomeSlot(slot);
        // Persist so the selection survives restart in the new UI
        // (the handler previously only markDirty'd — the daily-driver lost it).
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistSkydomeIndex(slot);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/skydome-custom-path")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        int slot = params.value("slot", -1);
        std::string p = params.value("path", std::string{});
        std::wstring wpath = Utf8ToWide(p);
        // Check BEFORE persisting: writing a refused path to the registry is
        // what made this the durable half of an-audit-finding — the startup restore would
        // replay it on every launch.
        if (!m_engine->SetSkydomeCustomPath(slot, wpath))
        {
            ctx.SendErr("skydome slot path rejected: bad slot, or a network/device path");
            return true;
        }
        // Persist the custom slot path (round-trips with legacy).
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistSkydomeCustomPath(slot, wpath);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/skydome-environment")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::string ctxStr = params.value("context", std::string{"space"});
        std::string prim   = params.value("primaryName", std::string{});
        std::string sec    = params.value("secondaryName", std::string{});
        // Renamed from `ctx` on the TU move — it would shadow the
        // BridgeRequestContext parameter.
        SkydomeContext skyCtx = (ctxStr == "land") ? SkydomeContext::Land : SkydomeContext::Space;
        m_engine->SetSkydomeEnvironment(skyCtx, prim, sec);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistSkydomeEnvironment(skyCtx == SkydomeContext::Land ? 0 : 1,
                                      Utf8ToWide(prim), Utf8ToWide(sec));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    // imported reference object: select by Name, toggle visibility, set transform.
    if (kind == "engine/set/reference-object")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::string name = params.value("name", std::string{});
        m_engine->SetReferenceObject(name);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
        {
            PersistReferenceObjectName(Utf8ToWide(name));
            // The swap may have changed the live transform (per-object memory:
            // restore this object's placement, or origin for a never-moved unit).
            // Persist it so the single global ReferenceObjectTransform in the
            // registry tracks the SHOWN object -- otherwise a stale transform from a
            // previous object would be restored onto this one at next startup and
            // float it (the cross-restart tail of the same bug).
            PersistReferenceObjectTransform(m_engine->GetReferencePosition(),
                                            m_engine->GetReferenceRotation());
        }
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/reference-object-visible")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        bool visible = params.value("visible", true);
        m_engine->SetReferenceObjectVisible(visible);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistReferenceObjectVisible(visible);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/reference-object-lock")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        bool locked = params.value("locked", false);
        m_engine->SetReferenceLocked(locked);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistReferenceObjectLock(locked);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/reference-object-transform")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        // Drop a UI-routed transform request against a locked
        // object — the picker already disables the spinners + Reset, so this is the
        // backstop for a stale/racing request. Returning BEFORE the undo capture
        // below means a dropped request also pushes no phantom undo step. This gates
        // only the UI path: the gizmo-drag path can't reach a locked object (it isn't
        // selectable, so no handle is grabbable), and engine-internal restores
        // (ApplyUndoSnapshot) intentionally still move it, so SetReferenceObjectTransform
        // itself stays ungated.
        if (m_engine->IsReferenceLocked()) { ctx.SendOk(json::object()); return true; }
        D3DXVECTOR3 pos = JsonToVec3(params.value("position", json::array()));
        D3DXVECTOR3 rot = JsonToVec3(params.value("rotation", json::array()));
        // PRE-mutation undo capture, keyed PER changed component
        // (see RefTransformUndoKey.h): an X-spinner edit and a Y-spinner edit
        // are separate undo steps; a no-op set (e.g. Reset on an already-origin
        // object) returns key 0 and we push NO entry. ctx.RequireEngine() above
        // guarantees m_engine here, so the getter reads are safe.
        {
            const D3DXVECTOR3 cp = m_engine->GetReferencePosition();
            const D3DXVECTOR3 cr = m_engine->GetReferenceRotation();
            const float inc[6] = { pos.x, pos.y, pos.z, rot.x, rot.y, rot.z };
            const float cur[6] = { cp.x,  cp.y,  cp.z,  cr.x,  cr.y,  cr.z  };
            const DWORD refKey = RefTransformCoalesceKey(inc, cur);
            if (refKey != 0) CaptureUndoPoint(refKey);
        }
        m_engine->SetReferenceObjectTransform(pos, rot);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistReferenceObjectTransform(pos, rot);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    // unit grid: toggle + spacing.
    if (kind == "engine/set/grid-visible")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        bool visible = params.value("visible", false);
        m_engine->SetGridVisible(visible);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistGrid(visible, m_engine->GetGridSpacing());
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/grid-spacing")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        float spacing = params.value("spacing", 20.0f);
        m_engine->SetGridSpacing(spacing);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistGrid(m_engine->GetGridVisible(), m_engine->GetGridSpacing());
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    // Persistent gizmo snap toggle. State + round-trip only; the drag-time
    // apply (reads GetSnapEnabled) is a separate task.
    if (kind == "engine/set/snap-enabled")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        bool enabled = params.value("enabled", false);
        m_engine->SetSnapEnabled(enabled);
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistSnap(enabled);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/background")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        unsigned int rgb = params.value("rgb", 0u);
        m_engine->SetBackground(static_cast<COLORREF>(rgb));
        // Persist the solid-colour background (same Background picker as
        // the skydome; previously only markDirty'd, so it was lost on restart).
        if (!m_ephemeral && !(m_testHost && !m_settingsLive))
            PersistBackgroundColor(static_cast<COLORREF>(rgb));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/bloom")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetBloom(params.value("enabled", false));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/bloom-strength")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetBloomStrength(params.value("v", 0.0f));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/bloom-cutoff")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetBloomCutoff(params.value("v", 0.0f));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/bloom-size")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetBloomSize(params.value("v", 0.0f));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    // Task 2.7 — leave particles after instance death. Persisted with
    // the ParticleSystem (chunk-serialised at [ParticleSystem.cpp:948])
    // so dirty must flip. Engine::KillParticleSystem honors the flag at
    // [src/engine.cpp:197].
    if (kind == "engine/set/leave-particles")
    {
        bool enabled = params.value("enabled", true);
        if (m_pParticleSystem != nullptr && *m_pParticleSystem)
        {
            ParticleSystem* sys = m_pParticleSystem->get();
            if (sys->getLeaveParticles() == enabled)
            {
                ctx.SendOk(json::object());
                return true;
            }

            // This flag is serialized document state, so snapshot the exact
            // pre-toggle value. A same-value request is not an edit and must
            // not manufacture an undo step or dirty the document.
            captureUndo();
            sys->setLeaveParticles(enabled);
            ctx.SendOk(json::object());
            ctx.MarkDirty();
            EmitEngineStateChanged();
        }
        else
        {
            // G3: HARD FAIL — success path is a bare ctx.SendOk(json::object())
            // with no nested ok, so no caller reads nested ok here. Sibling
            // engine/set/* handlers already sendErr (via requireEngine) for
            // the not-ready case from the same `void bridge.request` call
            // sites; converting aligns this outlier with them.
            ctx.SendErr("no particle system bound");
        }
        return true;
    }
    if (kind == "engine/set/heat-debug")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetHeatDebug(params.value("enabled", false));
        ctx.SendOk(json::object());
        // heat-debug is a view-only debug overlay. Don't mark dirty.
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/camera")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        Engine::Camera cam;
        cam.Position = JsonToVec3(params.value("position", json::array()));
        cam.Target   = JsonToVec3(params.value("target",   json::array()));
        cam.Up       = JsonToVec3(params.value("up",       json::array()));
        m_engine->SetCamera(cam);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/light")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::string which = params.value("which", std::string{"sun"});
        Engine::Light l;
        l.Diffuse   = JsonToVec4(params.value("diffuse",   json::array()));
        l.Specular  = JsonToVec4(params.value("specular",  json::array()));
        l.Position  = JsonToVec4(params.value("position",  json::array()));
        l.Direction = JsonToVec4(params.value("direction", json::array()));
        m_engine->SetLight(ParseLightWhich(which), l);
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/ambient")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetAmbient(JsonToVec4(params.value("color", json::array())));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/shadow")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetShadow(JsonToVec4(params.value("color", json::array())));
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    // View state (preview clock). SetPreviewPaused is a free function in
    // engine.h — sibling to IsPreviewPaused/StepPreviewFrames. Doesn't
    // touch Engine state, but the snapshot reads paused so a broadcast
    // keeps any subscriber's mirror in sync.
    if (kind == "engine/set/paused")
    {
        SetPreviewPaused(params.value("paused", false));
        ctx.SendOk(json::object());
        // paused is a view-only toggle (preview clock). Don't mark dirty.
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/set/overload-guard")
    {
        // [guard-config] View-only preview setting (like engine/set/paused):
        // never marks the document dirty. The engine clamps maxParticles
        // defensively; we cache pre-clamp intent for SetEngine reapply
        // (the engine re-clamps on every apply, so the cache needs no
        // clamping of its own).
        const bool enabled      = params.value("enabled", true);
        const int  maxParticles = params.value("maxParticles", 15'000);
        m_overloadGuardCached       = true;
        m_overloadGuardEnabled      = enabled;
        m_overloadGuardMaxParticles = maxParticles;
        if (m_engine) m_engine->SetOverloadGuard(enabled, maxParticles);
        ctx.SendOk(json::object());
        return true;
    }

    // -------- engine/set/msaa-level ----------------------------------
    // View-only display preference: never marks the document dirty.
    // Delegates directly to Engine::SetMsaaLevel; no caching needed
    // because MSAA capability is hardware-fixed and the level is
    // re-applied each time the engine rebuilds its swap chain.
    if (kind == "engine/set/msaa-level")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        const int level = params.value("level", 0);
        m_engine->SetMsaaLevel(level);
        ctx.SendOk(json::object());
        return true;
    }

    // -------- engine/set/model-shadows (view-only render preference) --------
    // "Model shadows": enables/disables the stencil shadow-volume pass for the
    // reference object. A view preference -- never marks the document dirty.
    // Persisted web-side (localStorage).
    if (kind == "engine/set/model-shadows")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetModelShadows(params.value("enabled", true));
        ctx.SendOk(json::object());
        return true;
    }

    // -------- engine/set/soft-shadows (view-only render preference) --------
    // "Soft shadows": blurred shadow-mask composite vs the hard stencil darken.
    // Like model-shadows it is a view preference -- never marks the document dirty,
    // no DTO field. Persisted web-side (localStorage). Only meaningful when model
    // shadows are on; the engine falls back to hard when off/unavailable.
    if (kind == "engine/set/soft-shadows")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetSoftShadows(params.value("enabled", true));
        ctx.SendOk(json::object());
        return true;
    }

    // -------- engine/set/estimated-load (hard-guard) -----------------
    // Web-computed estimate of alive particles per placed instance
    // (chain-load.ts owns the formula — see the hard-guard spec). Cached
    // and reapplied on SetEngine like the guard config.
    if (kind == "engine/set/estimated-load")
    {
        double perInstance = params.value("perInstance", 0.0);
        if (perInstance < 0.0) perInstance = 0.0;
        m_estimatedLoadCached       = true;
        m_estimatedLoadPerInstance  = perInstance;
        if (m_engine) m_engine->SetEstimatedLoad(perInstance);
        ctx.SendOk(json::object());
        return true;
    }

    // -------- engine/action/reset-view-settings ----------------------
    //
    // Cascade reset for the View → Reset View Settings
    // menu. Mirrors the legacy main.cpp reset path: pushes engine defaults
    // for background, ground (visibility + Z + texture), bloom (off +
    // canonical strength/cutoff/size), and skydome (Off slot). Lighting
    // reset rides with the lighting reset (separate handler around Force Align).
    //
    // Defaults match the Engine constructor (engine.cpp:1690-1715) —
    // kept in sync by hand because there's only one canonical value
    // each. Editor state (current path, dirty bit, selection) is left
    // alone since it isn't a "view setting."
    if (kind == "engine/action/reset-view-settings")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->SetBackground(RGB(0x14, 0x08, 0x34));
        m_engine->SetGround(true);
        m_engine->SetGroundZ(0.0f);
        m_engine->SetGroundTexture(0);
        m_engine->SetBloom(false);
        m_engine->SetBloomStrength(0.00f);
        m_engine->SetBloomCutoff (0.90f);
        m_engine->SetBloomSize   (0.10f);
        m_engine->SetSkydomeSlot(0);  // "Off"
        ctx.SendOk(json::object());
        // No markDirty — these are view settings, not particle-system
        // mutations. The user-visible cascade is communicated by a
        // single state-changed broadcast.
        EmitEngineStateChanged();
        return true;
    }


    // -------- engine/action/* --------
    if (kind == "engine/action/clear")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->Clear();
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/action/reload-shaders")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        if (!m_engine->ReloadShaders())
        {
            ctx.SendErr("shader reload was refused or failed");
            return true;
        }
        // Only invalidate dependent discovery state after the requested reload
        // actually ran. A blocked device must not acknowledge the action or
        // clear caches for work it dropped.
        m_engine->InvalidateSkydomeListCache();
        ctx.SendOk(json::object());
        // No dirty: reload-shaders re-reads disk; user state is unchanged.
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/action/reload-textures")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->InvalidateSkydomeListCache();   // explicit disk re-read -> refresh skydome XML too
        m_engine->ReloadTextures();
        // [C3] Reload re-reads the same filenames with NEW pixels, so the
        // (stack, filename)-keyed preview LRU + thumb cache are stale even
        // though the stack didn't change. Drop both (the epoch bump also
        // invalidates any in-flight encode). Mirrors the web bumpTextureEpoch.
        TexturePalette::ClearBridgeThumbCache();
        PreviewCacheClear();
        ctx.SendOk(json::object());
        // No dirty: reload-textures re-reads disk; user state is unchanged.
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "engine/action/on-particle-system-changed")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        m_engine->OnParticleSystemChanged(params.value("track", 0));
        ctx.SendOk(json::object());
        // No engine/state/changed broadcast — engine re-renders next frame.
        return true;
    }
    // Advance the preview clock by N frames. The free function is a
    // no-op when not paused, so the React Toolbar disables the button in
    // that state; no need to guard it here. Don't broadcast a state
    // change — the action emits zero or more state ticks via the normal
    // render loop, and an immediate broadcast would misleadingly suggest
    // a sync-time mutation.
    if (kind == "engine/action/step-frames")
    {
        StepPreviewFrames(params.value("frames", 1));
        ctx.SendOk(json::object());
        return true;
    }
    // Rescale the active particle system by a duration / size percentage.
    // Iterates over every emitter in the live ParticleSystem and applies the
    // helper from src/Rescale.cpp.
    //
    // This handler used to carry a comment excusing the missing captureUndo()
    // as "a no-op until the broader capture wiring lands". That wiring HAS
    // landed — rescale-emitter twenty lines below has called captureUndo()
    // since — but the stale comment read as a deliberate accepted limitation,
    // so every reader skipped past it and one Ctrl+Z after a whole-system
    // rescale silently restored nothing (2026-07 audit, an-audit-finding).
    if (kind == "engine/action/rescale-system")
    {
        float durPct  = params.value("durationScalePercent", 100.0f);
        float sizePct = params.value("sizeScalePercent",     100.0f);
        if (m_pParticleSystem == nullptr || !*m_pParticleSystem)
        {
            ctx.SendErr("particle system not bound");
            return true;
        }
        ParticleSystem* sys = m_pParticleSystem->get();
        const float timeScale = durPct  / 100.0f;
        const float sizeScale = sizePct / 100.0f;
        // Snapshot BEFORE the mutation, and after the not-bound check so an
        // error path never lands an entry. Mirrors rescale-emitter below.
        captureUndo();
        // Walk every emitter, not just roots — DoRescaleEmitter only
        // touches per-emitter scalar fields and doesn't recurse, so we
        // need to iterate the flat list. Mirrors the loop at
        // src/Rescale.cpp:181 used by RescaleParticleSystem.
        auto& emitters = sys->getEmitters();
        for (size_t i = 0; i < emitters.size(); ++i)
        {
            if (emitters[i] != nullptr)
                DoRescaleEmitter(emitters[i], timeScale, sizeScale);
        }
        ctx.SendOk(json::object());
        ctx.MarkDirty();
        // Rescaling rewrites per-emitter scalars AND clears/rebuilds the Scale
        // key container (src/Rescale.cpp:69-75), which live EmitterInstances
        // hold cached track cursors into. Without this an already-placed
        // instance keeps its creation-time composite values, exactly as it did
        // for set-properties before #682 (2026-07 audit, an-audit-finding).
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        // Emitter parameters changed → notify the React tree so the
        // sidebar re-fetches via emitters/list.
        EmitEmittersTreeChanged();
        return true;
    }


    // -------- engine/query/* --------
    if (kind == "engine/query/ground-slot-empty")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        ctx.SendOk(json(m_engine->IsGroundSlotEmpty(params.value("slot", -1))));
        return true;
    }
    if (kind == "engine/query/skydome-list")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::string ctxStr = params.value("context", std::string{"space"});
        // Renamed from `ctx` on the TU move — it would shadow the
        // BridgeRequestContext parameter.
        SkydomeContext skyCtx = (ctxStr == "land") ? SkydomeContext::Land : SkydomeContext::Space;
        std::vector<std::string> prim, sec;
        m_engine->EnumerateSkydomeNames(skyCtx, prim, sec);
        json out;
        out["primary"]   = prim;
        out["secondary"] = sec;
        ctx.SendOk(out);
        return true;
    }
    // Enumerate selectable game objects (Name + category) for the picker.
    if (kind == "engine/query/reference-object-list")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::vector<GameObjectRef> objs;
        m_engine->EnumerateReferenceObjects(objs);   // kicks the off-thread build
        json arr = json::array();
        for (const GameObjectRef& r : objs)
            // domain/role/bucket from the profile classifier drive the
            // picker's collapsible Heroes / Ground / Space tree (legacy `category` retired).
            arr.push_back(json{ {"name", r.name},
                                {"domain", ObjDomainName(r.domain)},
                                {"role",   ObjRoleName(r.role)},
                                {"bucket", ObjBucketName(r.bucket)},
                                {"affiliation", r.affiliation} });   // picker faction filter
        // While the catalog builds off the UI thread, `objs` is empty and
        // `building` is true -> the picker shows "Loading objects…" and re-queries when
        // the catalog-ready engine/state/changed event fires (see HostWindow RenderD3D9).
        ctx.SendOk(json{ {"objects", arr}, {"building", !m_engine->IsReferenceCatalogReady()} });
        return true;
    }
    if (kind == "engine/query/skydome-slot-empty")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        ctx.SendOk(json(m_engine->IsSkydomeSlotEmpty(params.value("slot", -1))));
        return true;
    }
    // Read-only live-simulation counters, straight off the Engine getters.
    //
    // Added by the 2026-07 audit: the bridge could describe the AUTHORED
    // ParticleSystem in detail (emitters/list) but exposed NOTHING about the
    // live instances rendered from it, so a whole class of defects — a placed
    // instance not seeing a structural edit (an-audit-finding), not seeing a rescale (an-audit-finding),
    // not repainting a paused interpolation change (an-audit-finding) — had no observable
    // and could not be regression-tested at all.
    //
    // `emitters` counts live EmitterInstances across ALL instances, which is
    // what makes "did the placed instance pick up the new root?" answerable.
    if (kind == "engine/query/live-instances")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        ctx.SendOk(json{
            {"instances", m_engine->GetNumInstances()},
            {"emitters",  m_engine->GetNumEmitters()},
            {"particles", m_engine->GetNumParticles()},
        });
        return true;
    }
    if (kind == "engine/query/bloom-available")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        ctx.SendOk(json(m_engine->IsBloomAvailable()));
        return true;
    }
    if (kind == "engine/query/msaa-levels")
    {
        if (!ctx.RequireEngine(kind.c_str())) return true;
        std::vector<int> supported = m_engine->GetSupportedMsaaLevels();
        json levels = json::array();
        for (int s : supported) levels.push_back(s);
        ctx.SendOk(json{ {"levels", levels}, {"current", m_engine->GetCurrentMsaaLevel()} });
        return true;
    }


    // -------- engine/action/rescale-emitter -------------------------
    //
    // Per-emitter rescale (vs `engine/action/rescale-system` which
    // walks every emitter). Mirrors the inner loop body of legacy
    // `RescaleEmitter` at src/Rescale.cpp; here we just call
    // DoRescaleEmitter once on the chosen emitter.
    if (kind == "engine/action/rescale-emitter")
    {
        int id = params.value("id", -1);
        float durPct  = params.value("durationScalePercent", 100.0f);
        float sizePct = params.value("sizeScalePercent",     100.0f);
        ParticleSystem::Emitter* target = getEmitterById(id);
        if (target == nullptr)
        {
            ctx.SendErr("emitter not found");
            return true;
        }

        captureUndo();
        DoRescaleEmitter(target, durPct / 100.0f, sizePct / 100.0f);

        ctx.SendOk(json::object());
        ctx.MarkDirty();
        // Same live-instance invalidation as rescale-system above — the single
        // emitter path rebuilds the same Scale key container.
        if (m_engine) m_engine->OnParticleSystemChanged(-1);
        EmitEngineStateChanged();
        EmitEmittersTreeChanged();
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
