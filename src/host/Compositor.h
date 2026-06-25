// Compositor — owns the DirectComposition V1 visual tree that hosts
// WebView2 in composition mode: WebView2 runs in
// composition mode (CreateCoreWebView2CompositionController) rather
// than HWND mode (CreateCoreWebView2Controller).
//
// This class is the production counterpart of the InitDComp +
// BuildVisualTree + Shutdown sections in src/host/spike/dxgi_spike.cpp
// — the working reference topology from the spike on the
// user's RTX 3080. Implementation details (DComp device, target,
// visual ComPtrs) live in Compositor.cpp via the pImpl idiom so
// consumers of this header (HostWindow.cpp) don't have
// to pull in <dcomp.h> / <d3d11.h> / <dxgi1_2.h>. That matters here
// because ParticleEditor.vcxproj puts the legacy DXSDK include path
// FIRST (for d3dx9.h), and DXSDK's stale DXGI.h / D3D11.h / Dcommon.h
// shadow the Win10 SDK versions dcomp.h expects. Keeping dcomp.h to a
// single translation unit (Compositor.cpp) with a per-file
// AdditionalIncludeDirectories override that excludes DXSDK is the
// isolation pattern — see Compositor.cpp's header comment.
//
// Spike invariants preserved in the implementation:
//   - V2 factory function DCompositionCreateDevice2(nullptr, ...),
//     V1 IDCompositionDevice IID — matches WebView2APISample on the
//     known-good baseline.
//   - CreateTargetForHwnd(hwnd, TRUE, ...) — topmost=TRUE.
//   - Visual tree built ONLY after the WebView2 composition controller
//     exists (deferred construction).
//   - AddVisual(visual, FALSE, nullptr) puts the visual in front of
//     all siblings; MSDN's naming is counterintuitive ("insertAbove
//     = FALSE + null ref" = "above all," NOT "behind all"). Bisected
//     via the spike's --no-engine mode.
//   - put_RootVisualTarget(webviewVisual) AFTER the visual is in the
//     tree.
//   - Commit() once at end of AttachWebView2; subsequent state
//     changes (size, transform) call Commit() themselves.
//
// The WebView2 visual is one child of the root; the engine D3D11
// swapchain visual is added as a sibling via the AttachEngineVisual()
// seam.
//
// Lifecycle ownership note. put_RootVisualTarget makes WebView2 hold
// a reference to the internal webview visual. The CALLER must tear
// down WebView2 FIRST (close the controller, release the
// controller/view pointers) before this Compositor is destroyed,
// otherwise WebView2 will hold a dangling reference to a released
// visual when its async work settles. HostWindow.cpp's WM_DESTROY
// enforces this ordering.

#ifndef HOST_COMPOSITOR_H
#define HOST_COMPOSITOR_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>
#include <memory>
#include <string>

// Forward declaration of WebView2 composition controller. The
// implementation file pulls in WebView2.h to use it; the header just
// needs the type name. (We CAN'T forward-declare it as `struct`
// because WebView2.h declares it as `interface ...` which is just
// `struct` under the hood but MSVC accepts the mismatch in
// declarations only if the type isn't fully used. As a method
// parameter to AttachWebView2, the pointer-to-incomplete-type is
// fine because we don't dereference it in the header.)
struct ICoreWebView2CompositionController;

namespace host {

class Compositor
{
public:
    // Optional logger callback. HostWindowImpl binds a lambda that
    // fans through Log() so [COMP] lines land in
    // %LOCALAPPDATA%\AloParticleEditor\host.log. Mirrors the
    // InputDispatcher pattern.
    using LogFn = std::function<void(const std::string& line)>;

    // ctor stores the host HWND; no DComp work yet. Init() does the
    // device creation. dtor releases the visual tree in the order
    // documented in the file header.
    Compositor(HWND hostHwnd, LogFn log) noexcept;
    ~Compositor();

    Compositor(const Compositor&)            = delete;
    Compositor& operator=(const Compositor&) = delete;

    // Create the DComp V1 device + target (idempotent — calling
    // again is a no-op if already up). Returns S_OK on success;
    // failure HRESULT is logged via the LogFn and propagated to the
    // caller for env-var-gated fallback to HWND mode.
    HRESULT Init();

    // Build the visual tree with the WebView2 composition surface
    // as the sole child of the root, plug the controller's
    // RootVisualTarget into the visual, and Commit. MUST be called
    // from the composition controller completion callback, not from
    // Init() (deferred tree construction). Idempotent
    // — second call is a no-op + returns S_OK.
    HRESULT AttachWebView2(ICoreWebView2CompositionController* ctl);

