// AlphaCompositor implementation. See AlphaCompositor.h for the
// high-level pipeline..

#include "AlphaCompositor.h"

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
#include <unordered_map>
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

struct Occlusion
{
    RECT rect;
    int  feather;
};

} // namespace

struct AlphaCompositor::Impl
{
    Microsoft::WRL::ComPtr<IDirect3DDevice9>  device;
    // Phase 3 Stage 2: shared-handle texture promoted from the
    // prior CreateRenderTarget surface. CreateTexture with USAGE_RENDER
    // TARGET + D3DPOOL_DEFAULT + a non-null pSharedHandle out-param is
    // the D3D9Ex idiom for cross-device shareable RTs (validated in the
    // dxgi_spike at commit 6c00536). sharedTex.GetSurfaceLevel(0) is
    // the same IDirect3DSurface9 the engine renders into via slot 0,
    // so the existing Render() chain is unchanged. sharedHandle is an
    // NT alias D3D11 can open via OpenSharedResource — Stage 4 wires
    // that side; Stage 2 just exposes the handle and verifies it.
    Microsoft::WRL::ComPtr<IDirect3DTexture9> sharedTex;
    HANDLE                                    sharedHandle = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> offscreenRT;   // sharedTex level-0, ARGB
    Microsoft::WRL::ComPtr<IDirect3DSurface9> sysMemSurface; // D3DPOOL_SYSTEMMEM, readback
    HDC      memDC      = nullptr;
    HBITMAP  dibBitmap  = nullptr;
    void*    dibPixels  = nullptr;
    int      width      = 0;
    int      height     = 0;

    std::unordered_map<std::string, Occlusion> occlusions;

    // Phase 3 follow-up: pre-stamp DIB cache for the
    // (canvas-JPEG transport) per-frame frame-server. Holds the engine-
    // rendered BGRA pixels captured each frame BEFORE the occlusion
    // stamps. EncodeFrameJpeg reads from here on the hot path.
    //
    // CaptureSnapshotPng used to read from here too, but it now does
    // its own on-demand GetRenderTargetData + LockRect to avoid paying
    // the ~19 MB memcpy on every frame in arch B (WS_EX_LAYERED),
    // where modal opens are the only consumer.
    //
    // The cache is OFF by default. FramePublisher's constructor flips
    // it on via SetPerFrameCacheEnabled. Reallocated lazily inside
    // Composite when the cached dims don't match the current DIB.
    bool     perFrameCacheEnabled = false;
    std::vector<uint8_t> lastRawDib;
    int      lastRawW    = 0;
    int      lastRawH    = 0;

