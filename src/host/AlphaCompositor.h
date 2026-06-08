// AlphaCompositor — bridges D3D9 rendering to a WS_EX_LAYERED top-level
// popup via UpdateLayeredWindow. Replaces/'s SetWindowRgn cut-out
// with software alpha-stamping: the engine still renders fully opaque
// pixels into an off-screen ARGB RT, the readback path stamps alpha=0
// (with a smoothstep feather) inside chrome occlusion rectangles, and
// the layered popup composites with the WebView2 underneath so menu
// dropdowns / tool panels show through cleanly.
//
// Pipeline per frame:
//   1. Engine renders the scene into the off-screen D3DFMT_A8R8G8B8 RT
//      we own (GetRenderTarget()).
//   2. Engine calls Composite(layeredHwnd).
//   3. We GetRenderTargetData → D3DPOOL_SYSTEMMEM surface.
//   4. LockRect, memcpy into a CreateDIBSection-allocated bitmap.
//   5. For each registered occlusion, stamp the DIB alpha bytes to 0
//      inside the rect, smoothstep-ramp on the feather band.
//   6. UpdateLayeredWindow(ULW_ALPHA) pushes the bitmap to the popup.
//
// Resize() must be called whenever the popup HWND's client area changes.
// The RT, system-mem surface, and DIB are all reallocated to match.
//
// SetOcclusion / RemoveOcclusion are single-threaded (UI thread only)
// — they match the existing LayoutBroker invariant. The map is mutated
// only between Render frames.
//
//. See tasks/todo.md.
#ifndef HOST_ALPHA_COMPOSITOR_H
#define HOST_ALPHA_COMPOSITOR_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace host {

class AlphaCompositor
{
public:
    // The device must outlive the AlphaCompositor. HostWindowImpl
    // guarantees this: Engine owns the device, the compositor is
    // detached on WM_DESTROY before either is destroyed.
    explicit AlphaCompositor(IDirect3DDevice9* device);
    ~AlphaCompositor();

    AlphaCompositor(const AlphaCompositor&)            = delete;
    AlphaCompositor& operator=(const AlphaCompositor&) = delete;

    // Resize the internal RT/sysmem/DIB to match the popup's client
    // size. Idempotent when (w, h) is unchanged. Throws
    // std::runtime_error on allocation failure.
    void Resize(int width, int height);

    // Release the D3DPOOL_DEFAULT render target (and friends) so
    // IDirect3DDevice9::Reset can succeed — Reset returns
    // D3DERR_INVALIDCALL if any POOL_DEFAULT resource is still
    // outstanding. Engine::Reset must call this BEFORE m_pDevice->Reset,
    // and the post-Reset Resize() recreates everything at the new
    // back-buffer size.
    void ReleaseGpuResources();

    // The off-screen ARGB render target Engine should set on slot 0
    // at the start of Render(). Returns nullptr until Resize() has
    // been called with a non-degenerate size.
    IDirect3DSurface9* GetRenderTarget() const;

    // Register / replace an occlusion rectangle. `rectClient` is in
    // popup-client coords (same coord space as the DIB). `feather`
    // is the smoothstep band (in pixels) at the rect's outer edge;
    // 0 disables feathering (hard rectangular alpha cut, same shape
    // as the old SetWindowRgn).
    void SetOcclusion(std::string id, RECT rectClient, int feather = 3);

    // Remove a previously registered occlusion. No-op if id unknown.
    void RemoveOcclusion(const std::string& id);

    // B1.4 T4c: the "scene rect" is the visible centre-quadrant
    // sub-rect inside the popup, in popup-client coords. Each Composite
    // call stamps alpha=0 for the four bands OUTSIDE this rect (hard
    // cut, no smoothstep). Layered-window compositing makes those
    // bands transparent for both rendering and hit-testing, so the UI
    // panels behind them show through and receive their own mouse
    // events.
    //
    // The stamps run BEFORE the existing per-id smoothstep occlusion
    // pass (which can then re-stamp tool-panel cutouts INSIDE the
    // scene rect with their soft falloff). The pre-stamp DIB cache
    // for B1.3.1.1's snapshot capture runs FIRST so the raw engine
    // pixels are preserved untouched — T4c.5 will crop the cached
    // DIB to this scene rect before PNG encoding.
    //
    // A zero-width or zero-height rect disables the mask entirely
    // (the full popup shows the rendered scene, which is the pre-T4c
    // behaviour — used during host boot before React has dispatched
    // the first layout/scene-rect).
    void SetSceneRect(int x, int y, int w, int h);

