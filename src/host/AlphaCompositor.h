// AlphaCompositor — owns the off-screen D3DFMT_A8R8G8B8 render target the
// engine draws into, exposed two ways:
//
//   1. As a cross-device SHARED-HANDLE texture (GetSharedHandle) the host's
//      Compositor opens from D3D11 and composites into its DComp visual —
//      the live render transport (host::Compositor::CompositeEngineFrame).
//   2. As an on-demand SNAPSHOT (CaptureSnapshotPng / CaptureSnapshotToFile)
//      read back via GetRenderTargetData: the modal frosted-glass backdrop
//      (JPEG, base64) and the `--capture` offline-diff path (PNG to disk).
//
// Resize() must be called whenever the viewport client area changes; the RT
// and its SYSTEMMEM readback surface are reallocated to match. The engine
// renders into GetRenderTarget() each frame; the host presents those pixels
// via DComp, so the engine itself never presents while a compositor is
// attached.
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

    // B1.4 T4c: the "scene rect" is the visible viewport
    // sub-region (the centre quadrant), in viewport-client coords.
    // CaptureSnapshotPng / CaptureSnapshotToFile crop the readback to
    // this rect so the modal backdrop / --capture PNG show only the
    // pixels the user sees, not the offstage engine content that sits
    // under the side panels.
    //
    // A zero-width or zero-height rect disables the crop (the full RT
    // is captured) — the host-boot default before React dispatches the
    // first layout/scene-rect.
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

    // Phase 3 Stage 2: NT-handle alias of the offscreen render
    // target, allocated in Resize via CreateTexture(USAGE_RENDERTARGET,
    // D3DPOOL_DEFAULT, &sharedHandle). A parallel D3D11 device opens it
    // via OpenSharedResource (validated end-to-end in the dxgi_spike at
    // commit 6c00536). Returns nullptr when Resize hasn't run or the
    // underlying device isn't D3D9Ex. The handle is owned by the source
    // texture — D3D11 callers do NOT CloseHandle when done.
    HANDLE GetSharedHandle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace host

#endif // HOST_ALPHA_COMPOSITOR_H