    // Update the root visual's clip / offset for a new host client
    // size. Calls Commit() internally. No-op if dimensions are
    // unchanged. Uses the full client rect; a scene-rect transform
    // for the engine visual is applied separately.
    HRESULT SetSize(int width, int height);

    // Explicit Commit — exposed so the caller can batch state
    // changes (multiple SetSize / future transforms) and commit
    // once. Most callers don't need this — SetSize / AttachWebView2
    // commit themselves.
    HRESULT Commit();

    // ---------- engine visual ----------

    // Stand up the D3D11 device + DXGI factory, open the
    // engine's shared texture as a D3D11 alias, create a composition
    // swapchain, build the engine IDCompositionVisual, insert it
    // BEHIND the WebView2 visual (via AddVisual(engine, TRUE, nullptr)
    // — the spike-bisected MSDN-naming inversion places "behind all
    // siblings"; see Compositor.cpp's body + dxgi_spike.cpp:488 for
    // the long-form comment), and SetContent(swapchain). Idempotent
    // on identical (sharedTexture, w, h). A different handle triggers
    // re-open via the lazy detection in CompositeEngineFrame.
    //
    // On any failure (LUID mismatch, D3D11 device create, OpenShared
    // Resource, swapchain create), returns the failure HRESULT and
    // leaves the engine visual NOT attached. Caller logs
    // [COMP-engine-fail] and continues with composition mode intact
    // (chrome works in composition mode; viewport area is empty).
    // By design, a failure here does NOT chain into the HWND-mode
    // fallback.
    //
    // Engine-side cross-device sync (D3D9 event query) is owned by
    // Engine; host orchestrates the spin between engine->Render() and
    // CompositeEngineFrame() — see Engine::IssueEndFrameQuery /
    // WaitEndFrameQuery path (b).
    //
    // engineAdapterLuid: caller (HostWindow) passes
    // engine->GetAdapterLuid(). On hybrid-GPU laptops where D3D9Ex
    // and D3D11 pick different adapters, OpenSharedResource silently
    // returns a wrong texture — the LUID compare catches that case
    // before any pixels are copied. A zero LUID (`{0,0}`) means
    // "caller doesn't know; skip the check" — single-GPU production
    // paths can pass `LUID{}` if they don't care to wire the helper.
    // Mismatch returns DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE so the
    // caller can distinguish LUID failure from other failure modes;
    // composition mode stays intact, engine visual is NOT attached.
    HRESULT AttachEngineVisual(HANDLE sharedTexture,
                               int    w,
                               int    h,
                               LUID   engineAdapterLuid) noexcept;

    // Per-frame composite step. Called from HostWindow's RenderD3D9
    // path AFTER engine->Render() + engine->WaitEndFrameQuery() under
    // composition mode. Sequence:
    //   1. Lazy handle check against the cached AttachEngineVisual
    //      tuple — re-open via RefreshEngineSharedHandle if changed.
    //   2. D3D11 immediate context: CopyResource(backBuffer, alias).
    //   3. swapChain->Present1(0, 0, &emptyParams).
    //
    // currentSharedHandle: caller (HostWindow) passes engine->
    // GetSharedTextureHandle() every frame. AlphaCompositor::Resize
    // invalidates the previous handle and creates a new one every
    // time the engine's RT is resized; the lazy compare against the
    // cached handle catches this and re-opens the D3D11 alias +
    // ResizeBuffers on the swapchain (4d). A single mismatched frame
    // costs one re-open and proceeds normally; the steady state is
    // a pointer-compare per frame with no extra work.
    //
    // Returns S_OK on success; S_FALSE when no engine visual is
    // attached (webview-only baseline state, or AttachEngineVisual-failed
    // state — chrome works, viewport empty).
    HRESULT CompositeEngineFrame(HANDLE currentSharedHandle) noexcept;

    // Drop the D3D11 alias and re-open against a fresh shared handle.
    // Called implicitly by CompositeEngineFrame's lazy handle-mismatch
    // path (the AlphaCompositor::Resize path invalidates the shared
    // HANDLE on every resize — every resize creates a new handle).
    // Exposed publicly so HostWindow can trigger an eager re-open if
    // the lazy per-frame detection isn't responsive enough for some
    // future use case. Re-open is lazy-only by default; the
    // explicit-call path stays available for diagnostic / future use.
    //
    // The (hintW, hintH) params are advisory — the actual swapchain
    // resize uses the texture's D3D11_TEXTURE2D_DESC width/height
    // after OpenSharedResource (the engine knows the actual size; the
    // caller's hint may be stale). Pass 0, 0 if you don't have a
    // hint; ignored either way.
    HRESULT RefreshEngineSharedHandle(HANDLE sharedTexture,
                                      int    hintW,
                                      int    hintH) noexcept;

