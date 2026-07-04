// AlphaCompositor implementation. See AlphaCompositor.h for the design.

#include "AlphaCompositor.h"
#include "GdiplusEncode.h"   // host::GdiplusEncoderClsid / host::Base64Encode

#include <d3d9.h>
#include <wrl/client.h>

#include <objbase.h>
#include <gdiplus.h>
#include <gdiplusimaging.h>
#pragma comment(lib, "gdiplus.lib")

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

namespace host {

namespace {

void ThrowIfFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr))
    {
        char buf[256];
        _snprintf_s(buf, _TRUNCATE,
                    "AlphaCompositor: %s failed hr=0x%08lX", what,
                    static_cast<unsigned long>(hr));
        throw std::runtime_error(buf);
    }
}

} // namespace

struct AlphaCompositor::Impl
{
    Microsoft::WRL::ComPtr<IDirect3DDevice9>  device;
    // Shared-handle texture promoted from the
    // prior CreateRenderTarget surface. CreateTexture with USAGE_RENDER
    // TARGET + D3DPOOL_DEFAULT + a non-null pSharedHandle out-param is
    // the D3D9Ex idiom for cross-device shareable RTs (validated in the
    // dxgi_spike at commit 6c00536). sharedTex.GetSurfaceLevel(0) is
    // the same IDirect3DSurface9 the engine renders into via slot 0,
    // so the existing Render() chain is unchanged. sharedHandle is an
    // NT alias D3D11 can open via OpenSharedResource on the compositor side.
    Microsoft::WRL::ComPtr<IDirect3DTexture9> sharedTex;
    HANDLE                                    sharedHandle = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> offscreenRT;   // sharedTex level-0, ARGB
    Microsoft::WRL::ComPtr<IDirect3DSurface9> sysMemSurface; // D3DPOOL_SYSTEMMEM, readback
    int      width      = 0;
    int      height     = 0;

    // The scene rect — the visible viewport sub-region
    // (viewport-client coords) that CaptureSnapshotPng / CaptureSnapshot
    // ToFile crop the readback to. Default (0/0/0/0) disables the crop
    // (full RT) — the host-boot default before React dispatches the
    // first layout/scene-rect.
    int      sceneX = 0;
    int      sceneY = 0;
    int      sceneW = 0;
    int      sceneH = 0;
};

AlphaCompositor::AlphaCompositor(IDirect3DDevice9* device)
    : m_impl(std::make_unique<Impl>())
{
    if (!device) throw std::invalid_argument("AlphaCompositor: null device");
    m_impl->device = device;
}

AlphaCompositor::~AlphaCompositor() = default;

