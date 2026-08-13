// Kind handlers for the spawner/* + settings/* bridge domain(s), moved out of
// DispatchInternal's ladder (Phase A dispatch split --
// tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "LightingSettings.h"
#include "../MouseCursor.h"

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchSpawner(BridgeRequestContext& ctx)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;

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
        float sunI = kSunIntensityDefault, sunZ = kSunZAngleDefault, sunT = kSunTiltDefault;
        DWORD sunDiff = SunDiffuseColorDefault(), sunSpec = SunSpecularColorDefault();
        DWORD ambient = SunAmbientColorDefault(), shadow = SunShadowColorDefault();
        float f1I = kFill1IntensityDefault, f1Z = kFill1ZAngleDefault, f1T = kFill1TiltDefault; DWORD f1Diff = Fill1DiffuseColorDefault();
        float f2I = kFill2IntensityDefault, f2Z = kFill2ZAngleDefault, f2T = kFill2TiltDefault; DWORD f2Diff = Fill2DiffuseColorDefault();
        bool  forceAlign = kForceAlignDefault;  // kLightForceAlignDefault

        const bool gated = !PersistsUserState();
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
                readF(kLightSunIntensity, sunI);
                readF(kLightSunZAngle,    sunZ);
                readF(kLightSunTilt,      sunT);
                readDw(kLightSunDiffuseColor,  sunDiff);
                readDw(kLightSunSpecularColor, sunSpec);
                readDw(kLightSunAmbientColor,  ambient);
                readDw(kLightSunShadowColor,   shadow);
                readF(kLightFill1Intensity, f1I);
                readF(kLightFill1ZAngle,    f1Z);
                readF(kLightFill1Tilt,      f1T);
                readDw(kLightFill1DiffuseColor, f1Diff);
                readF(kLightFill2Intensity, f2I);
                readF(kLightFill2ZAngle,    f2Z);
                readF(kLightFill2Tilt,      f2T);
                readDw(kLightFill2DiffuseColor, f2Diff);
                DWORD fa = kForceAlignDefault ? 1u : 0u;
                readDw(kLightForceFillAlignment, fa);
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
        const bool enabled = params.value("enabled", kForceAlignDefault);
        const bool gated = !PersistsUserState();
        if (!gated)
            WriteRegDword(kLightForceFillAlignment, enabled ? 1u : 0u);
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
        const bool gated = !PersistsUserState();
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

                writeF(kLightSunIntensity,     sun.value("intensity", kSunIntensityDefault));
                writeF(kLightSunZAngle,        sun.value("az", kSunZAngleDefault));
                writeF(kLightSunTilt,          sun.value("alt", kSunTiltDefault));
                writeDw(kLightSunDiffuseColor,  static_cast<DWORD>(sun.value("diffuse",  static_cast<int>(SunDiffuseColorDefault()))));
                writeDw(kLightSunSpecularColor, static_cast<DWORD>(sun.value("specular", static_cast<int>(SunSpecularColorDefault()))));
                writeDw(kLightSunAmbientColor,  static_cast<DWORD>(params.value("ambient", static_cast<int>(SunAmbientColorDefault()))));
                writeDw(kLightSunShadowColor,   static_cast<DWORD>(params.value("shadow",  static_cast<int>(SunShadowColorDefault()))));

                writeF(kLightFill1Intensity,   fill1.value("intensity", kFill1IntensityDefault));
                writeF(kLightFill1ZAngle,      fill1.value("az", kFill1ZAngleDefault));
                writeF(kLightFill1Tilt,        fill1.value("alt", kFill1TiltDefault));
                writeDw(kLightFill1DiffuseColor, static_cast<DWORD>(fill1.value("diffuse", static_cast<int>(Fill1DiffuseColorDefault()))));

                writeF(kLightFill2Intensity,   fill2.value("intensity", kFill2IntensityDefault));
                writeF(kLightFill2ZAngle,      fill2.value("az", kFill2ZAngleDefault));
                writeF(kLightFill2Tilt,        fill2.value("alt", kFill2TiltDefault));
                writeDw(kLightFill2DiffuseColor, static_cast<DWORD>(fill2.value("diffuse", static_cast<int>(Fill2DiffuseColorDefault()))));

                writeDw(kLightForceFillAlignment, params.value("forceAlign", kForceAlignDefault) ? 1u : 0u);
                RegCloseKey(hKey);
            }
        }
        ctx.SendOk(json::object());
        return true;
    }


    // -------- spawner/* -------------------
    //
    // BridgeDispatcher owns one parsed-and-clamped SpawnerConfig for all
    // snapshots/events, and applies that same struct to a bound driver.
    //
    // Note: spawner config is session state (matches legacy: "never
    // written into the .alo" per SpawnerDriver.h:16). It deliberately
    // does NOT set dirty=true.
    if (kind == "spawner/start")
    {
        ApplySpawnerStart(params);
        ctx.SendOk(json::object());
        EmitEngineStateChanged();
        return true;
    }
    if (kind == "spawner/trigger")
    {
        // The host's render loop advances the driver's burst state; only
        // forward a trigger while all production borrows are bound.
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
        ApplySpawnerStop();
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