    // Scene-rect transform on the engine
    // visual. Constrains the DComp engine visual to the scene-rect
    // sub-region of the host client so chrome panels stop bleeding
    // engine pixels.
    //
    // (x, y, w, h) is in host-client coordinates. The engine visual is
    // a direct child of the root visual whose coordinate space equals
    // host-client coords; no translation is required.
    //
    // COORDINATE-SPACE CONTRACT (post-fix). The engine
    // renders its scene into the (x, y, w, h) sub-region of its
    // full-client RT, so the swapchain back-buffer carries the scene
    // at pixels [x..x+w, y..y+h] and engine clear color elsewhere.
    // This method:
    //   - SetOffsetX(0) + SetOffsetY(0) — visual local origin = parent
    //     (root visual) origin = host-client origin. Swapchain pixel
    //     (px, py) paints at host-client (px, py).
    //   - SetClip({x, y, x+w, y+h}) in the visual's local coord space
    //     (which equals host-client coords here) — constrains the
    //     visible region to the scene-rect rectangle.
    // Combined effect: only the scene-rect rectangle of the engine RT
    // is visible on-screen, painted at the same coords. Chrome panel
    // backgrounds show through everywhere else.
    //
    // Idempotent on identical args. Returns:
    //   S_OK            — applied successfully (or no-op for idempotent)
    //   S_FALSE         — engine visual not yet attached
    //                     (AttachEngineVisual hasn't succeeded yet, or
    //                     failed — caller should treat
    //                     this as "not ready, retry later")
    //   E_INVALIDARG    — w <= 0 or h <= 0
    //   E_NOT_VALID_STATE — DComp device or engine visual missing
    //   other HRESULT   — propagated from SetOffset/SetClip/Commit
    //
    // Emits `[COMP-engine-transform] clip=(L,T,R,B) (absolute host-client)`
    // on actual changes (idempotent cases are silent).
    //
    // Defer semantics. By default (immediate=false), the transform is
    // QUEUED — it doesn't apply until the END of the next
    // CompositeEngineFrame, AFTER Present1 has pushed fresh swapchain
    // pixels for the new viewport. This synchronizes the DComp clip
    // update with the engine's render output, eliminating the
    // transient "stale clear-color strip" the user would otherwise
    // see at the new clip edges during a resize storm (the clip
    // widens immediately on Commit but engine pixels lag one frame).
    //
    // For the attach-time initial seed (where no engine render has
    // happened yet and the deferral path has nothing to coordinate
    // with), pass immediate=true to apply the transform straight
    // through.
    //
    // [resize-perf] quiet=true skips the [COMP-engine-transform]
    // line for THIS apply — used by per-frame anim applies (dock
    // slide / scene-rect chase), which would otherwise log + fflush
    // at the render rate. Instant and anim-TERMINAL applies keep
    // logging, so host.log always shows the settled clip and the log
    // rate self-throttles by construction.
    HRESULT SetEngineVisualTransform(int x, int y, int w, int h, bool immediate = false, bool quiet = false) noexcept;

    // theme-coloured composition backing. Recolour
    // the rearmost backing visual so every transparent DOM region
    // outside the scene rect (panel gaps, splitter seams, rounded-corner
    // wedges) composites over the app-shell `--bg` instead of the black
    // host backing.
    //
    // The backing is a 1x1 composition swapchain scaled to the full
    // client via the visual transform — minimal VRAM, no ResizeBuffers
    // churn during resize storms. It is inserted as the REARMOST child
    // of the root visual (behind the engine visual), created lazily on
    // the first call. `color` is a COLORREF (0x00BBGGRR per the Win32
    // RGB macro); alpha is forced opaque. Idempotent on an unchanged
    // colour.
    //
    // Safe to call before or after AttachWebView2 / AttachEngineVisual:
    //   - Before the tree is built (no root visual): the colour is
    //     cached and applied when AttachWebView2 commits the tree.
    //   - The child order is re-asserted after each engine attach so the
    //     backing always stays behind the engine visual.
    //
    // Returns S_OK on success (or cached-for-later), or a failure
    // HRESULT (logged as [COMP-backing-fail]); on failure the tree is
    // left intact and the backing falls back to today's black — no worse
    // than the prior behaviour.
    HRESULT SetBackingColor(COLORREF color) noexcept;

    // ---------------------------------------------------------------

    // Diagnostic accessors. IsReady() returns true after
    // AttachWebView2 has committed the tree at least once.
    bool IsReady() const noexcept;
    HWND HostHwnd() const noexcept;

private:
    // pImpl — keeps dcomp.h / d3d11.h / dxgi1_2.h out of this header.
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace host

#endif // HOST_COMPOSITOR_H
