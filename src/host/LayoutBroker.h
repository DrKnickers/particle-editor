// LayoutBroker — applies the React-side `layout/viewport-rect` message
// to the D3D9 viewport HWND.
//
// (May 2026): the viewport is a top-level WS_POPUP owned by the
// main HWND, not a WS_CHILD. React reports the viewport quadrant rect
// in main-client coordinates; LayoutBroker converts to screen
// coordinates for SetWindowPos on the popup. The popup is composited
// by DWM as its own layer, above any child HWND including WebView2 —
// that's what makes the viewport visible.
//
// Under composition the popup is hidden; LayoutBroker keeps the cached
// scene rect in sync (ReemitSceneRect) so the AlphaCompositor's snapshot
// crop (modal backdrops) and the DComp engine-visual transform both track
// popup moves and resizes.
#ifndef HOST_LAYOUT_BROKER_H
#define HOST_LAYOUT_BROKER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

class Engine;

namespace host {

class AlphaCompositor;
// Phase 3 Stage 5 — DComp tree compositor (owns the engine
// visual). LayoutBroker injects scene-rect transforms when composition
// mode is active. Forward-declared to keep dcomp.h out of this header
// (Compositor.cpp's isolation pattern).
class Compositor;

class LayoutBroker
{
public:
    explicit LayoutBroker(HWND viewport = nullptr) : m_viewport(viewport), m_engine(nullptr), m_lastW(0), m_lastH(0) {}

    void SetViewport(HWND viewport) { m_viewport = viewport; }
    HWND GetViewport() const { return m_viewport; }

    // Inject the live Engine after construction (mirrors
    // BridgeDispatcher::SetEngine). Null is treated as "engine not
    // ready"; Apply still performs the SetWindowPos so layout works,
    // but skips the D3D9 swap-chain reset.
    void SetEngine(Engine* engine) { m_engine = engine; }

    // inject the shared-RT compositor (owns the off-screen RT the
    // engine renders into and the on-demand snapshot used by modal
    // backdrops). On attach the cached scene rect is replayed so the
    // snapshot crop is correct on the first capture. Null = not installed.
    void SetAlphaCompositor(AlphaCompositor* compositor);

    // Phase 3 Stage 5 — inject the DComp-tree compositor (the
    // one that owns the engine visual). Non-null implies composition
    // mode is active; SetSceneRect will additionally forward the
    // scene-rect transform onto Compositor::SetEngineVisualTransform
    // and the engine's per-frame viewport via Engine::SetSceneViewport
    // (gated on this pointer per R9 mitigation c). Passing null at
    // teardown clears the gate so any late SetSceneRect dispatch
    // post-WM_DESTROY doesn't dereference a freed Compositor.
    //
    // Replays the cached scene-rect onto the newly-attached compositor
    // + engine, so the first frame after attach is sized correctly
    // even if React hasn't dispatched a layout/scene-rect since the
    // compositor came up.
    void SetCompositor(Compositor* compositor);

    // x/y/w/h are device pixels in the OWNER MAIN HWND'S client
    // coordinates, exactly what React's ViewportSlot sends from
    // getBoundingClientRect. With per-monitor-v2 DPI awareness,
    // child-window coordinates are in physical pixels.
    //
    // converts to screen coords via ClientToScreen(owner, …)
    // before SetWindowPos because the viewport is now a top-level
    // popup, not a child.
    void Apply(int x, int y, int w, int h);

    //: re-apply the last-cached client-coord rect, with a fresh
    // ClientToScreen translation. Called from HostWindow's WM_MOVE
    // handler when the main window is dragged across the desktop so
    // the popup viewport follows. Skips the Engine::Reset path
    // (size didn't change, only position).
    void RefreshScreenPosition();

    // polish: predict the viewport rect from main's CURRENT client
    // size + the layout offsets cached at the last Apply. Used by
    // HostWindow's WM_SIZE handler so the popup tracks main's resize
    // synchronously, before React's ResizeObserver fires the next
    // authoritative layout/viewport-rect.
    void PredictAndApply();

    // B1.4 T4c: under the popup-spans-window architecture,
    // React no longer dispatches a popup-rect via layout/viewport-rect.
    // The host sizes the popup HWND to the OWNER MAIN HWND's full
    // client rect on WM_CREATE / WM_SIZE / WM_WINDOWPOSCHANGED. The
    // scene-rect (from React via layout/scene-rect) bounds the DComp
    // engine visual to the centre quadrant via SetEngineVisualTransform.
    //
    // ApplyFullClient is equivalent to Apply(0, 0, clientW, clientH)
    // using the owner's current GetClientRect — wrapped here so
    // callers don't need to repeat the GetWindow / GetClientRect
    // boilerplate, and so the "popup = full client" invariant lives
    // in one place.
    void ApplyFullClient();

