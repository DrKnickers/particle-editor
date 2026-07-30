// Kind handlers for the spawner/* + settings/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "../MouseCursor.h"

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchSpawner(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // -------- settings/lighting (get) --------------------------------
    //
    // Cross-mode read of the RAW lighting split the LightingPanel seeds
    // its displayed controls from — intensity/colour kept SEPARATE (the
    // engine snapshot only carries the lossy folded Vec4). Follows the value
    // names/types the legacy `LightingDlgProc` registry reads used (native Win32
    // UI, since removed): same names/types (floats REG_BINARY, colours + the flag
    // REG_DWORD). Colours go on the wire as packed COLORREF ints (`Color`).
    //
    // Gated under --test-host (returns the canonical defaults, NOT the live
    // registry) so the dialog-lighting a11y golden stays deterministic —
    // UNLESS ALO_SETTINGS_LIVE lifts the gate (the CDP test seam).
    if (kind == "settings/lighting")
    {
        // Canonical defaults (matching the legacy Win32 dialog).
        float sunI = 0.50f, sunZ = 0.0f, sunT = 45.0f;
        DWORD sunDiff = RGB(180, 180, 190), sunSpec = RGB(190, 190, 200);
        DWORD ambient = RGB(40, 40, 50), shadow = RGB(100, 100, 110);
        float f1I = 0.50f, f1Z = 120.0f, f1T = -10.0f; DWORD f1Diff = RGB(60, 80, 160);
        float f2I = 0.50f, f2Z = 210.0f, f2T = -10.0f; DWORD f2Diff = RGB(60, 80, 160);
        bool  forceAlign = true;  // kLightForceAlignDefault

        const bool gated = m_ephemeral || (m_testHost && !m_settingsLive);
        if (!gated)
        {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0,
                              KEY_READ, &hKey) == ERROR_SUCCESS)
            {
                auto readF = [&](const wchar_t* name, float& out) {
                    float v = 0.0f; DWORD t = 0, s = sizeof(v);
                    if (RegQueryValueExW(hKey, name, nullptr, &t,
                                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
                        && t == REG_BINARY && s == sizeof(v) && v == v && (v - v) == 0.0f)
                        out = v;
                };
                auto readDw = [&](const wchar_t* name, DWORD& out) {
                    DWORD v = 0, t = 0, s = sizeof(v);
                    if (RegQueryValueExW(hKey, name, nullptr, &t,
                                         reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
                        && t == REG_DWORD)
                        out = v;
                };
                readF(L"LightSunIntensity", sunI);
                readF(L"LightSunZAngle",    sunZ);
                readF(L"LightSunTilt",      sunT);
                readDw(L"LightSunDiffuseColor",  sunDiff);
                readDw(L"LightSunSpecularColor", sunSpec);
                readDw(L"LightSunAmbientColor",  ambient);
                readDw(L"LightSunShadowColor",   shadow);
                readF(L"LightFill1Intensity", f1I);
                readF(L"LightFill1ZAngle",    f1Z);
                readF(L"LightFill1Tilt",      f1T);
                readDw(L"LightFill1DiffuseColor", f1Diff);
                readF(L"LightFill2Intensity", f2I);
                readF(L"LightFill2ZAngle",    f2Z);
                readF(L"LightFill2Tilt",      f2T);
                readDw(L"LightFill2DiffuseColor", f2Diff);
                DWORD fa = 1;
                readDw(L"LightingForceFillAlignment", fa);
                forceAlign = (fa != 0);
                RegCloseKey(hKey);
            }
        }

        auto lightJson = [](float intensity, float az, float alt,
                            DWORD diffuse, DWORD specular) {
            return json{
                {"intensity", intensity}, {"az", az}, {"alt", alt},
                {"diffuse",  static_cast<int>(diffuse)},
                {"specular", static_cast<int>(specular)},
            };
        };
        ctx.SendOk(json{
            {"sun",   lightJson(sunI, sunZ, sunT, sunDiff, sunSpec)},
            {"fill1", lightJson(f1I, f1Z, f1T, f1Diff, 0)},
            {"fill2", lightJson(f2I, f2Z, f2T, f2Diff, 0)},
            {"ambient",    static_cast<int>(ambient)},
            {"shadow",     static_cast<int>(shadow)},
            {"forceAlign", forceAlign},
        });
        return true;
    }


    // -------- settings/lighting-force-align/set ----------------------
    //
    // Cross-mode write of the `LightingForceFillAlignment` REG_DWORD
    // (WriteLightingBool) so a toggle in the new UI is
    // seen by legacy. No-op under --test-host (so the a11y harness never
    // mutates the dev box's registry) UNLESS ALO_SETTINGS_LIVE lifts the
    // gate for the CDP test seam.
    if (kind == "settings/lighting-force-align/set")
    {
        const bool enabled = params.value("enabled", true);
        const bool gated = m_ephemeral || (m_testHost && !m_settingsLive);
        if (!gated)
        {
            HKEY hKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                &hKey, nullptr) == ERROR_SUCCESS)
            {
                DWORD v = enabled ? 1 : 0;
                RegSetValueExW(hKey, L"LightingForceFillAlignment", 0, REG_DWORD,
                               reinterpret_cast<const BYTE*>(&v), sizeof(v));
                RegCloseKey(hKey);
            }
        }
        ctx.SendOk(json::object());
        return true;
    }


    // -------- settings/lighting/set ----------------------------------
    //
    // Full write-back of the raw lighting split the new-UI LightingPanel
    // holds in state. Writes the SAME 16 value names the GET handler reads
    // (and the legacy dialog read/wrote) so edits + Reset in the new UI survive a reopen/restart
    // and stay in sync with legacy. Without this, anything the legacy
    // dialog persisted (e.g. a stale ambient COLORREF) reappears on every
    // load and the new UI can't overwrite it.
    //
    // Same --test-host gate as the force-align write above: no-op under the
    // a11y harness unless ALO_SETTINGS_LIVE lifts it (the CDP test seam),
    // so the dialog-lighting golden never mutates the dev box's registry.
    if (kind == "settings/lighting/set")
    {
        const bool gated = m_ephemeral || (m_testHost && !m_settingsLive);
        if (!gated)
        {
            HKEY hKey = nullptr;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKeyPath, 0, nullptr,
                                REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                                &hKey, nullptr) == ERROR_SUCCESS)
            {
                // Floats persist as REG_BINARY (matches ReadLightingFloat);
                // colours + the flag as REG_DWORD (matches ReadLightingColor /
                // ReadLightingBool).
                auto writeF = [&](const wchar_t* name, float v) {
                    RegSetValueExW(hKey, name, 0, REG_BINARY,
                                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
                };
                auto writeDw = [&](const wchar_t* name, DWORD v) {
                    RegSetValueExW(hKey, name, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&v), sizeof(v));
                };
                auto lf = [&](const char* k) { return params.value(k, json::object()); };
                const json sun   = lf("sun");
                const json fill1 = lf("fill1");
                const json fill2 = lf("fill2");

                writeF(L"LightSunIntensity",     sun.value("intensity", 0.5f));
                writeF(L"LightSunZAngle",        sun.value("az", 0.0f));
                writeF(L"LightSunTilt",          sun.value("alt", 45.0f));
                writeDw(L"LightSunDiffuseColor",  static_cast<DWORD>(sun.value("diffuse",  static_cast<int>(RGB(180,180,190)))));
                writeDw(L"LightSunSpecularColor", static_cast<DWORD>(sun.value("specular", static_cast<int>(RGB(190,190,200)))));
                writeDw(L"LightSunAmbientColor",  static_cast<DWORD>(params.value("ambient", static_cast<int>(RGB(40,40,50)))));
                writeDw(L"LightSunShadowColor",   static_cast<DWORD>(params.value("shadow",  static_cast<int>(RGB(100,100,110)))));

                writeF(L"LightFill1Intensity",   fill1.value("intensity", 0.5f));
                writeF(L"LightFill1ZAngle",      fill1.value("az", 120.0f));
                writeF(L"LightFill1Tilt",        fill1.value("alt", -10.0f));
                writeDw(L"LightFill1DiffuseColor", static_cast<DWORD>(fill1.value("diffuse", static_cast<int>(RGB(60,80,160)))));

                writeF(L"LightFill2Intensity",   fill2.value("intensity", 0.5f));
                writeF(L"LightFill2ZAngle",      fill2.value("az", 210.0f));
                writeF(L"LightFill2Tilt",        fill2.value("alt", -10.0f));
                writeDw(L"LightFill2DiffuseColor", static_cast<DWORD>(fill2.value("diffuse", static_cast<int>(RGB(60,80,160)))));

                writeDw(L"LightingForceFillAlignment", params.value("forceAlign", true) ? 1u : 0u);
                RegCloseKey(hKey);
            }
        }
        ctx.SendOk(json::object());
        return true;
    }


    // -------- spawner/* -------------------
    //
    // The new-UI host doesn't yet own a SpawnerDriver* (matches the
    // ParticleSystem* situation). The handlers do the *editor-level* side of
    // the work: cache the incoming config in m_spawnerConfig so a
    // subsequent engine/state/snapshot returns it, log the request for
    // diagnostics, and broadcast engine/state/changed so React sees the
    // updated config land. When SpawnerDriver wiring happens later,
    // the cached config can be passed directly into
    // `m_spawnerDriver->SetConfig(...)` from these same handlers.
    //
    // Note: spawner config is session state (matches legacy: "never
    // written into the .alo" per SpawnerDriver.h:16). It deliberately
    // does NOT set dirty=true.
    if (kind == "spawner/start")
    {
        // cache + commit to the real driver. The cache is kept
        // updated so snapshot reads still work when no driver is bound
        // (Vitest / partial-wiring paths).
        m_spawnerConfig = params;
        if (m_spawnerDriver)
        {
            SpawnerConfig cfg = JsonToSpawnerConfig(params);
            ClampSpawnerConfig(cfg);
            m_spawnerDriver->SetConfig(cfg);
        }
        ctx.SendOk(json::object());
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "spawner/trigger")
    {
        // real trigger. Note that without per-frame Tick wiring
        // the burst-state machine doesn't advance — Trigger schedules
        // a burst that won't actually fire instances until later
        // work wires SpawnerDriver::Tick into the render loop. That's
        // a documented out-of-scope item for now.
        if (m_spawnerDriver
            && m_pParticleSystem
            && *m_pParticleSystem
            && m_engine)
        {
            m_spawnerDriver->Trigger(m_pParticleSystem->get(), m_engine);
        }
        ctx.SendOk(json::object());
        return true;
    }
    if (kind == "spawner/stop")
    {
        // flip enabled=false on the live driver. Auto-mode
        // bursts stop scheduling; manual triggers still work — but any
        // armed-yet-unbegun burst and queued triggers are cancelled
        // (SetConfig can't do it: in Manual mode enabled is already
        // false, so there's no transition for it to see). A burst that
        // has begun emitting finishes, as it always has.
        if (m_spawnerDriver)
        {
            m_spawnerDriver->CancelPending();
            SpawnerConfig cfg = m_spawnerDriver->GetConfig();
            cfg.enabled = false;
            m_spawnerDriver->SetConfig(cfg);
        }
        // Keep the JSON cache in sync so snapshots without a bound
        // driver also reflect the stop.
        if (m_spawnerConfig.is_object())
        {
            m_spawnerConfig["enabled"] = false;
        }
        ctx.SendOk(json::object());
        EmitEngineStateChanged();
        return true;
    }

    // -----------------------------------------------------------------
    // preview/* — the scripted mirror of the native Shift-hover spawn
    // (HostWindow's WM_KEYDOWN VK_SHIFT path), added so the guide's
    // ref-shift-preview clip can show the feature. Record-only kinds
    // (allowlisted in ClipTimeline.h; the web UI never sends them).
    //
    //   preview/attach {x,y}  spawn an instance parented to a dispatcher-
    //                         owned MouseCursor anchor at the unprojected
    //                         client (x,y) — the Shift-press behavior.
    //   preview/move {x,y}    re-position the anchor; the instance follows
    //                         in world space exactly like the native
    //                         feature (fired along the cursor path).
    //   preview/place {}      detach — the instance keeps emitting where
    //                         it is (the Shift-click place behavior).
    //   preview/kill {}       remove it (the Shift-release behavior).
    //
    // The anchor's UpdateVelocity is deliberately NOT called: it derives
    // velocity from QueryPerformanceCounter deltas, which would make the
    // parent-speed fling nondeterministic under the stepped record clock.
    // Position-follow (the taught behavior) doesn't need it. Failures are
    // SendErr so an authoring mistake aborts the record run (exit 3)
    // instead of silently rendering an empty viewport.
    //
    // [UAF/ABA guard] m_recordPreviewAttached is a tokenized borrow into
    // Engine::m_instances, and EVERY Engine::Clear() (file/new, file/open,
    // the overload hard-guard's refusal/edit-time clears) frees all
    // instances. Re-resolve both its address and immutable token before every
    // use; a stale or allocator-reused borrow self-heals to
    // "nothing attached", which then SendErrs loudly on move/place/kill.
    // (The file-op teardowns in BridgeDispatch_File.cpp also null it
    // eagerly, closing the free+realloc aliasing window for that family.)
    if (kind.rfind("preview/", 0) == 0
        && m_recordPreviewAttached
        && m_engine
        && !m_engine->ResolveInstance(m_recordPreviewAttached))
    {
        m_recordPreviewAttached.Reset();
    }
    if (kind == "preview/attach")
    {
        if (!m_engine || !m_pParticleSystem || !*m_pParticleSystem
            || (*m_pParticleSystem)->getEmitters().empty())
        {
            ctx.SendErr("preview/attach: no engine or empty particle system");
            return true;
        }
        if (m_recordPreviewAttached)
        {
            ctx.SendErr("preview/attach: already attached");
            return true;
        }
        const int x = params.value("x", 0);
        const int y = params.value("y", 0);
        D3DXVECTOR3 pos;
        GetCursorPos3D(m_engine, (short)x, (short)y, pos);
        m_recordPreviewCursor.SetPosition(pos);
        m_recordPreviewAttached = m_engine->MakeInstanceHandle(
            m_engine->SpawnParticleSystem(
                **m_pParticleSystem, &m_recordPreviewCursor));
        if (!m_recordPreviewAttached)
        {
            ctx.SendErr("preview/attach: spawn refused");
            return true;
        }
        ctx.SendOk(json::object());
        return true;
    }
    if (kind == "preview/move")
    {
        if (!m_engine || !m_recordPreviewAttached)
        {
            ctx.SendErr("preview/move: nothing attached");
            return true;
        }
        const int x = params.value("x", 0);
        const int y = params.value("y", 0);
        D3DXVECTOR3 pos;
        GetCursorPos3D(m_engine, (short)x, (short)y, pos);
        m_recordPreviewCursor.SetPosition(pos);
        ctx.SendOk(json::object());
        return true;
    }
    if (kind == "preview/place")
    {
        if (!m_engine || !m_recordPreviewAttached)
        {
            ctx.SendErr("preview/place: nothing attached");
            return true;
        }
        if (!m_engine->DetachParticleSystem(m_recordPreviewAttached))
        {
            m_recordPreviewAttached.Reset();
            ctx.SendErr("preview/place: attached instance is stale");
            return true;
        }
        m_recordPreviewAttached.Reset();
        ctx.SendOk(json::object());
        return true;
    }
    if (kind == "preview/kill")
    {
        if (!m_engine || !m_recordPreviewAttached)
        {
            ctx.SendErr("preview/kill: nothing attached");
            return true;
        }
        if (!m_engine->KillParticleSystem(m_recordPreviewAttached))
        {
            m_recordPreviewAttached.Reset();
            ctx.SendErr("preview/kill: attached instance is stale");
            return true;
        }
        m_recordPreviewAttached.Reset();
        ctx.SendOk(json::object());
        return true;
    }

    return false;   // kind not in this domain
}

} // namespace host
