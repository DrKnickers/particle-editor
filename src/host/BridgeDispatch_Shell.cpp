// Kind handlers for the window/layout/viewport/host/app/debug/stats + singles
// bridge domain(s), moved out of DispatchInternal's ladder (Phase A dispatch
// split -- tasks/2026-07-06-heavyweight-refactor-plan.md).

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"

#include "AcceleratorBridge.h"
#include "InputDispatcher.h"
#include "LayoutBroker.h"
#include "WindowCapture.h"    // CaptureWindowToPng (debug/capture-*)
#include "HostMessages.h"     // WM_APP_QUIT_CONFIRMED (app/quit)
#include "StringConv.h"       // host::Utf8ToWide (debug/capture-window)

#include <dwmapi.h>           // DwmSetWindowAttribute (host/backing-color)
#pragma comment(lib, "dwmapi.lib")
// DWM immersive dark-mode caption attribute — same SDK guard as
// BridgeDispatcher.cpp (value 20, post-Win10-2004).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using nlohmann::json;

namespace host {

bool BridgeDispatcher::TryDispatchShell(BridgeRequestContext& ctx, const std::string& kind)
{
    // DispatchInternal-local aliases so the moved ladder blocks below stay
    // verbatim (plan #3A transforms only).
    const json&        params = ctx.params;
    const std::string& id     = ctx.id;

    // -------- layout/viewport-rect --------
    if (kind == "layout/viewport-rect")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int w = params.value("w", 0);
        int h = params.value("h", 0);
        m_layout.Apply(x, y, w, h);
        ctx.SendOk(json::object());
        return true;
    }


    // -------- layout/scene-rect --------
    // Under the popup-spans-window architecture the
    // popup HWND covers the WebView's main-row area at all times.
    // This message updates the SUB-RECT inside the popup where the
    // rendered scene should be visible — AlphaCompositor stamps
    // alpha=0 outside this rect, so UI panels behind those bands
    // show through (and receive their own mouse events, courtesy of
    // WS_EX_LAYERED + ULW_ALPHA hit-test semantics).
    //
    // Engine code is unchanged — camera frustum still uses the full
    // popup backbuffer aspect (popup-rect aspect per the T4c
    // questionnaire). This message is alpha-mask-only; it does NOT
    // trigger Engine::Reset. Splitter drag can fire this per frame
    // without stacking expensive D3D9 device resets.
    //
    // [resize-perf hygiene] In PRODUCTION no web code sends
    // layout/viewport-rect at all (the handler above is exercised only
    // by tests/POCs, e.g. viewport-resize.spec.ts) — the live
    // window-resize reset driver is the NATIVE path:
    // WM_WINDOWPOSCHANGED → LayoutBroker::PredictAndApply →
    // Engine::Reset (deferred to gesture settle while in sizemove,
    // fix A). Don't chase this bridge binding when profiling resets.
    if (kind == "layout/scene-rect")
    {
        int x = params.value("x", 0);
        int y = params.value("y", 0);
        int w = params.value("w", 0);
        int h = params.value("h", 0);
        m_layout.SetSceneRect(x, y, w, h);
        ctx.SendOk(json::object());
        return true;
    }


    // -------- animate-scene-rect --------
    // One-shot dock-slide animation. Instead of the per-frame
    // layout/scene-rect stream (which the uncapped render loop samples at
    // irregular Δt → a juddering viewport edge), the web sends ONE of these at
    // the dock toggle; LayoutBroker then re-renders the engine at a wall-clock-
    // lerped rect every frame, synced to the CSS flex-grow tween. StartSceneAnim
    // is a no-op when no DComp compositor is attached. `from`/
    // `to` are scene rects in main-client device px; `msElapsedAtSend` back-dates
    // the host clock to the CSS origin across this IPC hop. `easing` is ignored
    // here — the host hardcodes the matching CSS `ease` cubic-bezier and the web
    // only ever sends "ease"; the field stays in the schema for forward-compat.
    if (kind == "animate-scene-rect")
    {
        const json from = params.value("from", json::object());
        const json to   = params.value("to",   json::object());
        m_layout.StartSceneAnim(
            from.value("x", 0), from.value("y", 0), from.value("w", 0), from.value("h", 0),
            to.value("x", 0),   to.value("y", 0),   to.value("w", 0),   to.value("h", 0),
            params.value("durationMs", 0.0),
            params.value("msElapsedAtSend", 0.0));
        ctx.SendOk(json::object());
        return true;
    }