void AlphaCompositor::Resize(int w, int h)
{
    if (w == m_impl->width && h == m_impl->height) return;
    if (w <= 0 || h <= 0) return;

    // Transactional rebuild. Build every new resource into
    // LOCALS first; only once they all succeed do we release the old set and
    // move the locals into m_impl. Pre-fix this freed all old resources up
    // front and then allocated — so a single failed Create* (transient VRAM /
    // GDI exhaustion: alt-tab from a fullscreen game, a driver TDR) left the
    // compositor half-destroyed (old gone, new partial, width/height stale),
    // i.e. a dead viewport until process restart. With the swap, any failure
    // throws with m_impl untouched, so the compositor keeps compositing the
    // old size and the next Resize can retry cleanly.
    Microsoft::WRL::ComPtr<IDirect3DTexture9> newTex;
    HANDLE                                    newHandle = nullptr; // owned by newTex
    Microsoft::WRL::ComPtr<IDirect3DSurface9> newRT;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> newSys;

    try
    {
        // Shared-handle render-target texture.
        // CreateTexture with USAGE_RENDERTARGET + D3DPOOL_DEFAULT and a
        // non-null pSharedHandle yields an NT-handle alias openable from a
        // parallel D3D11 device via OpenSharedResource. The level-0 surface
        // serves as the engine's render target slot 0, so the existing
        // render+readback path is structurally unchanged. The
        // sharedHandle out-param is populated only when the device is
        // D3D9Ex (the device-creation path hard-fails otherwise, so this is
        // always the case here). D3DMULTISAMPLE_NONE because GetRenderTargetData
        // rejects multisampled sources; scene AA still handled via texturing.
        HRESULT hr = m_impl->device->CreateTexture(
            static_cast<UINT>(w), static_cast<UINT>(h),
            1 /*levels*/, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &newTex, &newHandle);
        ThrowIfFailed(hr, "CreateTexture(shared RT)");
        hr = newTex->GetSurfaceLevel(0, &newRT);
        ThrowIfFailed(hr, "GetSurfaceLevel(0)");

        // Readback target. SYSTEMMEM is the only pool that
        // GetRenderTargetData can write to.
        hr = m_impl->device->CreateOffscreenPlainSurface(
            static_cast<UINT>(w), static_cast<UINT>(h),
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
            &newSys, nullptr);
        ThrowIfFailed(hr, "CreateOffscreenPlainSurface");
    }
    catch (...)
    {
        // ComPtr locals auto-release during unwind; m_impl is left entirely
        // untouched, so the old resources stay live and valid and the next
        // Resize can retry cleanly.
        throw;
    }

    // All allocations succeeded — commit. Release the old resources, then
    // move the locals in. The old sharedHandle is owned by the old sharedTex
    // — releasing the texture invalidates it, no explicit CloseHandle.
    m_impl->offscreenRT.Reset();
    m_impl->sharedTex.Reset();
    m_impl->sharedHandle = nullptr;
    m_impl->sysMemSurface.Reset();

    m_impl->sharedTex     = std::move(newTex);
    m_impl->sharedHandle  = newHandle;
    m_impl->offscreenRT   = std::move(newRT);
    m_impl->sysMemSurface = std::move(newSys);
    m_impl->width  = w;
    m_impl->height = h;

    fprintf(stderr, "[AlphaCompositor] shared RT %dx%d handle=%p\n",
            w, h, m_impl->sharedHandle);
    fflush(stderr);
}
void AlphaCompositor::ReleaseGpuResources()
{
    m_impl->offscreenRT.Reset();
    m_impl->sharedTex.Reset();
    m_impl->sharedHandle = nullptr;
    m_impl->sysMemSurface.Reset();
    // Clear cached size so the next Resize doesn't short-circuit on
    // "(w,h) unchanged" — width and height stay 0 until the next
    // successful allocation.
    m_impl->width  = 0;
    m_impl->height = 0;
}

IDirect3DSurface9* AlphaCompositor::GetRenderTarget() const { return m_impl->offscreenRT.Get(); }

HANDLE AlphaCompositor::GetSharedHandle() const { return m_impl->sharedHandle; }

void AlphaCompositor::SetSceneRect(int x, int y, int w, int h)
{
    m_impl->sceneX = x;
    m_impl->sceneY = y;
    m_impl->sceneW = w;
    m_impl->sceneH = h;
}

// The GDI+ encoder-CLSID lookup (PNG + JPEG) and the base64 encoder now live in
// host/GdiplusEncode.h: host::GdiplusEncoderClsid(mime, …)
// + host::Base64Encode(…). Was four copy-pasted CLSID lookups + two identical
// Base64 copies across AlphaCompositor / WindowCapture / PaletteThumbs.