    // B1.4 T4c: the scene rect inside the popup (popup-client
    // coords). Composite stamps alpha=0 for the four bands outside
    // this rect, so panels behind the popup's alpha-zero regions show
    // through and (because the popup is WS_EX_LAYERED with
    // UpdateLayeredWindow(ULW_ALPHA)) receive their own mouse events.
    // Default (0/0/0/0) disables the mask — used during host boot
    // before React dispatches the first layout/scene-rect, so the
    // popup keeps the pre-T4c "full popup = scene" semantics.
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

AlphaCompositor::~AlphaCompositor()
{
    if (m_impl->dibBitmap) DeleteObject(m_impl->dibBitmap);
    if (m_impl->memDC)     DeleteDC(m_impl->memDC);
}

void AlphaCompositor::Resize(int w, int h)
{
    if (w == m_impl->width && h == m_impl->height) return;
    if (w <= 0 || h <= 0) return;

    // [G7] Transactional rebuild. Build every new resource into
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
    HBITMAP newDib    = nullptr;
    void*   newPixels = nullptr;
    HDC     newDC     = nullptr;

    try
    {
        // Phase 3 Stage 2: shared-handle render-target texture.
        // CreateTexture with USAGE_RENDERTARGET + D3DPOOL_DEFAULT and a
        // non-null pSharedHandle yields an NT-handle alias openable from a
        // parallel D3D11 device via OpenSharedResource. The level-0 surface
        // serves as the engine's render target slot 0, so the existing
        // arch-A render+readback path is structurally unchanged. The
        // sharedHandle out-param is populated only when the device is
        // D3D9Ex (Stage 1 hard-fails otherwise, so this is always the case
        // post-Stage-1). D3DMULTISAMPLE_NONE because GetRenderTargetData
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

        // Top-down DIB (negative biHeight) so row 0 matches D3D9's row 0.
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = w;
        bi.bmiHeader.biHeight      = -h;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC screenDC = GetDC(nullptr);
        newDib = CreateDIBSection(screenDC, &bi, DIB_RGB_COLORS,
                                  &newPixels, nullptr, 0);
        ReleaseDC(nullptr, screenDC);
        if (!newDib || !newPixels)
            throw std::runtime_error("AlphaCompositor: CreateDIBSection failed");

        newDC = CreateCompatibleDC(nullptr);
        if (!newDC) throw std::runtime_error("AlphaCompositor: CreateCompatibleDC failed");
        SelectObject(newDC, newDib);
    }
    catch (...)
    {
        // Roll back the GDI locals (ComPtr locals auto-release during unwind).
        // DeleteDC first so the DIB is deselected before DeleteObject — a
        // bitmap still selected into a DC can't be deleted. m_impl is left
        // entirely untouched: the old resources stay live and valid.
        if (newDC)  DeleteDC(newDC);
        if (newDib) DeleteObject(newDib);
        throw;
    }

    // All allocations succeeded — commit. Release the old resources, then
    // move the locals in. ComPtr::Reset releases the COM ref; GDI handles
    // need explicit cleanup. The old sharedHandle is owned by the old
    // sharedTex — releasing the texture invalidates it, no explicit
    // CloseHandle.
    m_impl->offscreenRT.Reset();
    m_impl->sharedTex.Reset();
    m_impl->sharedHandle = nullptr;
    m_impl->sysMemSurface.Reset();
    if (m_impl->dibBitmap) { DeleteObject(m_impl->dibBitmap); m_impl->dibBitmap = nullptr; }
    if (m_impl->memDC)     { DeleteDC(m_impl->memDC);         m_impl->memDC     = nullptr; }

    m_impl->sharedTex     = std::move(newTex);
    m_impl->sharedHandle  = newHandle;
    m_impl->offscreenRT   = std::move(newRT);
    m_impl->sysMemSurface = std::move(newSys);
    m_impl->dibBitmap     = newDib;
    m_impl->dibPixels     = newPixels;
    m_impl->memDC         = newDC;
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
    if (m_impl->dibBitmap) { DeleteObject(m_impl->dibBitmap); m_impl->dibBitmap = nullptr; }
    if (m_impl->memDC)     { DeleteDC(m_impl->memDC);         m_impl->memDC     = nullptr; }
    m_impl->dibPixels = nullptr;
    // Clear cached size so the next Resize doesn't short-circuit on
    // "(w,h) unchanged" — width and height stay 0 until the next
    // successful allocation.
    m_impl->width  = 0;
    m_impl->height = 0;
}

IDirect3DSurface9* AlphaCompositor::GetRenderTarget() const { return m_impl->offscreenRT.Get(); }

HANDLE AlphaCompositor::GetSharedHandle() const { return m_impl->sharedHandle; }
void AlphaCompositor::SetOcclusion(std::string id, RECT rectClient, int feather)
{
    if (feather < 0) feather = 0;
    m_impl->occlusions[std::move(id)] = Occlusion{ rectClient, feather };
}

void AlphaCompositor::RemoveOcclusion(const std::string& id)
{
    m_impl->occlusions.erase(id);
}

void AlphaCompositor::SetSceneRect(int x, int y, int w, int h)
{
    m_impl->sceneX = x;
    m_impl->sceneY = y;
    m_impl->sceneW = w;
    m_impl->sceneH = h;
}

void AlphaCompositor::SetPerFrameCacheEnabled(bool enabled)
{
    if (m_impl->perFrameCacheEnabled == enabled) return;
    m_impl->perFrameCacheEnabled = enabled;
    if (!enabled)
    {
        // Release the ~19 MB on disable so arch toggles don't leak
        // and so a later CaptureSnapshotPng can't accidentally read a
        // stale pre-disable frame (it shouldn't — it now does its own
        // readback — but the contract is "off means empty").
        std::vector<uint8_t>().swap(m_impl->lastRawDib);
        m_impl->lastRawW = 0;
        m_impl->lastRawH = 0;
    }
}

namespace {

// smoothstep(0, w, x) — classic cubic, clamped.
inline float smoothstep01(float x)
{
    if (x <= 0.0f) return 0.0f;
    if (x >= 1.0f) return 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

// Apply a single occlusion to the DIB. Pixels strictly inside the
// rect, beyond the `feather` inset, are zeroed (RGBA = 0). Pixels
// in the `feather` band along the inner edge get RGBA *= weight,
// where weight ramps from 0 (deepest inside) to 1 (at the rect's
// outer edge), so alpha falls off smoothly and the premultiplied
// RGB matches the new alpha. Pixels outside the rect are untouched.
void ApplyOcclusion(uint8_t* dib, int dibW, int dibH,
                    const RECT& rect, int feather)
{
    // Clip the iteration range to DIB bounds, but compute the
    // feather distance from the ORIGINAL rect edges. When the rect
    // extends past the popup (e.g. a menu whose top is above the
    // viewport), the distance from popup-edge pixels to the
    // original rect edge is already > feather, so weight=0 falls
    // out naturally — no purple halo at the popup edge. When the
    // rect is fully inside, distance == clipped distance, so this
    // also matches the inside-popup case.
    const int x0 = (std::max)(static_cast<int>(rect.left),   0);
    const int y0 = (std::max)(static_cast<int>(rect.top),    0);
    const int x1 = (std::min)(static_cast<int>(rect.right),  dibW);
    const int y1 = (std::min)(static_cast<int>(rect.bottom), dibH);
    if (x1 <= x0 || y1 <= y0) return;

    const int rectLeft   = static_cast<int>(rect.left);
    const int rectTop    = static_cast<int>(rect.top);
    const int rectRight  = static_cast<int>(rect.right);
    const int rectBottom = static_cast<int>(rect.bottom);

    const int rowBytes = dibW * 4;

    for (int y = y0; y < y1; ++y)
    {
        const int dyTop = y - rectTop;             // ≥ 0 inside, larger near popup edge
        const int dyBot = (rectBottom - 1) - y;
        const int dy    = (dyTop < dyBot) ? dyTop : dyBot;

        uint8_t* row = dib + y * rowBytes;
        for (int x = x0; x < x1; ++x)
        {
            const int dxLeft  = x - rectLeft;
            const int dxRight = (rectRight - 1) - x;
            const int dx      = (dxLeft < dxRight) ? dxLeft : dxRight;

            // Chebyshev distance to the rect's nearest outer edge.
            const int d = (dx < dy) ? dx : dy;

            // d=0 at the rect's outer edge → keep pixel mostly opaque.
            // d=feather → fully cut (weight 0). Pixels beyond the
            // feather band (deeper inside the rect, or in a region
            // where the nearest rect edge sits outside the popup) get
            // weight=0 → full alpha cut.
            float weight = 0.0f;
            if (feather > 0 && d < feather)
            {
                weight = 1.0f - smoothstep01(static_cast<float>(d) / static_cast<float>(feather));
            }

            uint8_t* px = row + x * 4;
            if (weight <= 0.0f)
            {
                px[0] = px[1] = px[2] = px[3] = 0;
            }
            else
            {
                // Multiply RGB and A by `weight` to preserve the
                // premultiplied-alpha invariant ULW_ALPHA requires.
                px[0] = static_cast<uint8_t>(px[0] * weight);
                px[1] = static_cast<uint8_t>(px[1] * weight);
                px[2] = static_cast<uint8_t>(px[2] * weight);
                px[3] = static_cast<uint8_t>(px[3] * weight);
            }
        }
    }
}

// T4c band-stamp helper. Zero the alpha channel (and premultiplied
// RGB) of a hard-edged rectangle inside the DIB. Used for the four
// outside-scene bands — no smoothstep, no per-pixel weight; just
// memset to 0. Cheaper than four ApplyOcclusion(feather=0) calls
// because the inner loops collapse to memset per row.
void StampHardZeroBand(uint8_t* dib, int dibW, int dibH,
                       int x0, int y0, int x1, int y1)
{
    if (x0 < 0)    x0 = 0;
    if (y0 < 0)    y0 = 0;
    if (x1 > dibW) x1 = dibW;
    if (y1 > dibH) y1 = dibH;
    if (x1 <= x0 || y1 <= y0) return;
    const int rowBytes = dibW * 4;
    const int spanBytes = (x1 - x0) * 4;
    for (int y = y0; y < y1; ++y)
    {
        uint8_t* row = dib + y * rowBytes + x0 * 4;
        memset(row, 0, static_cast<size_t>(spanBytes));
    }
}

// B1.3.1.1: find the PNG encoder CLSID via Gdiplus::GetImageEncoders.
// The encoder list is enumerated once on first use; the CLSID is then
// cached statically. Returns false (and leaves outClsid untouched) if
// the PNG encoder isn't available — should never happen on a system
// with GDI+ initialized, but we fail soft anyway.
bool GetPngEncoderClsid(CLSID& outClsid)
{
    static CLSID cached = {};
    static bool  found  = false;
    if (found) { outClsid = cached; return true; }

    UINT numEncoders = 0;
    UINT bytes       = 0;
    if (Gdiplus::GetImageEncodersSize(&numEncoders, &bytes) != Gdiplus::Ok || bytes == 0)
        return false;

    std::vector<uint8_t> buf(bytes);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(numEncoders, bytes, info) != Gdiplus::Ok)
        return false;

    for (UINT i = 0; i < numEncoders; ++i)
    {
        if (wcscmp(info[i].MimeType, L"image/png") == 0)
        {
            cached  = info[i].Clsid;
            outClsid = cached;
            found   = true;
            return true;
        }
    }
    return false;
}

// B1.3.1.1: standard base64 encoder. 30 lines, no dep. Encodes the
// PNG bytes the GDI+ encoder produced into the ASCII string the
// React side reads via `data:image/png;base64,<payload>`.
std::string Base64Encode(const uint8_t* data, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 2 < len; i += 3)
    {
        const uint32_t v = (uint32_t(data[i]) << 16) |
                           (uint32_t(data[i + 1]) << 8) |
                           (uint32_t(data[i + 2]));
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back(alphabet[(v >>  6) & 0x3F]);
        out.push_back(alphabet[ v        & 0x3F]);
    }
    if (i < len)
    {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        out.push_back(alphabet[(v >> 18) & 0x3F]);
        out.push_back(alphabet[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? alphabet[(v >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace

// Phase 0 spike. JPEG encoder CLSID lookup — same shape as
// GetPngEncoderClsid above, just walks the encoder list for image/jpeg.
namespace {
bool GetJpegEncoderClsid(CLSID& outClsid)
{
    static CLSID cached = {};
    static bool  found  = false;
    if (found) { outClsid = cached; return true; }

    UINT numEncoders = 0;
    UINT bytes       = 0;
    if (Gdiplus::GetImageEncodersSize(&numEncoders, &bytes) != Gdiplus::Ok || bytes == 0)
        return false;

    std::vector<uint8_t> buf(bytes);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(numEncoders, bytes, info) != Gdiplus::Ok)
        return false;

    for (UINT i = 0; i < numEncoders; ++i)
    {
        if (wcscmp(info[i].MimeType, L"image/jpeg") == 0)
        {
            cached  = info[i].Clsid;
            outClsid = cached;
            found   = true;
            return true;
        }
    }
    return false;
}
} // namespace

bool AlphaCompositor::EncodeFrameJpeg(int quality,
                                      std::vector<uint8_t>& outBytes,
                                      int& outW, int& outH)
{
    if (m_impl->lastRawDib.empty() || m_impl->lastRawW <= 0 || m_impl->lastRawH <= 0)
        return false;

    CLSID jpegClsid = {};
    if (!GetJpegEncoderClsid(jpegClsid)) return false;

    const int srcW   = m_impl->lastRawW;
    const int srcH   = m_impl->lastRawH;
    const int stride = srcW * 4;

    // Crop to scene rect if set — same shape as CaptureSnapshotPng.
    // The popup is full-main-row sized post-B1.4 T4c, but only the
    // scene rect holds pixels the user expects to see in the centre
    // quadrant. Stretching the full DIB onto the canvas would paint
    // the offstage engine content under the side panels too.
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

    BYTE* scan0 = const_cast<BYTE*>(m_impl->lastRawDib.data()) +
                  static_cast<size_t>(cropY) * static_cast<size_t>(stride) +
                  static_cast<size_t>(cropX) * 4u;
    Gdiplus::Bitmap bmp(cropW, cropH, stride, PixelFormat32bppARGB, scan0);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    // Build a quality EncoderParameter (JPEG-specific). EncoderQuality
    // is the canonical GUID; value is a LONG in 0-100.
    if (quality < 1)   quality = 1;
    if (quality > 100) quality = 100;
    ULONG qval = static_cast<ULONG>(quality);
    Gdiplus::EncoderParameters params;
    params.Count = 1;
    params.Parameter[0].Guid           = Gdiplus::EncoderQuality;
    params.Parameter[0].Type           = Gdiplus::EncoderParameterValueTypeLong;
    params.Parameter[0].NumberOfValues = 1;
    params.Parameter[0].Value          = &qval;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || !stream)
        return false;

    if (bmp.Save(stream, &jpegClsid, &params) != Gdiplus::Ok)
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
    const size_t jpegBytes = static_cast<size_t>(stat.cbSize.QuadPart);
    outBytes.resize(jpegBytes);
    ULONG read = 0;
    if (FAILED(stream->Read(outBytes.data(), static_cast<ULONG>(jpegBytes), &read)) ||
        read != jpegBytes)
    {
        stream->Release();
        outBytes.clear();
        return false;
    }
    stream->Release();

    outW = cropW;
    outH = cropH;
    return true;
}

bool AlphaCompositor::CaptureSnapshotPng(std::string& outBase64, int& outW, int& outH)
{
    // Phase 3 follow-up: self-sufficient readback. The cache
    // path was the bottleneck in arch B (layered popup) — paying
    // a 19 MB memcpy every frame to keep a snapshot fresh for modals
    // that fire seconds-to-minutes apart. We now do a one-shot readback
    // at snapshot time (~12-15 ms of one-time cost, imperceptible vs.
    // the ~50-100 ms dialog mount + React reflow that triggers us).
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
    // Safety: offscreenRT holds the engine's pre-stamp pixels —
    // stamps in Composite() mutate dibPixels only, never the GPU
    // render target. So between Engine::Render calls, offscreenRT is
    // always re-readable. During the Win32 modal sizing loop (/
    // cb7b4c7), Engine::Render also doesn't run, so offscreenRT holds
    // the pre-resize frame — same observable behavior as the prior
    // cached path.
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
    if (!GetJpegEncoderClsid(jpegClsid)) return false;
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

    // B1.4 T4c.5: crop region = the current scene rect (the only sub-region
    // that holds pixels the user sees; encoding the full DIB would stretch
    // outside-scene engine content into the modal's quadrant <img>). When no
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

    // instant-modal] Encoded (downscaled) output dims. The snapshot is
    // only ever shown as a modal's frosted-glass backdrop — Dialog.Overlay
    // paints bg-black/60 + backdrop-blur-sm over it, so a full-res encode is
    // wasted work. Two knobs cut it: kSnapshotMaxEdge caps the long edge
    // (bounding the upscale/softness under the blur — ~3.4x at 3440 -> 1024);
    // kSnapshotDownscale forces a min reduction even for sub-cap (windowed)
    // captures. This formula is reused VERBATIM by both paths so the encoded
    // size is byte-identical to the pre-output (the native dim tests and
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

        // JPEG quality EncoderParameter (LONG 1..100), same shape as
        // EncodeFrameJpeg. qval must outlive the Save call.
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

        outBase64 = Base64Encode(img.data(), img.size());
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
        // and is never presented in, so this is side-effect-free, and
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

    // ===== Slow path (fallback): the proven pre-full readback + GDI+
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
    if (!GetPngEncoderClsid(pngClsid)) return false;

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

void AlphaCompositor::Composite(HWND layeredHwnd)
{
    if (!layeredHwnd) return;
    if (!m_impl->offscreenRT || !m_impl->sysMemSurface || !m_impl->dibBitmap) return;
    if (m_impl->width <= 0 || m_impl->height <= 0) return;

    // GPU → SYSTEMMEM. The actual readback cost (~1-3 ms on modern
    // hardware for a typical viewport size).
    HRESULT hr = m_impl->device->GetRenderTargetData(
        m_impl->offscreenRT.Get(), m_impl->sysMemSurface.Get());
    if (FAILED(hr)) return;

    D3DLOCKED_RECT locked = {};
    hr = m_impl->sysMemSurface->LockRect(&locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) return;

    // D3DFMT_A8R8G8B8 stores pixels as BB GG RR AA in memory; GDI
    // BI_RGB 32bpp shares that byte order. Direct memcpy works,
    // accounting for the source pitch (locked.Pitch >= w * 4).
    auto*       dst      = static_cast<uint8_t*>(m_impl->dibPixels);
    const auto* src      = static_cast<const uint8_t*>(locked.pBits);
    const int   rowBytes = m_impl->width * 4;
    for (int y = 0; y < m_impl->height; ++y)
    {
        memcpy(dst + y * rowBytes, src + y * locked.Pitch, rowBytes);
    }

    m_impl->sysMemSurface->UnlockRect();

    // Phase 3 follow-up: cache the pre-stamp engine pixels
    // ONLY when the frame-server is active (FramePublisher
    // constructor flips perFrameCacheEnabled). Arch B (layered
    // popup) skips this — modal-open snapshots now do their own
    // on-demand readback in CaptureSnapshotPng, so this ~19 MB
    // memcpy is pure waste in that path. Reclaims ~2-5 ms/frame at
    // 3440×1440.
    //
    // When active, this must run AFTER the readback memcpy but
    // BEFORE the occlusion stamps — the cache is the engine's raw
    // output, so JPEG-encoded frames in arch C don't include the
    // chrome cut-outs (which only the layered-popup arch B needs).
#ifndef NDEBUG
    // [CACHE-DEFERRAL-PERF] per-frame phase timing. Grep this prefix
    // when verifying the perf hypothesis at maximized 3440×1440.
    // Throttled to ~1 Hz so the printf cost doesn't itself dominate
    // the measurement.
    LARGE_INTEGER cdpQpf{}, cdpT0{}, cdpT1{};
    QueryPerformanceFrequency(&cdpQpf);
    QueryPerformanceCounter(&cdpT0);
#endif
    if (m_impl->perFrameCacheEnabled)
    {
        const size_t bytes = static_cast<size_t>(m_impl->width) *
                             static_cast<size_t>(m_impl->height) * 4u;
        if (m_impl->lastRawDib.size() != bytes)
        {
            m_impl->lastRawDib.resize(bytes);
        }
        memcpy(m_impl->lastRawDib.data(), dst, bytes);
        m_impl->lastRawW = m_impl->width;
        m_impl->lastRawH = m_impl->height;
    }
#ifndef NDEBUG
    QueryPerformanceCounter(&cdpT1);
    static DWORD s_cdpLastLogTick = 0;
    const DWORD  cdpNowTick       = GetTickCount();
    if (cdpNowTick - s_cdpLastLogTick > 1000)
    {
        s_cdpLastLogTick = cdpNowTick;
        const double cdpMs = (cdpT1.QuadPart - cdpT0.QuadPart) * 1000.0 /
                             static_cast<double>(cdpQpf.QuadPart);
        fprintf(stderr,
                "[CACHE-DEFERRAL-PERF] cacheCopy=%.3f ms (enabled=%d, %dx%d)\n",
                cdpMs, m_impl->perFrameCacheEnabled ? 1 : 0,
                m_impl->width, m_impl->height);
        fflush(stderr);
    }
#endif

    // B1.4 T4c: stamp alpha=0 for the four bands OUTSIDE the scene
    // rect. The popup HWND occupies the entire main-row area; only
    // the scene rect should show rendered scene pixels. The bands
    // become transparent for both compositing (rendered pixels behind
    // the alpha=0 area show WebView2 chrome) AND for hit-testing
    // (WS_EX_LAYERED + ULW_ALPHA → alpha-zero pixels pass clicks
    // through to the WebView2 underneath, so panels receive their own
    // mouse events).
    //
    // Runs AFTER the lastRawDib cache (so snapshots still hold the
    // raw engine pixels — T4c.5 crops the cached DIB to the scene
    // rect before PNG encoding) and BEFORE the per-id smoothstep
    // occlusion pass (so tool-panel cutouts inside the scene rect
    // can layer on top with their soft feather edges).
    //
    // sceneW/sceneH = 0 means "no mask" — used during host boot
    // before the first layout/scene-rect dispatch arrives.
    if (m_impl->sceneW > 0 && m_impl->sceneH > 0)
    {
        const int sx = m_impl->sceneX;
        const int sy = m_impl->sceneY;
        const int sR = sx + m_impl->sceneW;
        const int sB = sy + m_impl->sceneH;
        const int W  = m_impl->width;
        const int H  = m_impl->height;
        // Top band:    (0,0) → (W, sy)
        StampHardZeroBand(dst, W, H, 0, 0, W, sy);
        // Bottom band: (0, sB) → (W, H)
        StampHardZeroBand(dst, W, H, 0, sB, W, H);
        // Left band:   (0, sy) → (sx, sB)
        StampHardZeroBand(dst, W, H, 0, sy, sx, sB);
        // Right band:  (sR, sy) → (W, sB)
        StampHardZeroBand(dst, W, H, sR, sy, W, sB);
    }

    // Stamp the alpha (and premultiplied RGB) for each occluded
    // chrome rect. Outside occlusions the DIB keeps the engine's
    // fully-opaque pixels.
    for (const auto& kv : m_impl->occlusions)
    {
        ApplyOcclusion(dst, m_impl->width, m_impl->height,
                       kv.second.rect, kv.second.feather);
    }

    POINT srcPoint = { 0, 0 };
    SIZE  bmpSize  = { m_impl->width, m_impl->height };
    BLENDFUNCTION blend = {};
    blend.BlendOp             = AC_SRC_OVER;
    blend.BlendFlags          = 0;
    blend.SourceConstantAlpha = 0xFF;
    blend.AlphaFormat         = AC_SRC_ALPHA;

    // pptDst = nullptr → screen position unchanged (LayoutBroker has
    // already SetWindowPos'd the viewport popup).
    UpdateLayeredWindow(layeredHwnd, nullptr /*hdcDst*/,
                        nullptr /*pptDst*/, &bmpSize,
                        m_impl->memDC, &srcPoint,
                        0 /*crKey*/, &blend, ULW_ALPHA);
}

} // namespace host