    // B1.3.1.1: capture the most recent pre-stamp engine frame as a
    // base64-encoded PNG. `outBase64` is filled with the PNG payload
    // (no "data:image/png;base64," prefix — the caller adds that).
    // `outW` / `outH` are the snapshot pixel dimensions. Returns
    // `false` and leaves outputs untouched when no frame has been
    // composited yet (e.g. the engine never rendered or the device
    // just reset). Safe to call from the UI thread between frames;
    // GDI+ must have been initialized by the host (HostWindow::Run).
    //
    // The snapshot reflects the engine's raw output BEFORE chrome
    // occlusion stamps and BEFORE modal-mask dim/blur — exactly the
    // pixels React's <img src=...> backdrop wants to see, so CSS
    // effects above can dim + blur it uniformly with the panels.
    bool CaptureSnapshotPng(std::string& outBase64, int& outW, int& outH);

    // rendering-fidelity] Write the most recent pre-stamp engine
    // frame straight to a PNG file at `path`. Same readback + crop +
    // GDI+ encode as CaptureSnapshotPng, but saves to disk instead of
    // returning base64 — used by the `--capture` CLI mode so rendering
    // fidelity can be inspected/diffed offline without a screen. Returns
    // false on the same conditions as CaptureSnapshotPng (no frame, no
    // RT, encoder unavailable, or the file write failing). GDI+ must be
    // initialized by the host (HostWindow::Run). UI-thread only.
    bool CaptureSnapshotToFile(const std::wstring& path);

    // Phase 0 spike — Path A (JPEG via WebResourceRequested).
    // Encode the cached pre-stamp engine pixels as JPEG bytes. Output
    // buffer is resized to fit the encoded payload. `quality` is 1-100
    // (clamped); typical spike values: 70, 85, 95. Crops to the current
    // scene rect if one has been set (same crop semantics as
    // CaptureSnapshotPng). Returns false if no frame has been composited
    // yet OR the JPEG encoder is unavailable; outputs are left untouched
    // on failure. UI-thread only.
    //
    // Bytes-out is raw JPEG (the file format), suitable for inclusion as
    // the body of a WebResourceRequested response with
    // Content-Type: image/jpeg. The renderer-side `<canvas>` paints via
    // createImageBitmap(blob) → ctx.drawImage.
    bool EncodeFrameJpeg(int quality, std::vector<uint8_t>& outBytes,
                         int& outW, int& outH);

    // Per-frame readback + occlusion stamp + UpdateLayeredWindow push.
    // No-op if Resize hasn't run or the HWND is null.
    void Composite(HWND layeredHwnd);

    // Phase 3 Stage 2: NT-handle alias of the offscreen render
    // target, allocated in Resize via CreateTexture(USAGE_RENDERTARGET,
    // D3DPOOL_DEFAULT, &sharedHandle). A parallel D3D11 device opens it
    // via OpenSharedResource (validated end-to-end in the dxgi_spike at
    // commit 6c00536). Returns nullptr when Resize hasn't run or the
    // underlying device isn't D3D9Ex. The handle is owned by the source
    // texture — D3D11 callers do NOT CloseHandle when done.
    HANDLE GetSharedHandle() const;

    // Phase 3 Stage 1 follow-up: enable the per-frame pre-stamp
    // DIB cache used by EncodeFrameJpeg in the (canvas-JPEG)
    // transport. Off by default — the legacy layered-popup path
    // (arch B) only reads the cache for rare modal-open snapshots,
    // which now do their own on-demand readback inside
    // CaptureSnapshotPng. Leaving the cache off in arch B reclaims a
    // ~19 MB memcpy per frame (~2-5 ms at 3440×1440). FramePublisher's
    // constructor flips this on; nothing else needs to.
    //
    // Idempotent. The disable path also frees the cache buffer so
    // arch-A → → arch-A toggles don't leak.
    void SetPerFrameCacheEnabled(bool enabled);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace host

#endif // HOST_ALPHA_COMPOSITOR_H