    // B1.4 T4c: set the scene rect (the visible viewport sub-region)
    // in MAIN-HWND-CLIENT coords. LayoutBroker caches it and re-emits it
    // (ReemitSceneRect) to the AlphaCompositor (which crops its snapshot —
    // the modal backdrop — to it, in popup-client coords) and to the DComp
    // Compositor + Engine (the live render transform, in main-client coords)
    // whenever the popup moves or resizes.
    //
    // Passing w<=0 or h<=0 clears the scene rect (snapshot crop disabled —
    // the full RT is captured).
    void SetSceneRect(int x, int y, int w, int h);

    // Phase 3 Stage 5 — read the cached scene rect (in
    // main-client coords). Returns true and populates the outs when a
    // non-degenerate scene rect has been dispatched at least once;
    // returns false (outs untouched) otherwise. Used by HostWindow at
    // composition-controller-ready time to seed the engine visual's
    // initial transform without waiting for React's next dispatch
    // (sub-plan §3.5 — avoids a 1-3 frame full-client glitch).
    bool GetSceneRect(int& x, int& y, int& w, int& h) const;

    // forward the theme background colour to the DComp
    // compositor's rearmost backing visual (composition mode only).
    // No-op when no Compositor is attached. `color`
    // is a COLORREF (0x00BBGGRR). Mirrors the SetSceneRect → Compositor
    // forwarding pattern.
    void SetBackingColor(COLORREF color);

    // B1.3.1.1: forward a viewport snapshot request to the compositor.
    // Returns `false` (and leaves outputs untouched) when no
    // compositor is attached or the compositor has no cached frame
    // (engine never composited, just-reset device, etc.). React's
    // Modal calls this through `viewport/capture-snapshot` so it can
    // render the engine output as a frozen <img> backdrop while the
    // modal is open and CSS effects blur it uniformly with the
    // panels.
    bool CaptureSnapshotPng(std::string& outBase64, int& outW, int& outH);

    // [render-capture] Forward a to-file snapshot request to the compositor.
    // Writes the most recent pre-stamp engine frame straight to a PNG at `path`
    // (same readback as CaptureSnapshotPng, lossless PNG to disk). Returns false
    // when no compositor is attached or it has no frame yet. Used by the
    // debug/--test-host-gated `debug/capture-frame` bridge kind so the rendered
    // viewport can be inspected/diffed offline (render-fidelity feel-tests +
    // pause->step->capture particle filmstrips).
    bool CaptureSnapshotToFile(const std::wstring& path);

    // [Item 3] Dock-slide viewport interpolation. The web sends ONE
    // animate-scene-rect at the dock open/close toggle; the host then re-renders
    // the engine at a wall-clock-lerped scene rect every render frame, synced to
    // the CSS flex-grow tween, so the viewport edge glides with the panel instead
    // of juddering against the clumpy per-frame scene-rect stream. `from`/`to`
    // are scene rects in MAIN-HWND-CLIENT device px (same space as SetSceneRect);
    // `from` is the live on-screen edge the web measured at toggle. `durationMs`
    // matches the CSS duration; `msElapsedAtSend` (ms since the flex actually
    // changed, web-stamped) back-dates the start clock to the CSS origin across
    // the IPC hop. A no-op when no DComp compositor is attached (e.g.
    // headless --capture).
    void StartSceneAnim(int fromX, int fromY, int fromW, int fromH,
                        int toX, int toY, int toW, int toH,
                        double durationMs, double msElapsedAtSend);

    // Advance an in-flight scene anim to `qpcNow` (QueryPerformanceCounter ticks,
    // the host render clock). Applies the time-lerped rect through the internal
    // apply path; ends the anim (and applies the exact `to`) at t>=1. Returns
    // true while active. A cheap no-op when idle. Call once per render frame,
    // BEFORE engine->Render(), so the engine paints the advanced rect.
    bool AdvanceSceneAnim(long long qpcNow);

    // True while a dock-slide anim owns the scene rect — drives SetSceneRect's
    // self-defense (stray/late scene-rects are dropped mid-anim).
    bool IsSceneAnimActive() const { return m_sceneAnim.active; }

