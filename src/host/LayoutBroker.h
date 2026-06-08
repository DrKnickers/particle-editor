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
// (May 2026): the popup is also WS_EX_LAYERED. Occlusion updates
// from React (`viewport/occlude`) used to drive a SetWindowRgn HRGN
// cut-out; they now translate the occlusion rect into popup-client
// coords and forward to AlphaCompositor::SetOcclusion, which
// per-frame stamps alpha (with a smoothstep feather) into the
// readback DIB before UpdateLayeredWindow. LayoutBroker keeps the
// main-client-coord map so it can re-emit translated rects whenever
// the popup moves or resizes.
#ifndef HOST_LAYOUT_BROKER_H
#define HOST_LAYOUT_BROKER_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <unordered_map>

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

    //: inject the layered-window compositor. SetOcclusion /
    // RemoveOcclusion forward to it after translating the rect from
    // main-client coords to popup-client coords. Null is treated as
    // "compositor not installed" — occlusion updates are recorded
    // (so a later attach can replay them) but no stamping happens.
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
    // scene-rect (from React via layout/scene-rect) masks everything
    // outside the centre quadrant via AlphaCompositor band stamps.
    //
    // ApplyFullClient is equivalent to Apply(0, 0, clientW, clientH)
    // using the owner's current GetClientRect — wrapped here so
    // callers don't need to repeat the GetWindow / GetClientRect
    // boilerplate, and so the "popup = full client" invariant lives
    // in one place.
    void ApplyFullClient();

    // Register an occlusion rect (in main-client coords) from React.
    // `id` is a stable handle the React side uses (e.g.
    // "tool-panel:background", "menu:file"). Passing a null rect
    // (w<=0 or h<=0) removes the occlusion for that id. `feather` is
    // the AlphaCompositor smoothstep band width (in physical pixels)
    // at the rect's unclipped edges — 0 for a hard cut, ~padding-px
    // when the React caller padded the rect to absorb a shadow.
    void SetOcclusion(const std::string& id, int x, int y, int w, int h, int feather = 0);
    void RemoveOcclusion(const std::string& id);

    // B1.4 T4c: set the scene rect (the visible centre-quadrant
    // sub-rect inside the popup) in MAIN-HWND-CLIENT coords. LayoutBroker
    // translates to popup-client coords (using the current popup origin)
    // and forwards to AlphaCompositor. The compositor stamps alpha=0
    // for the four bands outside this rect each frame.
    //
    // The rect is cached so it can be re-emitted with a fresh translation
    // whenever the popup moves or resizes (Apply / PredictAndApply).
    //
    // Passing w<=0 or h<=0 clears the scene rect (the compositor's
    // mask becomes a no-op — the full popup shows rendered scene).
    void SetSceneRect(int x, int y, int w, int h);

    // Phase 3 Stage 5 — read the cached scene rect (in
    // main-client coords). Returns true and populates the outs when a
    // non-degenerate scene rect has been dispatched at least once;
    // returns false (outs untouched) otherwise. Used by HostWindow at
    // composition-controller-ready time to seed the engine visual's
    // initial transform without waiting for React's next dispatch
    // (sub-plan §3.5 — avoids a 1-3 frame full-client glitch).
    bool GetSceneRect(int& x, int& y, int& w, int& h) const;

    // (session 3): forward the theme background colour to the DComp
    // compositor's rearmost backing visual (composition mode only).
    // No-op when no Compositor is attached (legacy arch-A path). `color`
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

private:
    //: forward all currently-registered occlusions to the
    // compositor, translated to popup-client coords using the current
    // popup origin (m_lastX, m_lastY). Called after Apply /
    // PredictAndApply / RefreshScreenPosition because moving the
    // popup changes the popup-client coord of every occlusion.
    void ReemitOcclusions();

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
    // Occlusion rects from React (main-client coords). Each id maps
    // to one rect; null/zero-size removes the entry.
    struct Occlusion { int x, y, w, h; int feather; };
    std::unordered_map<std::string, Occlusion> m_occlusions;
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
};

} // namespace host

#endif // HOST_LAYOUT_BROKER_H