bool AlphaCompositor::CaptureSnapshotPng(std::string& outBase64, int& outW, int& outH)
{
    // On-demand readback: a one-shot GetRenderTargetData at snapshot time
    // (~12-15 ms, imperceptible vs. the ~50-100 ms dialog mount + React
    // reflow that triggers us). Modal opens are the only consumer, so there
    // is no per-frame cost.
    //
    // The maximized case (3440x1369) still cost ~69 ms because
    // the readback, the ~19 MB memcpy AND the GDI+ DrawImage downscale
    // all ran at full RT size — only the *encode* saw the small image.
    // The fast path below moves the crop+downscale onto the GPU with a
    // single StretchRect into a small render target, so every step after
    // it operates on the already-small (~1024x383) image. It falls back
    // to the proven full-readback path (further down) when the device
    // lacks the StretchRect caps or the GPU path hits any failure, so
    // there is zero behavioural regression.
    //
    // Safety: offscreenRT holds the engine's rendered pixels and nothing
    // mutates it on the CPU side, so between Engine::Render calls it is
    // always re-readable. During the Win32 modal sizing loop, Engine::Render
    // also doesn't run, so offscreenRT holds the
    // pre-resize frame — exactly the backdrop the modal wants.
    if (!m_impl->offscreenRT || !m_impl->sysMemSurface) return false;
    if (m_impl->width <= 0 || m_impl->height <= 0)     return false;

    // Encode the backdrop as JPEG, not PNG. It's only ever shown
    // blurred under Dialog.Overlay's backdrop-blur-sm, so lossy is invisible;
    // JPEG encodes several times faster than the GDI+ PNG path and transmits
    // ~10x fewer bytes (base64 + IPC + browser decode), which is what
    // dominated the maximized latency once the StretchRect fast path below cut
    // the readback. (CaptureSnapshotToFile keeps PNG — the lossless --capture
    // offline-diff path.)
    CLSID jpegClsid = {};
    if (!host::GdiplusEncoderClsid(L"image/jpeg", jpegClsid)) return false;
    constexpr int kBackdropJpegQuality = 82;  // blurred backdrop — fidelity is moot

#ifndef NDEBUG
    // [CACHE-DEFERRAL-PERF] / [INSTANT-MODAL] timing anchor. Logs once per
    // snapshot call (snapshots are rare — no throttling needed).
    LARGE_INTEGER sQpf{}, sT0{};
    QueryPerformanceFrequency(&sQpf);
    QueryPerformanceCounter(&sT0);
#endif

    const int srcW = m_impl->width;
    const int srcH = m_impl->height;

    // Crop region = the current scene rect (the only sub-region
    // that holds pixels the user sees; encoding the full RT would stretch
    // outside-scene engine content into the modal's backdrop). When no
    // scene rect is set (boot, or harnesses that drive CaptureSnapshotPng
    // without a layout/scene-rect dispatch), fall back to the full RT. This
    // needs only width/height, so it runs BEFORE any readback and is shared
    // by both the fast (StretchRect) and slow (full-readback) paths.
    int cropX = 0;
    int cropY = 0;
    int cropW = srcW;
    int cropH = srcH;
    if (m_impl->sceneW > 0 && m_impl->sceneH > 0)
    {
        cropX = (m_impl->sceneX < 0) ? 0 : m_impl->sceneX;
        cropY = (m_impl->sceneY < 0) ? 0 : m_impl->sceneY;
        const int maxW = srcW - cropX;
        const int maxH = srcH - cropY;
        cropW = (m_impl->sceneW < maxW) ? m_impl->sceneW : maxW;
        cropH = (m_impl->sceneH < maxH) ? m_impl->sceneH : maxH;
        if (cropW <= 0 || cropH <= 0) return false;
    }

    // Encoded (downscaled) output dims. The snapshot is
    // only ever shown as a modal's frosted-glass backdrop — Dialog.Overlay
    // paints bg-black/60 + backdrop-blur-sm over it, so a full-res encode is
    // wasted work. Two knobs cut it: kSnapshotMaxEdge caps the long edge
    // (bounding the upscale/softness under the blur — ~3.4x at 3440 -> 1024);
    // kSnapshotDownscale forces a min reduction even for sub-cap (windowed)
    // captures. This formula is reused VERBATIM by both paths so the encoded
    // size is byte-identical to the prior output (the native dim tests and
    // the backdrop-blur "floor" both depend on it — do not retune lightly).
    constexpr int kSnapshotMaxEdge   = 1024;  // upper bound on the encoded long edge
    constexpr int kSnapshotDownscale = 2;     // min downscale factor (windowed snappiness)
    int dstW = cropW;
    int dstH = cropH;
    {
        const int longEdge   = (cropW > cropH) ? cropW : cropH;
        int       targetLong = longEdge / kSnapshotDownscale;
        if (targetLong > kSnapshotMaxEdge) targetLong = kSnapshotMaxEdge;
        if (targetLong < 1)                targetLong = 1;
        if (targetLong < longEdge)
        {
            const double s = static_cast<double>(targetLong) / longEdge;
            dstW = static_cast<int>(cropW * s + 0.5);
            dstH = static_cast<int>(cropH * s + 0.5);
            if (dstW < 1) dstW = 1;
            if (dstH < 1) dstH = 1;
        }
    }

    // Shared encode tail: PNG-encode `bmp` into outBase64 (+ set the out dims)
    // via an in-memory IStream. CreateStreamOnHGlobal(nullptr, TRUE, ...) lets
    // the stream own its HGLOBAL — released when the IStream releases. Used by
    // BOTH the fast and slow paths; `pathTag` is for the debug latency log.
    auto encodeBitmap = [&](Gdiplus::Bitmap* bmp, const char* pathTag) -> bool
    {
        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
            return false;

        // JPEG quality EncoderParameter (LONG 1..100). qval must outlive
        // the Save call.
        ULONG qval = static_cast<ULONG>(kBackdropJpegQuality);
        Gdiplus::EncoderParameters encParams;
        encParams.Count = 1;
        encParams.Parameter[0].Guid           = Gdiplus::EncoderQuality;
        encParams.Parameter[0].Type           = Gdiplus::EncoderParameterValueTypeLong;
        encParams.Parameter[0].NumberOfValues = 1;
        encParams.Parameter[0].Value          = &qval;

        if (bmp->Save(stream, &jpegClsid, &encParams) != Gdiplus::Ok)
        {
            stream->Release();
            return false;
        }

        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);

        STATSTG stat = {};
        if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        {
            stream->Release();
            return false;
        }
        const size_t imgBytes = static_cast<size_t>(stat.cbSize.QuadPart);
        std::vector<uint8_t> img(imgBytes);
        ULONG read = 0;
        if (FAILED(stream->Read(img.data(), static_cast<ULONG>(imgBytes), &read)) || read != imgBytes)
        {
            stream->Release();
            return false;
        }
        stream->Release();

        outBase64 = host::Base64Encode(img.data(), img.size());
        outW = dstW;
        outH = dstH;

#ifndef NDEBUG
        // [INSTANT-MODAL] Total capture cost (readback → downscale → JPEG
        // encode → base64) — the latency the gated save-changes dialog waits
        // on. `path=` says whether the GPU StretchRect fast path or the
        // full-readback fallback ran. sT0/sQpf come from the anchor above.
        {
            LARGE_INTEGER sT2{};
            QueryPerformanceCounter(&sT2);
            const double totalMs = (sT2.QuadPart - sT0.QuadPart) * 1000.0 /
                                   static_cast<double>(sQpf.QuadPart);
            fprintf(stderr,
                    "[INSTANT-MODAL] snapshotCapture total=%.1f ms path=%s "
                    "(encoded %dx%d from crop %dx%d, jpg=%zu bytes)\n",
                    totalMs, pathTag, dstW, dstH, cropW, cropH, img.size());
            fflush(stderr);
        }
#else
        (void)pathTag;
#endif
        return true;
    };

    // ===== Fast path: GPU StretchRect crop+downscale → small readback.
    // Returns true only if it fully produced outBase64; ANY miss (missing caps,
    // a failed Create*/StretchRect/readback) returns false so we fall through
    // to the proven slow path below.
    auto tryStretchPath = [&]() -> bool
    {
        // A 1:1 copy gains nothing (no downscale) — let the slow path handle it.
        if (dstW >= cropW && dstH >= cropH) return false;

        // offscreenRT is a render-target *texture* surface (CreateTexture +
        // GetSurfaceLevel), so StretchRect from it requires
        // CAN_STRETCHRECT_FROM_TEXTURES; D3DTEXF_LINEAR requires the filter
        // cap. Both are universal on modern HAL but are real preconditions.
        D3DCAPS9 caps = {};
        if (FAILED(m_impl->device->GetDeviceCaps(&caps)))                  return false;
        if (!(caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES))  return false;
        const D3DTEXTUREFILTERTYPE filter =
            (caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFLINEAR)
                ? D3DTEXF_LINEAR : D3DTEXF_POINT;

        // Destination: a small RT (POOL_DEFAULT, same ARGB, non-MSAA) for the
        // StretchRect, plus a matching SYSTEMMEM surface for the readback.
        Microsoft::WRL::ComPtr<IDirect3DSurface9> smallRT;
        if (FAILED(m_impl->device->CreateRenderTarget(
                static_cast<UINT>(dstW), static_cast<UINT>(dstH),
                D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                &smallRT, nullptr)))
            return false;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> smallSys;
        if (FAILED(m_impl->device->CreateOffscreenPlainSurface(
                static_cast<UINT>(dstW), static_cast<UINT>(dstH),
                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &smallSys, nullptr)))
            return false;

        // offscreenRT is the engine's *currently bound* slot-0 render target
        // (engine.cpp:674/943, left bound at :1017; the snapshot runs between
        // Render calls, outside BeginScene/EndScene). StretchRect from the
        // active RT can fail D3DERR_INVALIDCALL, so park slot 0 on the swap-
        // chain back buffer just for the blit, then restore. We touch ONLY
        // slot 0 (not depth); the back buffer is full-size (>= the small RT)
        // and is never presented in this architecture, so this is side-effect-free, and
        // the engine re-binds offscreenRT at the top of every frame regardless.
        Microsoft::WRL::ComPtr<IDirect3DSurface9> savedRT;
        if (FAILED(m_impl->device->GetRenderTarget(0, &savedRT)) || !savedRT) return false;
        Microsoft::WRL::ComPtr<IDirect3DSurface9> backBuf;
        if (FAILED(m_impl->device->GetBackBuffer(
                0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuf)) || !backBuf)
            return false;

        const RECT srcRect{ cropX, cropY, cropX + cropW, cropY + cropH };
        const RECT dstRect{ 0, 0, dstW, dstH };

        if (FAILED(m_impl->device->SetRenderTarget(0, backBuf.Get()))) return false;
        const HRESULT stretchHr = m_impl->device->StretchRect(
            m_impl->offscreenRT.Get(), &srcRect, smallRT.Get(), &dstRect, filter);
        m_impl->device->SetRenderTarget(0, savedRT.Get());  // restore (single point, unconditional)

        if (FAILED(stretchHr)) return false;

        if (FAILED(m_impl->device->GetRenderTargetData(smallRT.Get(), smallSys.Get())))
            return false;

        D3DLOCKED_RECT locked = {};
        if (FAILED(smallSys->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
            return false;

        const int dstStride = dstW * 4;
        std::vector<uint8_t> smallBuf(static_cast<size_t>(dstStride) *
                                      static_cast<size_t>(dstH));
        {
            const auto* src = static_cast<const uint8_t*>(locked.pBits);
            for (int y = 0; y < dstH; ++y)
                memcpy(smallBuf.data() + static_cast<size_t>(y) * dstStride,
                       src + static_cast<size_t>(y) * locked.Pitch,
                       static_cast<size_t>(dstStride));
        }
        smallSys->UnlockRect();

#ifndef NDEBUG
        {
            LARGE_INTEGER rT{};
            QueryPerformanceCounter(&rT);
            const double rMs = (rT.QuadPart - sT0.QuadPart) * 1000.0 /
                               static_cast<double>(sQpf.QuadPart);
            fprintf(stderr,
                    "[CACHE-DEFERRAL-PERF] snapshotReadback=%.3f ms (fast %dx%d "
                    "filter=%s stretchHr=0x%08lX)\n",
                    rMs, dstW, dstH,
                    (filter == D3DTEXF_LINEAR) ? "LINEAR" : "POINT",
                    static_cast<unsigned long>(stretchHr));
            fflush(stderr);
        }
#endif

        // ARGB Bitmap over the tightly-packed small buffer (alive across the
        // Save call inside encodeBitmap). No GDI+ DrawImage — the GPU already
        // did the resample.
        Gdiplus::Bitmap smallBmp(dstW, dstH, dstStride, PixelFormat32bppARGB,
                                 smallBuf.data());
        if (smallBmp.GetLastStatus() != Gdiplus::Ok) return false;
        return encodeBitmap(&smallBmp, "fast");
    };

    if (tryStretchPath()) return true;

    // ===== Slow path (fallback): the proven full readback + GDI+
    // downscale. Reached when the device lacks the StretchRect caps or the GPU
    // fast path hit any failure (so the modal still gets its backdrop).
    HRESULT hr = m_impl->device->GetRenderTargetData(
        m_impl->offscreenRT.Get(), m_impl->sysMemSurface.Get());
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT locked = {};
    hr = m_impl->sysMemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return false;

#ifndef NDEBUG
    {
        LARGE_INTEGER rT{};
        QueryPerformanceCounter(&rT);
        const double rMs = (rT.QuadPart - sT0.QuadPart) * 1000.0 /
                           static_cast<double>(sQpf.QuadPart);
        fprintf(stderr,
                "[CACHE-DEFERRAL-PERF] snapshotReadback=%.3f ms (slow %dx%d)\n",
                rMs, srcW, srcH);
        fflush(stderr);
    }
#endif

    const int stride = srcW * 4;

    // Copy SYSTEMMEM → a local buffer so the LockRect window is as short as
    // possible (we don't hold the lock across PNG encoding, which can be ms).
    std::vector<uint8_t> rawDib(static_cast<size_t>(stride) *
                                 static_cast<size_t>(srcH));
    {
        const auto* src = static_cast<const uint8_t*>(locked.pBits);
        for (int y = 0; y < srcH; ++y)
        {
            memcpy(rawDib.data() + static_cast<size_t>(y) *
                                    static_cast<size_t>(stride),
                   src + static_cast<size_t>(y) * locked.Pitch,
                   static_cast<size_t>(stride));
        }
    }
    m_impl->sysMemSurface->UnlockRect();

    // The DIB pixels are BGRA in memory (D3DFMT_A8R8G8B8 + BI_RGB). We use
    // ARGB for the GDI+ Bitmap so PNG encoding writes straight sRGB. scan0
    // points at the crop's top-left pixel and we keep the full source stride,
    // so GDI+ steps row-to-row at the same X offset inside the parent buffer.
    BYTE* scan0 = const_cast<BYTE*>(rawDib.data()) +
                  static_cast<size_t>(cropY) * static_cast<size_t>(stride) +
                  static_cast<size_t>(cropX) * 4u;
    Gdiplus::Bitmap srcBmp(cropW, cropH, stride, PixelFormat32bppARGB, scan0);
    if (srcBmp.GetLastStatus() != Gdiplus::Ok) return false;

    Gdiplus::Bitmap* encodeBmp = &srcBmp;
    std::unique_ptr<Gdiplus::Bitmap> downscaled;
    if (dstW != cropW || dstH != cropH)
    {
        downscaled = std::make_unique<Gdiplus::Bitmap>(dstW, dstH, PixelFormat32bppARGB);
        if (downscaled->GetLastStatus() != Gdiplus::Ok) return false;
        Gdiplus::Graphics g(downscaled.get());
        // Bilinear is plenty under the backdrop blur and avoids the multi-tap
        // prefilter cost of the high-quality modes on a large downscale.
        g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        if (g.DrawImage(&srcBmp, Gdiplus::Rect(0, 0, dstW, dstH),
                        0, 0, cropW, cropH, Gdiplus::UnitPixel) != Gdiplus::Ok)
            return false;
        encodeBmp = downscaled.get();
    }

    return encodeBitmap(encodeBmp, "slow");
}

bool AlphaCompositor::CaptureSnapshotToFile(const std::wstring& path)
{
    // Same one-shot readback + scene-rect crop as CaptureSnapshotPng,
    // but GDI+ saves straight to `path` instead of encoding to base64.
    // Kept as a separate method (rather than refactoring the shared
    // readback) so the proven modal-snapshot path stays untouched.
    if (!m_impl->offscreenRT || !m_impl->sysMemSurface) return false;
    if (m_impl->width <= 0 || m_impl->height <= 0)       return false;

    CLSID pngClsid = {};
    if (!host::GdiplusEncoderClsid(L"image/png", pngClsid)) return false;

    HRESULT hr = m_impl->device->GetRenderTargetData(
        m_impl->offscreenRT.Get(), m_impl->sysMemSurface.Get());
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT locked = {};
    hr = m_impl->sysMemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return false;

    const int srcW   = m_impl->width;
    const int srcH   = m_impl->height;
    const int stride = srcW * 4;

    std::vector<uint8_t> rawDib(static_cast<size_t>(stride) *
                                static_cast<size_t>(srcH));
    {
        const auto* src = static_cast<const uint8_t*>(locked.pBits);
        for (int y = 0; y < srcH; ++y)
        {
            memcpy(rawDib.data() + static_cast<size_t>(y) * static_cast<size_t>(stride),
                   src + static_cast<size_t>(y) * locked.Pitch,
                   static_cast<size_t>(stride));
        }
    }
    m_impl->sysMemSurface->UnlockRect();

    // Crop to scene rect if one is set; otherwise the full RT (the
    // typical case under --capture, since no React layout/scene-rect
    // dispatch runs — and the full engine RT is exactly what we want).
    int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
    if (m_impl->sceneW > 0 && m_impl->sceneH > 0)
    {
        cropX = (m_impl->sceneX < 0) ? 0 : m_impl->sceneX;
        cropY = (m_impl->sceneY < 0) ? 0 : m_impl->sceneY;
        const int maxW = srcW - cropX;
        const int maxH = srcH - cropY;
        cropW = (m_impl->sceneW < maxW) ? m_impl->sceneW : maxW;
        cropH = (m_impl->sceneH < maxH) ? m_impl->sceneH : maxH;
        if (cropW <= 0 || cropH <= 0) return false;
    }

    BYTE* scan0 = const_cast<BYTE*>(rawDib.data()) +
                  static_cast<size_t>(cropY) * static_cast<size_t>(stride) +
                  static_cast<size_t>(cropX) * 4u;
    Gdiplus::Bitmap bmp(cropW, cropH, stride, PixelFormat32bppARGB, scan0);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    return bmp.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
}

bool AlphaCompositor::CaptureSnapshotBgra(std::vector<unsigned char>& outBgra,
                                          int& outW, int& outH, int& outX, int& outY)
{
    // Engine LAYER of the headless --record composite: the SAME
    // GetRenderTargetData + LockRect + scene-rect crop the modal/--capture
    // snapshots use, but returning tight BGRA instead of a PNG. Kept as a
    // distinct method (mirroring CaptureSnapshotToFile) so the two proven
    // snapshot paths stay untouched.
    if (!m_impl->offscreenRT || !m_impl->sysMemSurface) return false;
    if (m_impl->width <= 0 || m_impl->height <= 0)       return false;

    const int srcW = m_impl->width;
    const int srcH = m_impl->height;

    // Crop to the scene rect if one is set; otherwise the full RT (the
    // host-boot default before React's first layout/scene-rect dispatch).
    // Clamp exactly as CaptureSnapshotToFile does so an over-large or negative
    // scene rect can't read out of bounds. Computed + allocated BEFORE the lock
    // so no throwing op runs while the surface is locked (would skip the unlock
    // → the surface leaks locked; pre-PR review finding 2).
    int cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
    if (m_impl->sceneW > 0 && m_impl->sceneH > 0)
    {
        cropX = (m_impl->sceneX < 0) ? 0 : m_impl->sceneX;
        cropY = (m_impl->sceneY < 0) ? 0 : m_impl->sceneY;
        const int maxW = srcW - cropX;
        const int maxH = srcH - cropY;
        cropW = (m_impl->sceneW < maxW) ? m_impl->sceneW : maxW;
        cropH = (m_impl->sceneH < maxH) ? m_impl->sceneH : maxH;
        if (cropW <= 0 || cropH <= 0) return false;
    }
    const size_t dstStride = static_cast<size_t>(cropW) * 4u;
    outBgra.resize(dstStride * static_cast<size_t>(cropH));

    HRESULT hr = m_impl->device->GetRenderTargetData(
        m_impl->offscreenRT.Get(), m_impl->sysMemSurface.Get());
    if (FAILED(hr)) return false;

    D3DLOCKED_RECT locked = {};
    hr = m_impl->sysMemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return false;

    // Copy the crop sub-rect into the tightly-packed buffer, top-down.
    // locked.Pitch is the source stride (positive for D3D9) and may exceed
    // srcW*4. No allocation happens between LockRect and UnlockRect.
    const auto* srcBase = static_cast<const uint8_t*>(locked.pBits);
    for (int y = 0; y < cropH; ++y)
    {
        const uint8_t* srcRow = srcBase
            + static_cast<size_t>(cropY + y) * static_cast<size_t>(locked.Pitch)
            + static_cast<size_t>(cropX) * 4u;
        memcpy(outBgra.data() + static_cast<size_t>(y) * dstStride, srcRow, dstStride);
    }
    m_impl->sysMemSurface->UnlockRect();

    outW = cropW;
    outH = cropH;
    outX = cropX;
    outY = cropY;
    return true;
}

} // namespace host