    // Cancel an in-flight dock-slide anim so the static scene-rect path takes
    // over. Called from the viewport RESIZE paths (Apply / PredictAndApply on a
    // size change — incl. a DPR move's Engine::Reset): a resize invalidates the
    // anim's captured absolute-px from/to, so per spec risk #4 we drop the anim
    // (a 1-frame discontinuity is fine; a ~200ms stale-target slide is not). A
    // pure window MOVE (RefreshScreenPosition) does NOT cancel — the scene rect
    // is client-relative, so a move leaves from/to valid.
    void CancelSceneAnim() { m_sceneAnim.active = false; }

    // [resize-perf revised Fix A] Safety-net settle: if the popup size
    // diverged from the size at the last completed engine reset (i.e. a
    // per-tick reset FAILED mid-gesture), reset once now. A cheap no-op
    // in the normal case — every size change resets inline via
    // ResetEngineForResize, so the sizes already match. Called from
    // WM_EXITSIZEMOVE and the quiescence-timer fallback.
    void SettleDeferredReset();

private:
    // Re-emit the cached scene rect to the AlphaCompositor (snapshot crop,
    // popup-client coords) and the DComp Compositor + Engine (render
    // transform, main-client coords). Called after Apply / PredictAndApply /
    // attach because moving the popup changes the popup-client translation.
    void ReemitSceneRect();

    // [Item 3] The real scene-rect application (compositor mask + engine
    // viewport + DComp clip). SetSceneRect is now a thin guard that drops
    // external updates while a dock-slide anim owns the rect; both the guard and
    // the per-frame anim advance funnel the actual work through here.
    // [resize-perf C2] animFrame=true marks a mid-flight anim apply —
    // the DComp transform log is skipped for those (they fire at the
    // render rate); instant applies and anim terminals log normally.
    void ApplySceneRect(int x, int y, int w, int h, bool animFrame = false);

    // [resize-perf revised Fix A] One resize-driven engine reset, shared
    // by Apply / PredictAndApply / SettleDeferredReset (one helper, not
    // three hand-copies —). Tries the cheap ResetEx path
    // (Engine::ResetForResize — textures/shaders persist, ~3-5 ms) and
    // falls back to the full Reset() + RecoverDeviceIfNeeded chain on
    // failure. Updates m_resetW/H bookkeeping. (w, h) is the popup size
    // the reset is FOR.
    void ResetEngineForResize(int w, int h);

    HWND    m_viewport;
    Engine* m_engine;
    // legacy popup band-mask compositor — owned by HostWindow,
    // injected via SetAlphaCompositor.
    AlphaCompositor* m_alphaCompositor = nullptr;
    // Phase 3 Stage 5 — DComp-tree compositor injected by
    // HostWindow when composition mode is active. Non-null IS the
    // composition-mode signal LayoutBroker uses to gate the new
    // SetEngineVisualTransform + Engine::SetSceneViewport calls (per
    // sub-plan §3.3 + R9 mitigation c). Owned by HostWindow.
    Compositor* m_dcompCompositor = nullptr;
    // B1.4 T4c: scene rect in MAIN-HWND-CLIENT coords. (0/0/0/0)
    // disables the compositor mask.
    int     m_sceneX = 0;
    int     m_sceneY = 0;
    int     m_sceneW = 0;
    int     m_sceneH = 0;
    // Track the last applied size so we only fire a (relatively
    // expensive) D3D9 device Reset when the size actually changed.
    int     m_lastW;
    int     m_lastH;
    //: last viewport rect (in main-client coords).
    int     m_lastX = 0;
    int     m_lastY = 0;
    // polish: main's client size at the last Apply.
    int     m_lastClientW = 0;
    int     m_lastClientH = 0;

    // [resize-perf revised Fix A] m_resetW/H track the popup size at the
    // last completed engine reset so SettleDeferredReset can tell whether
    // a per-tick reset failed (m_lastW/H ≠ m_resetW/H) without poking at
    // the engine's presentation parameters.
    int     m_resetW = 0;
    int     m_resetH = 0;

    // [Item 3] In-flight dock-slide interpolation. `active` is false when idle.
    // Rects are MAIN-HWND-CLIENT device px (held as float so the per-frame lerp
    // keeps sub-pixel precision, rounded only at apply). `startQpc` is a QPC tick
    // count back-dated to the CSS origin; `durMs` is the CSS duration.
    struct ViewportAnim
    {
        bool      active   = false;
        float     fromX = 0, fromY = 0, fromW = 0, fromH = 0;
        float     toX   = 0, toY   = 0, toW   = 0, toH   = 0;
        long long startQpc = 0;
        double    durMs    = 0.0;
    };
    ViewportAnim m_sceneAnim;
};

} // namespace host

#endif // HOST_LAYOUT_BROKER_H