    // -------- host/backing-color --------
    // recolour the DComp composition backing to the
    // app-shell --bg so every transparent DOM region outside the scene
    // rect (panel gaps, splitter seams, rounded-corner wedges) blends
    // into the shell instead of showing the black host backing. `color`
    // is a CSS string from getComputedStyle ("#rrggbb" / "rgb(r,g,b)").
    // Unparseable strings answer ok:false and leave the backing as-is —
    // never throw into the dispatch path. No-op until LayoutBroker has a
    // DComp compositor attached.
    if (kind == "host/backing-color")
    {
        const std::string colorStr = params.value("color", std::string{});
        COLORREF c = 0;
        if (!ParseCssColorToColorRef(colorStr, c))
        {
            ctx.SendErr("host/backing-color: unparseable color '" + colorStr + "'");
            return true;
        }
        m_layout.SetBackingColor(c);

        // Theme the native title bar to match the app shell. The backing
        // colour IS the resolved `--bg`, pushed on mount + every theme
        // toggle, so deriving the caption's dark-mode from its luminance
        // makes the Win32 caption follow the in-app theme for free (no new
        // bridge surface). Perceived luminance (Rec.601); < 128 ⇒ dark.
        if (m_hostHwnd)
        {
            const int luma = (GetRValue(c) * 299 + GetGValue(c) * 587 +
                              GetBValue(c) * 114) / 1000;
            BOOL dark = (luma < 128) ? TRUE : FALSE;
            DwmSetWindowAttribute(m_hostHwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                                  &dark, sizeof(dark));
        }

        printf("[backing] color '%s' -> RGB(%u,%u,%u)\n",
               colorStr.c_str(),
               GetRValue(c), GetGValue(c), GetBValue(c));
        fflush(stdout);
        ctx.SendOk(json::object());
        return true;
    }


    // -------- viewport/capture-snapshot --------
    // React's Modal primitive calls this on open to grab a
    // frozen image of the engine viewport. It renders the JPEG as an
    // opaque <img> portaled into the WebView2 viewport DOM, covering the
    // live DComp engine visual beneath the transparent WebView2 — so
    // Dialog.Overlay's `backdrop-blur-sm` can blur engine + panels
    // uniformly. The compositor reads the engine RT back on demand, so
    // the capture sees the raw engine output.
    //
    // Returns `{ imageBase64, w, h }` — base64 of a JPEG (the 
    // backdrop is shown blurred, so lossy is invisible and far cheaper to
    // encode + transmit than PNG). When the compositor has no frame yet
    // (engine never composited, device just reset), returns an empty string
    // + zero dims so the React side can short-circuit its <img> render.
    if (kind == "viewport/capture-snapshot")
    {
        std::string imageBase64;
        int w = 0;
        int h = 0;
        if (m_layout.CaptureSnapshotPng(imageBase64, w, h))
        {
            ctx.SendOk(json{{"imageBase64", std::move(imageBase64)}, {"w", w}, {"h", h}});
        }
        else
        {
            ctx.SendOk(json{{"imageBase64", ""}, {"w", 0}, {"h", 0}});
        }
        return true;
    }


    // -------- debug/capture-frame ([render-capture]) --------
    // Write the current engine viewport frame straight to a PNG at `path` so the
    // rendered output can be inspected / diffed offline -- render-fidelity
    // feel-tests and, paired with engine/set/paused + engine/action/step-frames,
    // deterministic pause->step->capture particle-lifecycle filmstrips. Reuses the
    // proven CaptureSnapshotToFile readback (the real Engine::Render() RT), so it
    // never diverges into a second renderer.
    //
    // Gated: Debug builds always; Release only under --test-host. It writes a
    // caller-chosen path, so it must never be reachable from a normal Release
    // session. Returns { path } on success.
    if (kind == "debug/capture-frame")
    {
#ifdef NDEBUG
        const bool allowed = m_testHost;
#else
        const bool allowed = true;
#endif
        if (!allowed)
        {
            ctx.SendErr("debug/capture-frame is gated to debug builds / --test-host");
            return true;
        }
        const std::string path = params.value("path", std::string{});
        if (path.empty())
        {
            ctx.SendErr("debug/capture-frame requires a non-empty 'path'");
            return true;
        }
        const bool ok = m_layout.CaptureSnapshotToFile(Utf8ToWide(path));
        if (ok) ctx.SendOk(json{{"path", path}});
        else    ctx.SendErr("capture failed (no composited frame yet, no render target, or file write failed)");
        return true;
    }


    // -------- debug/capture-window ([composed-capture] Track C) --------
    // Capture the FULL composed window (the DWM/DirectComposition flatten of the
    // React UI visual + the engine viewport) to a PNG at `path` — the whole-app
    // analogue of debug/capture-frame (which is engine-RT only). Uses the host's
    // top-level HWND (m_hostHwnd, set via SetHostHwnd at HostWindow.cpp) +
    // PrintWindow(PW_RENDERFULLCONTENT). Same gating as capture-frame: Debug
    // always, Release only under --test-host (it writes a caller-chosen path).
    if (kind == "debug/capture-window")
    {
#ifdef NDEBUG
        const bool allowed = m_testHost;
#else
        const bool allowed = true;
#endif
        if (!allowed)
        {
            ctx.SendErr("debug/capture-window is gated to debug builds / --test-host");
            return true;
        }
        const std::string path = params.value("path", std::string{});
        if (path.empty())
        {
            ctx.SendErr("debug/capture-window requires a non-empty 'path'");
            return true;
        }
        if (!m_hostHwnd)
        {
            ctx.SendErr("debug/capture-window: no host window");
            return true;
        }
        const bool ok = host::CaptureWindowToPng(m_hostHwnd, Utf8ToWide(path));
        if (ok) ctx.SendOk(json{{"path", path}});
        else    ctx.SendErr("composed-window capture failed (PrintWindow or file write failed)");
        return true;
    }


    // -------- viewport/input --------
    //
    // DOM mouse/wheel/key events on the in-DOM <canvas> arrive here
    // and are forwarded to InputDispatcher, which PostMessages the
    // synthesized Win32 message to the (hidden) viewport popup HWND.
    // When InputDispatcher is null (compositor init failed) the request
    // is a silent no-op — ack rather than error so a stray event doesn't
    // surface as a noisy reject.
    if (kind == "viewport/input")
    {
        if (m_input) m_input->Dispatch(params);
        ctx.SendOk(json::object());
        return true;
    }


    // -------- register-accelerators --------
    if (kind == "register-accelerators")
    {
        auto combos = params.value("combos", std::vector<std::string>{});
        m_accel.RegisterCombos(combos);
        fprintf(stderr, "[host] AcceleratorBridge registered %zu combo(s)\n", combos.size());
        ctx.SendOk(json::object());
        return true;
    }


    // -------- app/quit -----------------------------------------------
    //
    // React File → Exit (and the native-X → app/close-requested → prompt path)
    // dispatch this AFTER the Save/Discard/Cancel prompt has cleared.
    // PostMessage WM_APP_QUIT_CONFIRMED (not WM_CLOSE) so the wndproc's
    // DestroyWindow → WM_DESTROY teardown runs WITHOUT re-entering the dirty
    // WM_CLOSE veto. PostMessage (not SendMessage) so the response envelope is
    // emitted before the pump processes the quit.
    if (kind == "app/quit")
    {
        ctx.SendOk(json::object());
        if (m_hostHwnd != nullptr)
        {
            PostMessage(m_hostHwnd, WM_APP_QUIT_CONFIRMED, 0, 0);
        }
        return true;
    }


    // -------- window/minimize | window/maximize | window/close ----------
    //
    // The frameless custom title bar's controls act on the top-level HWND.
    // minimize/maximize call ShowWindow directly (maximize TOGGLES restore via
    // IsZoomed). close posts WM_CLOSE — the SAME entry the native frame-X used,
    // so the dirty-doc Save prompt (WM_CLOSE → app/close-requested) still fires;
    // never a direct DestroyWindow.
    if (kind == "window/minimize")
    {
        ctx.SendOk(json::object());
        if (m_hostHwnd != nullptr) ShowWindow(m_hostHwnd, SW_MINIMIZE);
        return true;
    }
    if (kind == "window/maximize")
    {
        ctx.SendOk(json::object());
        if (m_hostHwnd != nullptr)
            ShowWindow(m_hostHwnd, IsZoomed(m_hostHwnd) ? SW_RESTORE : SW_MAXIMIZE);
        return true;
    }
    if (kind == "window/close")
    {
        ctx.SendOk(json::object());
        if (m_hostHwnd != nullptr) PostMessage(m_hostHwnd, WM_CLOSE, 0, 0);
        return true;
    }


    // -------- stats/set-frozen --------
    // Test-only knob that suppresses
    // the 4 Hz stats/tick emission AND tells React's StatusBar to
    // clear its local state. Used by a11y spec beforeEach to bring
    // the StatusBar to a deterministic placeholder render before
    // capturing UIA goldens. Default-true for ergonomic test calls;
    // pass {frozen: false} to resume normal emissions.
    if (kind == "stats/set-frozen")
    {
        const bool frozen = params.value("frozen", true);
        m_statsFrozen = frozen;
        if (m_emit)
        {
            json env = {
                {"type",    "evt"},
                {"kind",    "stats/frozen-changed"},
                {"payload", {{"frozen", frozen}}},
            };
            m_emit(env.dump());
        }
        ctx.SendOk(json::object());
        return true;
    }


    return false;   // kind not in this domain
}

} // namespace host
