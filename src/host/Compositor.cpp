// Compositor — see Compositor.h for the design overview.
//
// Most of this file is a port of src/host/spike/dxgi_spike.cpp's
// InitDComp + BuildVisualTree + Shutdown sections (the working
// Stage 0 spike on user's RTX 3080). The spike topology is
// preserved exactly; only the wrapping changes (LogFn callback
// instead of LogDbg, ComPtr members on a pImpl struct, idempotency
// guards).
//
// IMPORTANT — translation-unit isolation. This .cpp is the ONLY
// place dcomp.h / d3d11.h / dxgi1_2.h are included in the
// ParticleEditor binary. ParticleEditor.vcxproj's project-level
// AdditionalIncludeDirectories puts $(DXSDK_DIR)Include first so
// the engine can find DXSDK June 2010's d3dx9.h, but DXSDK also
// has stale DXGI.h / D3D11.h / Dcommon.h that predate Direct2D 1.1
// and DirectComposition. If those are included BEFORE the Win10
// SDK versions, the modern types (D2D_VECTOR_2F,
// DXGI_COLOR_SPACE_TYPE, etc.) come up undeclared and dcomp.h hits
// syntax errors. The per-file <AdditionalIncludeDirectories> on
// this Compositor.cpp entry in the vcxproj REPLACES (not appends
// to) the project default with a Win10-SDK-only path, so dxgi.h /
// d3d11.h / dcommon.h / d2d1_1.h all resolve to the modern Win10
// SDK versions. dcomp.h then has the types it needs.
//
// Consumers of Compositor.h (HostWindow.cpp in Stage 3b) don't pay
// this cost — Compositor.h's pImpl hides every DComp type behind
// `struct Impl`, so HostWindow.cpp's translation unit never sees
// dcomp.h. HostWindow.cpp keeps the project's DXSDK-first include
// path unchanged and the engine still finds d3dx9.h normally.

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>

// Include order mirrors dxgi_spike.cpp. dcomp.h transitively
// references DXGI types (DXGI_COLOR_SPACE_TYPE, IDXGI* interfaces)
// that must be declared first via <d3d11.h> + <dxgi1_2.h>.
#include <d3d11.h>
#include <dxgi1_2.h>

// D2D_USE_C_DEFINITIONS — opt out of d2d1's inline C++ helper
// classes (D2D1::Matrix3x2F, Matrix4x3F, Matrix4x4F, etc.).
// dcomp.h transitively pulls in <d2d1_1helper.h>, whose
// helper-class constructors reference struct member names like
// _11/_12/_13. We don't use the C++ helpers — only the plain C
// struct types like D2D_RECT_F for SetClip — so skipping them
// avoids any helper-related parse work that depends on Win10 SDK
// header layout being clean.
#ifndef D2D_USE_C_DEFINITIONS
#define D2D_USE_C_DEFINITIONS
#endif

#include <dcomp.h>

#include "WebView2.h"

#include "Compositor.h"

#pragma comment(lib, "dcomp.lib")
// Phase 3 Stage 4b — D3D11 device + DXGI factory + composition
// swapchain. d3d11.lib for D3D11CreateDevice; dxgi.lib for
// CreateDXGIFactory2. dcomp.lib already covers the existing visual
// tree work. All three libs ship with the Win10 SDK and are picked
// up by Compositor.cpp's per-file include override.
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include <cstdio>

namespace host {

namespace {

// Format an HRESULT into a string for logging. Matches the format
// spike uses ([SPIKE-ERROR] ... hr=0x%08lX).
std::string FormatHresult(HRESULT hr)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

} // namespace

// pImpl — keeps every DComp / WebView2 ComPtr off the public header
// surface so consumers (HostWindow.cpp) don't have to pull in
// dcomp.h. Also keeps the dxgi.h / d2d1.h transitive includes
// scoped to this single translation unit.
struct Compositor::Impl
{
    HWND  hwnd;
    Compositor::LogFn log;

    Microsoft::WRL::ComPtr<IDCompositionDevice>  device;
    Microsoft::WRL::ComPtr<IDCompositionTarget>  target;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  rootVisual;
    Microsoft::WRL::ComPtr<IDCompositionVisual>  webviewVisual;

    // The WebView2 composition controller that owns the
    // RootVisualTarget binding. Retained so the dtor can clear the
    // binding (put_RootVisualTarget(nullptr)) before releasing the
    // visual; without this the WebView2 internal reference to
    // webviewVisual would dangle. Set by AttachWebView2.
    Microsoft::WRL::ComPtr<ICoreWebView2CompositionController> controller;

    int  lastW = 0;
    int  lastH = 0;
    bool treeBuilt = false;

    // Phase 3 Stage 4 — engine D3D11 / DXGI bridge + visual.
    // ComPtr destruction order on ~Impl: engineVisual (releases
    // SetContent(swapchain) ref) → sharedTexD3D11 (D3D11 alias on the
    // engine-owned D3D9 VRAM; alias release does NOT free the
    // underlying texture, which AlphaCompositor owns) → engineBackBuffer
    // → engineSwapChain → dxgiFactory → d3d11Context → d3d11Device.
    // AlphaCompositor + Engine teardown later (HostWindow.cpp WM_DESTROY
    // order) releases the D3D9 side. See Compositor.h's engine-visual
    // block + sub-plan §3.6 for lifecycle.
    Microsoft::WRL::ComPtr<ID3D11Device>          d3d11Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>   d3d11Context;
    Microsoft::WRL::ComPtr<IDXGIFactory2>         dxgiFactory;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>       engineSwapChain;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>       engineBackBuffer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>       sharedTexD3D11;
    Microsoft::WRL::ComPtr<IDCompositionVisual>   engineVisual;

    // Cache + flag for AttachEngineVisual idempotence + 4c lazy
    // handle/size detection in CompositeEngineFrame.
    HANDLE engineHandleCached   = nullptr;
    int    engineWidthCached    = 0;
    int    engineHeightCached   = 0;
    bool   engineVisualAttached = false;

    // Phase 3 Stage 5 — last applied scene-rect transform on
    // the engine visual. Idempotence on SetEngineVisualTransform: skip
    // the SetOffset/SetClip/Commit call sequence when the new args
    // match the cache. (0,0,0,0) means "no transform applied yet" —
    // distinguishable from any real scene-rect because the args fail
    // the W>0 && H>0 idempotence check.
    int engineLastX          = 0;
    int engineLastY          = 0;
    int engineLastTransformW = 0;
    int engineLastTransformH = 0;

    // Phase 3 Stage 5 (T6 follow-up) — pending transform queue.
    // SetEngineVisualTransform with default (immediate=false) writes
    // the new args here and returns; CompositeEngineFrame applies the
    // pending transform at the END of the frame, after Present1 has
    // pushed fresh swapchain pixels for the new viewport. The next
    // DWM composition cycle sees both the new pixels and the new clip
    // simultaneously — no transient "engine clear color at the new
    // clip edge" artifact during pane-drag resize storms.
    // [resize-perf C2] pendingQuiet carries the caller's quiet flag
    // (per-frame anim applies skip the transform log) across the
    // deferral.
    bool pendingTransform  = false;
    bool pendingQuiet      = false;
    int  pendingX          = 0;
    int  pendingY          = 0;
    int  pendingW          = 0;
    int  pendingH          = 0;

    // Phase 3 Stage 4c — 1 Hz throttled diagnostics for
    // [COMP-engine-frame] + [COMP-engine-handle-hash]. Per-frame
    // emission would dominate the log; per-second is enough to spot
    // a stalled composite (count stops) or a swapped texture
    // (sharedTex pointer changes mid-run).
    DWORD    engineLastFrameLogTick = 0;
    uint64_t engineFrameCount       = 0;

    // (session 3) — theme-coloured composition backing. A 1x1
    // composition swapchain on its OWN D3D11 device (kept independent of
    // the engine's device so the engine's LUID guard + shared-texture
    // path stay byte-for-byte unchanged), scaled to the full client via
    // the visual transform and inserted as the REARMOST child of the root
    // visual (behind the engine visual). Recoloured by SetBackingColor on
    // each theme change so transparent DOM gaps / splitter seams /
    // rounded-corner wedges composite over the app-shell `--bg` instead of
    // the black host backing. A solid 1x1 means the transform's scaling is
    // colour-uniform (no sampling artifacts) and resize never reallocates
    // buffers (just updates the scale) — smooth during resize storms.
    Microsoft::WRL::ComPtr<ID3D11Device>        backingDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> backingContext;
    Microsoft::WRL::ComPtr<IDXGIFactory2>       backingFactory;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>     backingSwapChain;
    Microsoft::WRL::ComPtr<IDCompositionVisual> backingVisual;
    COLORREF backingColor      = 0;
    bool     backingColorValid = false;   // a colour has been pushed at least once
    bool     backingInTree     = false;   // backingVisual added to rootVisual

    // Backing helpers (defined after the struct, like ApplyTransform).
    HRESULT EnsureBackingDevice();   // own D3D11 device + DXGI factory
    HRESULT EnsureBackingVisual();   // 1x1 swapchain + DComp visual, inserted rearmost
    HRESULT PaintBacking(COLORREF color);  // clear the 1x1 backbuffer + Present
    void    InsertBackingRearmost();       // (re)prepend the backing as child 0
    void    ApplyBackingTransform();       // scale 1x1 → full client (no Commit)

    void LogLine(const std::string& s) const
    {
        if (log) log(s);
    }

    // Phase 3 Stage 5 T6 follow-up — apply a scene-rect
    // transform immediately. Shared by SetEngineVisualTransform's
    // immediate path and CompositeEngineFrame's pending-apply tail.
    // Caller validates engineVisualAttached + engineVisual + device
    // exist and (w, h) > 0. [resize-perf C2] quiet=true skips the
    // [COMP-engine-transform] log line — used for per-frame anim
    // applies (dock slide / scene-rect chase, which would otherwise
    // log + fflush at the render rate); instant and anim-TERMINAL
    // applies log as before, so the settled state is always visible
    // in host.log and the rate self-throttles by construction.
    HRESULT ApplyTransform(int x, int y, int w, int h, bool quiet);
};

HRESULT Compositor::Impl::ApplyTransform(int x, int y, int w, int h, bool quiet)
{
    HRESULT hr;

    // SetOffset to (0, 0) — visual's local coord space equals parent's
    // (the root visual at host-client coords). Explicit even though
    // (0, 0) is the default, because a previously-stale offset from
    // a pre-fix build would otherwise survive (visible to debuggers /
    // smoke after upgrade-in-place builds).
    hr = engineVisual->SetOffsetX(0.0f);
    if (FAILED(hr))
    {
        LogLine("[COMP-engine-fail] ApplyTransform SetOffsetX hr=" + FormatHresult(hr));
        return hr;
    }
    hr = engineVisual->SetOffsetY(0.0f);
    if (FAILED(hr))
    {
        LogLine("[COMP-engine-fail] ApplyTransform SetOffsetY hr=" + FormatHresult(hr));
        return hr;
    }

    // SetClip in local coord space (which equals parent / host-client
    // coords because the visual has zero offset). The clip is the
    // scene-rect rectangle in absolute host-client coords:
    // (left=x, top=y, right=x+w, bottom=y+h).
    D2D_RECT_F clip = {
        static_cast<float>(x),
        static_cast<float>(y),
        static_cast<float>(x + w),
        static_cast<float>(y + h)
    };
    hr = engineVisual->SetClip(clip);
    if (FAILED(hr))
    {
        LogLine("[COMP-engine-fail] ApplyTransform SetClip hr=" + FormatHresult(hr));
        return hr;
    }

    hr = device->Commit();
    if (FAILED(hr))
    {
        LogLine("[COMP-engine-fail] ApplyTransform Commit hr=" + FormatHresult(hr));
        return hr;
    }

    engineLastX          = x;
    engineLastY          = y;
    engineLastTransformW = w;
    engineLastTransformH = h;

    if (!quiet)
    {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "[COMP-engine-transform] clip=(%d,%d,%d,%d) (absolute host-client)",
                 x, y, x + w, y + h);
        LogLine(buf);
    }
    return S_OK;
}

// ---------- (session 3) — theme-coloured backing helpers ----------

HRESULT Compositor::Impl::EnsureBackingDevice()
{
    if (backingDevice && backingFactory) return S_OK;

    HRESULT hr;
    if (!backingDevice)
    {
        // Own HARDWARE device with BGRA support (the composition
        // swapchain format is B8G8R8A8). Debug-layer fallback mirrors
        // AttachEngineVisual: retry without DEBUG when the SDK layers
        // aren't installed. NO LUID guard — the backing renders its own
        // solid colour and never OpenSharedResource's the engine texture,
        // so adapter choice is irrelevant here.
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, _countof(wanted), D3D11_SDK_VERSION,
            backingDevice.GetAddressOf(), &got, backingContext.GetAddressOf());
#ifndef NDEBUG
        if (FAILED(hr))
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                wanted, _countof(wanted), D3D11_SDK_VERSION,
                backingDevice.GetAddressOf(), &got, backingContext.GetAddressOf());
        }
#endif
        if (FAILED(hr) || !backingDevice)
        {
            LogLine("[COMP-backing-fail] D3D11CreateDevice hr=" + FormatHresult(hr));
            backingContext.Reset();
            backingDevice.Reset();
            return FAILED(hr) ? hr : E_FAIL;
        }
    }
    if (!backingFactory)
    {
        // CreateDXGIFactory1 + QI to IDXGIFactory2 — same DXSDK-shadowing
        // workaround as AttachEngineVisual ().
        Microsoft::WRL::ComPtr<IDXGIFactory1> f1;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(f1.GetAddressOf()));
        if (FAILED(hr))
        {
            LogLine("[COMP-backing-fail] CreateDXGIFactory1 hr=" + FormatHresult(hr));
            return hr;
        }
        hr = f1.As(&backingFactory);
        if (FAILED(hr) || !backingFactory)
        {
            LogLine("[COMP-backing-fail] QI IDXGIFactory1->IDXGIFactory2 hr=" + FormatHresult(hr));
            return hr;
        }
    }
    return S_OK;
}

void Compositor::Impl::InsertBackingRearmost()
{
    if (!backingVisual || !rootVisual) return;
    // (TRUE, nullptr) prepends to the children list = rendered FIRST =
    // BEHIND all siblings (the spike-bisected MSDN-naming inversion; see
    // AttachWebView2's webview-add comment). Remove first if already in
    // the tree so a re-prepend after a later engine attach keeps the
    // order [backing, engine, webview].
    if (backingInTree) rootVisual->RemoveVisual(backingVisual.Get());
    rootVisual->AddVisual(backingVisual.Get(), TRUE, nullptr);
    backingInTree = true;
}

void Compositor::Impl::ApplyBackingTransform()
{
    if (!backingVisual) return;
    // Scale the 1x1 swapchain content to the full client. Column-major
    // D2D_MATRIX_3X2_F {_11,_12,_21,_22,_31,_32}: pure scale (sx, sy).
    // Clamp to >=1 so a pre-SetSize call doesn't collapse to a zero matrix.
    const float sx = static_cast<float>(lastW > 0 ? lastW : 1);
    const float sy = static_cast<float>(lastH > 0 ? lastH : 1);
    D2D_MATRIX_3X2_F m = { sx, 0.0f, 0.0f, sy, 0.0f, 0.0f };
    backingVisual->SetTransform(m);
}

HRESULT Compositor::Impl::EnsureBackingVisual()
{
    if (backingVisual && backingSwapChain)
    {
        // Already created — make sure it's still rearmost (cheap, covers
        // the case where an engine (re)attach reordered the children).
        InsertBackingRearmost();
        return S_OK;
    }
    HRESULT hr = EnsureBackingDevice();
    if (FAILED(hr)) return hr;

    // 1x1 composition swapchain. Opaque (ALPHA_MODE_IGNORE) — this is the
    // bottom layer; the theme bg is a solid colour. Format/effect match
    // the engine swapchain so the DComp path is identical.
    DXGI_SWAP_CHAIN_DESC1 sc = {};
    sc.Width  = 1;
    sc.Height = 1;
    sc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 2;
    sc.SampleDesc.Count = 1;
    sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sc.AlphaMode  = DXGI_ALPHA_MODE_IGNORE;
    sc.Scaling    = DXGI_SCALING_STRETCH;

    hr = backingFactory->CreateSwapChainForComposition(
        backingDevice.Get(), &sc, nullptr, backingSwapChain.GetAddressOf());
    if (FAILED(hr) || !backingSwapChain)
    {
        LogLine("[COMP-backing-fail] CreateSwapChainForComposition hr=" + FormatHresult(hr));
        backingSwapChain.Reset();
        return FAILED(hr) ? hr : E_FAIL;
    }

    hr = device->CreateVisual(backingVisual.GetAddressOf());
    if (FAILED(hr) || !backingVisual)
    {
        LogLine("[COMP-backing-fail] CreateVisual(backing) hr=" + FormatHresult(hr));
        backingVisual.Reset();
        return FAILED(hr) ? hr : E_FAIL;
    }
    hr = backingVisual->SetContent(backingSwapChain.Get());
    if (FAILED(hr))
    {
        LogLine("[COMP-backing-fail] backingVisual->SetContent hr=" + FormatHresult(hr));
        return hr;
    }

    ApplyBackingTransform();   // scale to current client
    InsertBackingRearmost();   // child 0 (behind engine + webview)
    LogLine("[COMP-backing] backing visual created + inserted rearmost (behind engine)");
    return S_OK;
}

HRESULT Compositor::Impl::PaintBacking(COLORREF color)
{
    if (!backingSwapChain || !backingDevice || !backingContext)
    {
        return E_NOT_VALID_STATE;
    }
    // FLIP swapchains rotate the backbuffer on Present, so re-get buffer 0
    // + recreate the RTV each paint rather than caching a stale view.
    // Recolour only happens on theme change, so this is not hot.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> bb;
    HRESULT hr = backingSwapChain->GetBuffer(0, IID_PPV_ARGS(bb.GetAddressOf()));
    if (FAILED(hr) || !bb)
    {
        LogLine("[COMP-backing-fail] backing GetBuffer(0) hr=" + FormatHresult(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
    hr = backingDevice->CreateRenderTargetView(bb.Get(), nullptr, rtv.GetAddressOf());
    if (FAILED(hr) || !rtv)
    {
        LogLine("[COMP-backing-fail] CreateRenderTargetView hr=" + FormatHresult(hr));
        return FAILED(hr) ? hr : E_FAIL;
    }
    const float rgba[4] = {
        GetRValue(color) / 255.0f,
        GetGValue(color) / 255.0f,
        GetBValue(color) / 255.0f,
        1.0f,   // opaque backing
    };
    backingContext->ClearRenderTargetView(rtv.Get(), rgba);

    DXGI_PRESENT_PARAMETERS pp = {};
    hr = backingSwapChain->Present1(0, 0, &pp);
    if (FAILED(hr))
    {
        LogLine("[COMP-backing-fail] backing Present1 hr=" + FormatHresult(hr));
        return hr;
    }
    return S_OK;
}

Compositor::Compositor(HWND hostHwnd, LogFn log) noexcept
    : m_impl(std::make_unique<Impl>())
{
    m_impl->hwnd = hostHwnd;
    m_impl->log  = std::move(log);
}

Compositor::~Compositor()
{
    // Spike's Shutdown order, lines 783-818. WebView2 controller
    // teardown is the CALLER's responsibility (HostWindow.cpp
    // WM_DESTROY); we trust that's already done by the time we get
    // here. Belt-and-suspenders: clear put_RootVisualTarget anyway
    // so a misordered teardown doesn't crash.
    if (m_impl && m_impl->controller && m_impl->webviewVisual)
    {
        m_impl->controller->put_RootVisualTarget(nullptr);
    }
    // ~unique_ptr<Impl>() releases the Impl, which releases each
    // ComPtr in the documented order (member declaration order
    // reversed): controller, webviewVisual, rootVisual, target,
    // device. Matches the spike's Shutdown sequence.
    if (m_impl) m_impl->LogLine("[COMP] dtor complete");
}

HRESULT Compositor::Init()
{
    if (m_impl->device)
    {
        m_impl->LogLine("[COMP] Init: already up, no-op");
        return S_OK;
    }

    // V2 factory function, V1 IID — same shape as WebView2APISample
    // and dxgi_spike.cpp:InitDComp. v2 tried V2
    // IDCompositionDesktopDevice and still produced white; v3
    // reverted to V1. The spike confirmed V1 works on this rig.
    HRESULT hr = DCompositionCreateDevice2(
        nullptr, IID_PPV_ARGS(m_impl->device.GetAddressOf()));
    if (FAILED(hr) || !m_impl->device)
    {
        m_impl->LogLine("[COMP-fail] DCompositionCreateDevice2 hr=" + FormatHresult(hr));
        return hr;
    }
    m_impl->LogLine("[COMP-init] DComp V1 device created");
    return S_OK;
}

HRESULT Compositor::AttachWebView2(ICoreWebView2CompositionController* ctl)
{
    if (!ctl)
    {
        m_impl->LogLine("[COMP-fail] AttachWebView2 called with null controller");
        return E_POINTER;
    }
    if (m_impl->treeBuilt)
    {
        m_impl->LogLine("[COMP] AttachWebView2: tree already built, no-op");
        return S_OK;
    }
    if (!m_impl->device)
    {
        m_impl->LogLine("[COMP-fail] AttachWebView2 before Init");
        return E_NOT_VALID_STATE;
    }
    HRESULT hr;

    // Create the target. Topmost=TRUE matches spike line 440 +
    // sample line 919. v1 tried both TRUE and FALSE; topmost
    // wasn't the failure mode but matches the working sample.
    hr = m_impl->device->CreateTargetForHwnd(
        m_impl->hwnd, TRUE, m_impl->target.GetAddressOf());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] CreateTargetForHwnd hr=" + FormatHresult(hr));
        return hr;
    }

    // Root visual.
    hr = m_impl->device->CreateVisual(m_impl->rootVisual.GetAddressOf());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] CreateVisual(root) hr=" + FormatHresult(hr));
        return hr;
    }
    hr = m_impl->target->SetRoot(m_impl->rootVisual.Get());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] SetRoot hr=" + FormatHresult(hr));
        return hr;
    }

    // WebView2 visual. Spike adds the engine visual FIRST (so the
    // WebView2 added below renders ABOVE it via list-order). Stage 3
    // has no engine visual yet — only the WebView2. Stage 4 will
    // insert the engine visual BEFORE this one so the z-order stays
    // engine-behind-WebView2 (which is what we want — chrome on top,
    // viewport pixels showing through transparent DOM regions).
    hr = m_impl->device->CreateVisual(m_impl->webviewVisual.GetAddressOf());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] CreateVisual(webview) hr=" + FormatHresult(hr));
        return hr;
    }

    // AddVisual with insertAbove=FALSE + referenceVisual=nullptr puts
    // this visual at the END of the children list, which DComp
    // renders LAST — in front of all siblings (DComp draws children
    // list-order, last-drawn-on-top). The MSDN naming is
    // counterintuitive: "insertAbove=TRUE + NULL ref" actually means
    // "behind all," NOT "above all." Bisected via the spike's
    // --no-engine smoke mode; see dxgi_spike.cpp:489-494 for the
    // long-form comment.
    hr = m_impl->rootVisual->AddVisual(m_impl->webviewVisual.Get(), FALSE, nullptr);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] root->AddVisual(webview) hr=" + FormatHresult(hr));
        return hr;
    }

    // Plug WebView2's surface into the visual. This is the
    // load-bearing call — if put_RootVisualTarget returns S_OK and
    // the tree still produces opaque white, we are in
    // territory.
    hr = ctl->put_RootVisualTarget(m_impl->webviewVisual.Get());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] put_RootVisualTarget hr=" + FormatHresult(hr));
        return hr;
    }
    m_impl->controller = ctl;
    m_impl->LogLine("[COMP-attach] webview visual attached (RootVisualTarget set)");

    hr = m_impl->device->Commit();
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] Commit hr=" + FormatHresult(hr));
        return hr;
    }

    m_impl->treeBuilt = true;
    m_impl->LogLine("[COMP-tree] tree committed (Stage 3: webview-only)");

    // (session 3): if React pushed a backing colour before the tree
    // was built (host/backing-color racing AttachWebView2), apply it now.
    // SetBackingColor caches + no-ops the visual work until the tree is
    // ready; this is the deferred-apply hook.
    if (m_impl->backingColorValid)
    {
        SetBackingColor(m_impl->backingColor);
    }
    return S_OK;
}

HRESULT Compositor::SetSize(int width, int height)
{
    if (!m_impl->device || !m_impl->rootVisual)
    {
        return E_NOT_VALID_STATE;
    }
    if (width == m_impl->lastW && height == m_impl->lastH)
    {
        return S_OK;
    }
    m_impl->lastW = width;
    m_impl->lastH = height;

    // (session 3): rescale the backing visual's 1x1 content to fill
    // the new client. No-op until the backing visual exists; published by
    // the Commit at the end of this function.
    m_impl->ApplyBackingTransform();

    // Root visual offset + clip. Stage 3 anchors the WebView2
    // visual at (0,0) covering the full host client; SetOffsetX/Y
    // at zero is the no-op default but documented here for Stage 4
    // (which will offset the engine visual by the scene-rect
    // origin).
    HRESULT hr = m_impl->rootVisual->SetOffsetX(0.0f);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] SetOffsetX hr=" + FormatHresult(hr));
        return hr;
    }
    hr = m_impl->rootVisual->SetOffsetY(0.0f);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] SetOffsetY hr=" + FormatHresult(hr));
        return hr;
    }

    D2D_RECT_F clip = { 0.0f, 0.0f,
                        static_cast<float>(width),
                        static_cast<float>(height) };
    hr = m_impl->rootVisual->SetClip(clip);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] SetClip hr=" + FormatHresult(hr));
        return hr;
    }

    hr = m_impl->device->Commit();
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-fail] SetSize Commit hr=" + FormatHresult(hr));
        return hr;
    }
    return S_OK;
}

HRESULT Compositor::Commit()
{
    if (!m_impl->device)
    {
        return E_NOT_VALID_STATE;
    }
    return m_impl->device->Commit();
}

HRESULT Compositor::SetBackingColor(COLORREF color) noexcept
{
    // Always cache so a call that races ahead of AttachWebView2 (the tree
    // isn't built yet) is applied later via the deferred-apply hook at the
    // end of AttachWebView2.
    const bool unchanged = (m_impl->backingColorValid && m_impl->backingColor == color);
    m_impl->backingColor      = color;
    m_impl->backingColorValid = true;

    if (!m_impl->device || !m_impl->rootVisual)
    {
        m_impl->LogLine("[COMP-backing] color cached (tree not built yet)");
        return S_OK;
    }
    if (unchanged && m_impl->backingInTree)
    {
        return S_OK;   // idempotent — same colour, already showing
    }

    HRESULT hr = m_impl->EnsureBackingVisual();
    if (FAILED(hr)) return hr;   // already logged [COMP-backing-fail]

    hr = m_impl->PaintBacking(color);
    if (FAILED(hr)) return hr;

    hr = m_impl->device->Commit();
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-backing-fail] SetBackingColor Commit hr=" + FormatHresult(hr));
        return hr;
    }

    char buf[96];
    snprintf(buf, sizeof(buf),
             "[COMP-backing] recolor #%02X%02X%02X applied (rearmost visual)",
             GetRValue(color), GetGValue(color), GetBValue(color));
    m_impl->LogLine(buf);
    return S_OK;
}

// ---------- Phase 3 Stage 4 — engine visual ----------
// 4a shipped declarations + stub bodies; 4b replaces AttachEngineVisual
// with the real D3D11 + DXGI + DComp wiring. CompositeEngineFrame and
// RefreshEngineSharedHandle stay stubs until 4c / 4d respectively.

HRESULT Compositor::AttachEngineVisual(HANDLE sharedTexture,
                                       int    w,
                                       int    h,
                                       LUID   engineAdapterLuid) noexcept
{
    if (!sharedTexture || w <= 0 || h <= 0)
    {
        m_impl->LogLine("[COMP-engine-fail] AttachEngineVisual: invalid params (handle/w/h)");
        return E_INVALIDARG;
    }
    if (!m_impl->device || !m_impl->rootVisual)
    {
        m_impl->LogLine("[COMP-engine-fail] AttachEngineVisual called before AttachWebView2");
        return E_NOT_VALID_STATE;
    }

    // Idempotence — same handle + size, already attached: no-op.
    if (m_impl->engineVisualAttached &&
        m_impl->engineHandleCached == sharedTexture &&
        m_impl->engineWidthCached  == w &&
        m_impl->engineHeightCached == h)
    {
        m_impl->LogLine("[COMP-engine-init] AttachEngineVisual: already attached with same params, no-op");
        return S_OK;
    }

    HRESULT hr;
    char buf[192];

    // 1. D3D11 device (lazy create — persists across re-attach with
    // different handle/size since the device is adapter-bound, not
    // resource-bound).
    if (!m_impl->d3d11Device)
    {
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL wanted[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;

        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, _countof(wanted), D3D11_SDK_VERSION,
            m_impl->d3d11Device.GetAddressOf(), &chosenLevel,
            m_impl->d3d11Context.GetAddressOf());
#ifndef NDEBUG
        if (FAILED(hr))
        {
            // SDK debug layer not installed — retry without DEBUG flag.
            // Spike's pattern at dxgi_spike.cpp:322. Production Debug
            // builds with SDK layers proceed via the first path; Debug
            // builds on a machine without SDK layers fall back here.
            m_impl->LogLine("[COMP-engine-init] D3D11CreateDevice with DEBUG failed hr=" + FormatHresult(hr) + " — retrying without (SDK layers missing?)");
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                wanted, _countof(wanted), D3D11_SDK_VERSION,
                m_impl->d3d11Device.GetAddressOf(), &chosenLevel,
                m_impl->d3d11Context.GetAddressOf());
        }
#endif
        if (FAILED(hr) || !m_impl->d3d11Device)
        {
            m_impl->LogLine("[COMP-engine-fail] D3D11CreateDevice hr=" + FormatHresult(hr));
            m_impl->d3d11Context.Reset();
            m_impl->d3d11Device.Reset();
            return hr;
        }
        snprintf(buf, sizeof(buf),
                 "[COMP-engine-init] D3D11 device created (level=0x%X flags=0x%X)",
                 static_cast<unsigned>(chosenLevel), flags);
        m_impl->LogLine(buf);

        // LUID guard. Query the D3D11 device's adapter and compare
        // against the engine's. On mismatch (hybrid-GPU laptops), bail
        // BEFORE OpenSharedResource — that call silently returns a
        // different texture across adapters, so the visible failure
        // mode is "engine pixels are garbage" rather than a clean
        // error. Catching it here turns it into a clean log line +
        // skip-engine-attach.
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(m_impl->d3d11Device.As(&dxgiDevice)))
        {
            Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf())))
            {
                DXGI_ADAPTER_DESC desc = {};
                adapter->GetDesc(&desc);
                snprintf(buf, sizeof(buf),
                         "[COMP-engine-luid] D3D11 adapter LUID=%08lX-%08lX (engine LUID=%08lX-%08lX)",
                         static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                         static_cast<unsigned long>(desc.AdapterLuid.LowPart),
                         static_cast<unsigned long>(engineAdapterLuid.HighPart),
                         static_cast<unsigned long>(engineAdapterLuid.LowPart));
                m_impl->LogLine(buf);

                const bool callerProvidedLuid =
                    (engineAdapterLuid.HighPart != 0 || engineAdapterLuid.LowPart != 0);
                if (callerProvidedLuid &&
                    (desc.AdapterLuid.HighPart != engineAdapterLuid.HighPart ||
                     desc.AdapterLuid.LowPart  != engineAdapterLuid.LowPart))
                {
                    m_impl->LogLine("[COMP-engine-fail] LUID mismatch — engine D3D9Ex and Compositor D3D11 picked different adapters; skipping engine visual attach (composition mode stays, viewport area empty)");
                    m_impl->d3d11Context.Reset();
                    m_impl->d3d11Device.Reset();
                    // Distinctive HRESULT so callers can disambiguate
                    // the LUID-mismatch fallback from other failure modes.
                    return DXGI_ERROR_GRAPHICS_VIDPN_SOURCE_IN_USE;
                }
            }
        }
    }

    // 2. DXGI factory (lazy create — same lifecycle as the D3D11 device).
    //
    // ParticleEditor.vcxproj puts $(DXSDK_DIR)Lib\x64 FIRST on
    // AdditionalLibraryDirectories (for d3dx9.lib), which shadows the
    // Win10 SDK's dxgi.lib with DXSDK June 2010's pre-Win8 version
    // that lacks CreateDXGIFactory2. Same shape as include-
    // path shadowing on the linker side. Workaround: use
    // CreateDXGIFactory1 (in DXSDK's dxgi.lib since Win7 SDK era) +
    // QI to IDXGIFactory2. If QI fails, the system is pre-Win8 and
    // CreateSwapChainForComposition (also DXGI 1.2) wouldn't work
    // anyway — composition mode requires DXGI 1.2. Spike sidesteps
    // this entirely because dxgi_spike.vcxproj doesn't reference
    // DXSDK at all; its dxgi.lib resolves to the Win10 SDK directly.
    if (!m_impl->dxgiFactory)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
        hr = CreateDXGIFactory1(IID_PPV_ARGS(factory1.GetAddressOf()));
        if (FAILED(hr))
        {
            m_impl->LogLine("[COMP-engine-fail] CreateDXGIFactory1 hr=" + FormatHresult(hr));
            return hr;
        }
        hr = factory1.As(&m_impl->dxgiFactory);
        if (FAILED(hr) || !m_impl->dxgiFactory)
        {
            m_impl->LogLine("[COMP-engine-fail] QI IDXGIFactory1→IDXGIFactory2 hr=" + FormatHresult(hr) + " (pre-Win8 system? DXGI 1.2 required for composition)");
            return hr;
        }
    }

    // 3. Open the engine's shared texture as a D3D11 alias.
    m_impl->sharedTexD3D11.Reset();
    hr = m_impl->d3d11Device->OpenSharedResource(
        sharedTexture, IID_PPV_ARGS(m_impl->sharedTexD3D11.GetAddressOf()));
    if (FAILED(hr) || !m_impl->sharedTexD3D11)
    {
        m_impl->LogLine("[COMP-engine-fail] OpenSharedResource hr=" + FormatHresult(hr));
        return hr;
    }
    {
        D3D11_TEXTURE2D_DESC desc = {};
        m_impl->sharedTexD3D11->GetDesc(&desc);
        snprintf(buf, sizeof(buf),
                 "[COMP-engine-open] OpenSharedResource handle=%p texSize=%ux%u fmt=%u bind=0x%X share=0x%X",
                 sharedTexture, desc.Width, desc.Height,
                 static_cast<unsigned>(desc.Format), desc.BindFlags, desc.MiscFlags);
        m_impl->LogLine(buf);
    }

    // 4. Composition swapchain. Format + alpha + buffer count match
    // dxgi_spike.cpp:377-396 exactly — that combination works on the
    // user's RTX 3080 per the Stage 0 spike measurements.
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width  = static_cast<UINT>(w);
    scDesc.Height = static_cast<UINT>(h);
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;       // matches engine's D3DFMT_A8R8G8B8
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SampleDesc.Count = 1;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // Phase 3 Stage 4d.1 — ALPHA_MODE_IGNORE, NOT PREMULTIPLIED.
    //
    // The spike's swapchain used DXGI_ALPHA_MODE_PREMULTIPLIED (its
    // engine workload was D3DClear() to solid color, alpha was clean).
    // The production engine's particle blend states (additive for
    // fire, alpha-blend for smoke, etc.) leave the engine RT's alpha
    // channel in an arbitrary state — the engine never cared because
    // legacy arch-A's UpdateLayeredWindow uses the popup's STAMPED
    // alpha (from AlphaCompositor::Composite), not the RT's alpha.
    //
    // Under DXGI with PREMULTIPLIED, DComp interpreted the RT's RGB
    // as already-multiplied-by-alpha. When alpha was less than full
    // over particle-blended regions, DComp's compositing math
    // darkened the output — visible as "additive fire sprites
    // overlapping smoke render with dark/black backgrounds" during
    // 4d smoke (the user-surfaced bug that originally read as a
    // separate issue).
    //
    // IGNORE tells DComp "treat this surface as fully opaque; don't
    // try to blend with what's behind." Engine visual becomes an
    // opaque rectangle at (0,0,W,H); WebView2 chrome composites on
    // top where the WebView2 visual is opaque, transparent regions
    // show full-opacity engine. That's the legacy parity.
    scDesc.AlphaMode  = DXGI_ALPHA_MODE_IGNORE;
    scDesc.Scaling    = DXGI_SCALING_STRETCH;          // tolerates engine/swapchain size mismatch

    m_impl->engineSwapChain.Reset();
    m_impl->engineBackBuffer.Reset();
    hr = m_impl->dxgiFactory->CreateSwapChainForComposition(
        m_impl->d3d11Device.Get(), &scDesc, nullptr,
        m_impl->engineSwapChain.GetAddressOf());
    if (FAILED(hr) || !m_impl->engineSwapChain)
    {
        m_impl->LogLine("[COMP-engine-fail] CreateSwapChainForComposition hr=" + FormatHresult(hr));
        return hr;
    }
    snprintf(buf, sizeof(buf),
             "[COMP-engine-swap] composition swapchain created %dx%d FLIP_SEQ BGRA8 premul", w, h);
    m_impl->LogLine(buf);

    hr = m_impl->engineSwapChain->GetBuffer(0, IID_PPV_ARGS(m_impl->engineBackBuffer.GetAddressOf()));
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] swapchain GetBuffer(0) hr=" + FormatHresult(hr));
        return hr;
    }

    // 5. Engine visual. Replace any prior attempt — Re-attach with
    // different params recreates from scratch (the cached state at
    // top of function would have caught no-change case).
    if (m_impl->engineVisual)
    {
        m_impl->rootVisual->RemoveVisual(m_impl->engineVisual.Get());
        m_impl->engineVisual.Reset();
    }
    hr = m_impl->device->CreateVisual(m_impl->engineVisual.GetAddressOf());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] CreateVisual(engine) hr=" + FormatHresult(hr));
        return hr;
    }
    hr = m_impl->engineVisual->SetContent(m_impl->engineSwapChain.Get());
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] engineVisual->SetContent(swapchain) hr=" + FormatHresult(hr));
        return hr;
    }

    // 6. Insert engine visual BEHIND the WebView2 visual. The MSDN-
    // naming inversion: `AddVisual(visual, insertAbove=TRUE,
    // referenceVisual=nullptr)` actually places this visual at the
    // BEGINNING of the children list (= rendered FIRST = BEHIND all
    // siblings) — bisected from spike's --no-engine smoke mode (see
    // dxgi_spike.cpp:488-495 long comment). The Stage 3 webview was
    // added via AttachWebView2 with (FALSE, nullptr) which places it
    // at the END of the children list (= rendered LAST = IN FRONT of
    // all siblings). So after this AddVisual call the children list
    // is [engine, webview]; DComp draws engine first, webview on top.
    // Chrome with opaque backgrounds occludes; transparent regions
    // show engine through. (D3 OK + sub-plan §3.4.)
    hr = m_impl->rootVisual->AddVisual(m_impl->engineVisual.Get(), TRUE, nullptr);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] root->AddVisual(engine, behind) hr=" + FormatHresult(hr));
        return hr;
    }

    // (session 3): the engine was just prepended to the children
    // list; if the theme-coloured backing was already present it is now
    // IN FRONT of the engine. Re-prepend the backing so the order stays
    // [backing, engine, webview]. No-op when the backing hasn't been
    // created yet.
    m_impl->InsertBackingRearmost();

    // 7. Commit.
    hr = m_impl->device->Commit();
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] AttachEngineVisual Commit hr=" + FormatHresult(hr));
        return hr;
    }

    // Cache (handle, size) + flag. Lazy detection in 4c's real
    // CompositeEngineFrame will check this tuple against
    // engine->GetSharedTextureHandle() each frame; 4d's resize-handle
    // re-open hooks here too.
    m_impl->engineHandleCached    = sharedTexture;
    m_impl->engineWidthCached     = w;
    m_impl->engineHeightCached    = h;
    m_impl->engineVisualAttached  = true;
    m_impl->LogLine("[COMP-engine-attach] engine visual attached (behind WebView2, swapchain content set, tree committed)");

    return S_OK;
}

HRESULT Compositor::CompositeEngineFrame(HANDLE currentSharedHandle) noexcept
{
    // No engine visual attached → S_FALSE per the documented
    // contract. Host's per-frame loop treats S_FALSE as "skip the
    // composite step this frame." Triggered by:
    //   - Stage 3 baseline (no AttachEngineVisual ever called).
    //   - AttachEngineVisual failed (LUID mismatch, D3D11 device,
    //     OpenSharedResource, swapchain) — composition mode stays
    //     intact per §3.8, viewport stays empty.
    if (!m_impl->engineVisualAttached) return S_FALSE;

    // Defensive — engineVisualAttached implies all resources exist,
    // but cover the edge case where teardown is mid-flight on a
    // different thread (shouldn't happen on the single-threaded
    // message pump, but cheap insurance).
    if (!m_impl->d3d11Context || !m_impl->engineSwapChain)
    {
        return E_NOT_VALID_STATE;
    }

    // Phase 3 Stage 4d — lazy resize-handle re-open. Every
    // AlphaCompositor::Resize call invalidates the previous shared
    // HANDLE and creates a new one. The previous CopyResource path
    // would read from a released D3D9 texture (visible as a frozen
    // viewport — engine keeps rendering into new VRAM but our alias
    // still points at the old, dead allocation). Per-frame compare +
    // re-open recovers in 1 frame.
    //
    // Pointer compare per frame is the dominant steady-state cost;
    // the re-open path only runs on actual resize events. nullptr is
    // tolerated as a sentinel for "engine doesn't have a shared
    // texture right now" (e.g. AlphaCompositor not yet Resized) —
    // we skip the composite that frame; previous frame remains on
    // screen.
    if (!currentSharedHandle) return S_FALSE;
    if (currentSharedHandle != m_impl->engineHandleCached)
    {
        // Re-open against the new handle. RefreshEngineSharedHandle
        // ignores the (w, h) hints — it reads the actual size from
        // the texture descriptor after OpenSharedResource, then
        // calls IDXGISwapChain1::ResizeBuffers if needed.
        HRESULT rhr = RefreshEngineSharedHandle(currentSharedHandle, 0, 0);
        if (FAILED(rhr))
        {
            // Re-open failed — clear cache so the next frame retries
            // (RefreshEngineSharedHandle clears engineHandleCached on
            // failure). Return the failure HRESULT so the host can
            // observe; the visible symptom is "viewport stays at the
            // last successful composite" until re-open succeeds.
            return rhr;
        }
    }

    if (!m_impl->engineBackBuffer || !m_impl->sharedTexD3D11)
    {
        // Defensive — RefreshEngineSharedHandle should have populated
        // both. Cover an interim state where teardown raced.
        return E_NOT_VALID_STATE;
    }

    // D3D11 CopyResource — source and dest sizes match (the
    // RefreshEngineSharedHandle path above ResizeBuffers'd the
    // swapchain to the texture descriptor's actual size before
    // returning success, so the steady-state invariant holds).
    m_impl->d3d11Context->CopyResource(
        m_impl->engineBackBuffer.Get(),
        m_impl->sharedTexD3D11.Get());

    // Present1 with no sync interval — composition rate is throttled
    // by DComp's commit cadence (~60-360 Hz depending on monitor +
    // composition refresh), not by Present rate. Spike measured
    // 0.30 ms total transport at 3440x1440 — Present is essentially
    // free, the spin in WaitEndFrameQuery dominates.
    DXGI_PRESENT_PARAMETERS pp = {};
    HRESULT hr = m_impl->engineSwapChain->Present1(0, 0, &pp);
    if (FAILED(hr))
    {
        m_impl->LogLine("[COMP-engine-fail] Present1 hr=" + FormatHresult(hr));
        return hr;
    }

    // Phase 3 Stage 5 T6 follow-up — apply any pending scene-
    // rect transform NOW that the swapchain has fresh pixels from the
    // engine's render at the new viewport. Without this deferral
    // (i.e. if SetEngineVisualTransform Commit'd the new clip
    // immediately on dispatch), the next DWM composition cycle would
    // composite the OLD swapchain content (pre-render with the old
    // viewport) through the NEW clip rect — visible as a transient
    // engine-clear-color strip at the widened clip edges during
    // pane-drag resize storms. Applying after Present1 ensures the
    // clip update arrives at the same DWM cycle as the new pixels.
    if (m_impl->pendingTransform)
    {
        // Idempotence vs already-applied — if the engine's render +
        // CopyResource above already corresponded to a viewport
        // matching the pending transform (the common case during
        // smooth drags), there's still a clip update to push because
        // SetEngineVisualTransform deferred it. Apply unconditionally
        // unless the pending value matches the cached LAST one (also
        // a common case after a single-tick stall where the pending
        // got queued and immediately overwritten by a no-op).
        if (m_impl->pendingX != m_impl->engineLastX ||
            m_impl->pendingY != m_impl->engineLastY ||
            m_impl->pendingW != m_impl->engineLastTransformW ||
            m_impl->pendingH != m_impl->engineLastTransformH)
        {
            (void)m_impl->ApplyTransform(
                m_impl->pendingX, m_impl->pendingY,
                m_impl->pendingW, m_impl->pendingH,
                m_impl->pendingQuiet);
            // Best-effort: log and proceed even on failure (the visible
            // result is "scene-rect doesn't update," which is the same
            // shape the user already sees during drag storms — not a
            // worse state, just a missed sync). ApplyTransform itself
            // logs the failure via [COMP-engine-fail].
        }
        m_impl->pendingTransform = false;
    }

    // 1 Hz throttled diagnostics. GetTickCount() wraps after ~49 days
    // of uptime; we don't care because the editor session is unlikely
    // to span that.
    m_impl->engineFrameCount++;
    DWORD now = GetTickCount();
    if (m_impl->engineLastFrameLogTick == 0 ||
        (now - m_impl->engineLastFrameLogTick) >= 1000)
    {
        m_impl->engineLastFrameLogTick = now;

        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[COMP-engine-frame] composite n=%llu (1 Hz throttle)",
                 static_cast<unsigned long long>(m_impl->engineFrameCount));
        m_impl->LogLine(buf);

        // [COMP-engine-handle-hash] sanity diagnostic per the
        // dxgi-stage-4 sub-plan §4 4c addendum. Logs current handle
        // value + cached resource COM-object addresses each second.
        // If `sharedTex` pointer changes mid-run WITHOUT a preceding
        // [COMP-engine-resize] entry, OpenSharedResource silently
        // returned a different texture — the spike's documented
        // wrong-handle failure mode at dxgi_spike.cpp:355-357.
        snprintf(buf, sizeof(buf),
                 "[COMP-engine-handle-hash] handle=%p sharedTex=%p backBuffer=%p texSize=%dx%d",
                 m_impl->engineHandleCached,
                 m_impl->sharedTexD3D11.Get(),
                 m_impl->engineBackBuffer.Get(),
                 m_impl->engineWidthCached,
                 m_impl->engineHeightCached);
        m_impl->LogLine(buf);
    }

    return S_OK;
}

HRESULT Compositor::RefreshEngineSharedHandle(HANDLE sharedTexture,
                                              int    hintW,
                                              int    hintH) noexcept
{
    (void)hintW;  // advisory only — actual size read from texture descriptor
    (void)hintH;

    if (!sharedTexture)
    {
        m_impl->LogLine("[COMP-engine-fail] RefreshEngineSharedHandle: null handle");
        return E_INVALIDARG;
    }
    if (!m_impl->d3d11Device || !m_impl->engineSwapChain)
    {
        m_impl->LogLine("[COMP-engine-fail] RefreshEngineSharedHandle: D3D11 device or swapchain missing (call AttachEngineVisual first)");
        return E_NOT_VALID_STATE;
    }

    // Re-open the D3D11 alias against the new handle. The old alias
    // pointed at released D3D9 VRAM and any further CopyResource on
    // it would read garbage.
    m_impl->sharedTexD3D11.Reset();
    HRESULT hr = m_impl->d3d11Device->OpenSharedResource(
        sharedTexture, IID_PPV_ARGS(m_impl->sharedTexD3D11.GetAddressOf()));
    if (FAILED(hr) || !m_impl->sharedTexD3D11)
    {
        m_impl->LogLine("[COMP-engine-fail] RefreshEngineSharedHandle: OpenSharedResource hr=" + FormatHresult(hr));
        // Clear the cache so the next CompositeEngineFrame retries —
        // the engine may produce a new valid handle on the next
        // AlphaCompositor::Resize that we'll pick up automatically.
        m_impl->engineHandleCached = nullptr;
        return hr;
    }

    // Read the texture's actual size from the descriptor — this is
    // authoritative even when the caller's (hintW, hintH) are stale.
    D3D11_TEXTURE2D_DESC desc = {};
    m_impl->sharedTexD3D11->GetDesc(&desc);
    const int newW = static_cast<int>(desc.Width);
    const int newH = static_cast<int>(desc.Height);

    // ResizeBuffers if the swapchain back buffer's size differs from
    // the new alias. DXGI requires releasing the cached back buffer
    // BEFORE ResizeBuffers (any outstanding back-buffer ref blocks
    // the resize), then re-acquiring after. DXGI_FORMAT_UNKNOWN +
    // BufferCount=0 means "keep the existing format / count" —
    // semantically a size-only resize. The DComp engineVisual's
    // SetContent(swapchain) reference stays valid through
    // ResizeBuffers (DXGI documents this; visual identity is on the
    // IDXGISwapChain1, not on the back buffers it holds).
    if (newW != m_impl->engineWidthCached || newH != m_impl->engineHeightCached)
    {
        m_impl->engineBackBuffer.Reset();
        hr = m_impl->engineSwapChain->ResizeBuffers(
            0, static_cast<UINT>(newW), static_cast<UINT>(newH),
            DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr))
        {
            m_impl->LogLine("[COMP-engine-fail] RefreshEngineSharedHandle: ResizeBuffers hr=" + FormatHresult(hr));
            m_impl->engineHandleCached = nullptr;
            return hr;
        }
        hr = m_impl->engineSwapChain->GetBuffer(0, IID_PPV_ARGS(m_impl->engineBackBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            m_impl->LogLine("[COMP-engine-fail] RefreshEngineSharedHandle: GetBuffer post-Resize hr=" + FormatHresult(hr));
            m_impl->engineHandleCached = nullptr;
            return hr;
        }
    }

    // Diagnostic — emit BEFORE updating the cache so the line shows
    // both old and new values. Resize storms (mid-drag) produce one
    // line per actual handle change; the 1 Hz throttle on
    // [COMP-engine-frame] doesn't apply here (events are sparse).
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[COMP-engine-resize] handle %p -> %p, size %dx%d -> %dx%d",
             m_impl->engineHandleCached, sharedTexture,
             m_impl->engineWidthCached, m_impl->engineHeightCached,
             newW, newH);
    m_impl->LogLine(buf);

    m_impl->engineHandleCached = sharedTexture;
    m_impl->engineWidthCached  = newW;
    m_impl->engineHeightCached = newH;
    return S_OK;
}

// ---------- Phase 3 Stage 5 — scene-rect transform ----------
//
// Positions the visible portion of the DComp engine visual to the
// LayoutBroker's scene-rect sub-region of the host client. See
// Compositor.h for the public contract.
//
// Design (sub-plan §3.1 / D1 — Option A, post-fix):
//   - Swapchain stays at engine RT size (full host client). No
//     ResizeBuffers per scene-rect update; DComp's per-visual
//     SetClip is cheap (nanoseconds) vs swapchain resize storms
//     during pane drags.
//
// COORDINATE-SPACE CONTRACT (load-bearing — initial Stage 5 ship
// had this inverted; see lessons.md candidate). Under B-γ,
// the engine renders its scene into the SCENE-RECT SUB-REGION of
// its full-client RT (Engine::SetSceneViewport calls SetViewport
// with (sceneX, sceneY, sceneW, sceneH) after the full-RT Clear,
// per the D12 ordering rule). The CopyResource at every frame
// makes the swapchain back-buffer a verbatim copy of the RT, so
// the scene lives at swapchain pixels [sceneX..sceneX+W,
// sceneY..sceneY+H] and engine clear color is elsewhere.
//
// The DComp engine visual paints its swapchain content at its
// local (0, 0) onwards. With SetOffset(0, 0) (default), the
// visual's local coord space EQUALS its parent's (the root
// visual, which is at host-client coords). So swapchain pixel
// (X, Y) is painted at host-client (X, Y) — which is exactly
// where the engine drew the scene. SetClip({x, y, x+w, y+h})
// constrains the visible region to the scene-rect rectangle on
// screen.
//
// We do NOT use SetOffset(sceneX, sceneY) here. That would shift
// the entire swapchain by (sceneX, sceneY), creating a double-
// offset against the engine's already-scene-rect-positioned RT
// content (visible as the scene appearing in the bottom-right
// corner of the scene-rect quadrant). The original sub-plan §3.1
// described SetOffset + clip-at-(0,0,W,H), which would have worked
// IF the engine rendered at RT (0, 0); under B-γ, the engine
// renders at RT (sceneX, sceneY), so SetOffset(0, 0) + clip at
// absolute coords is the matching shape.
HRESULT Compositor::SetEngineVisualTransform(int x, int y, int w, int h, bool immediate, bool quiet) noexcept
{
    if (!m_impl->engineVisualAttached) return S_FALSE;
    if (!m_impl->engineVisual || !m_impl->device)
    {
        return E_NOT_VALID_STATE;
    }
    if (w <= 0 || h <= 0)
    {
        m_impl->LogLine("[COMP-engine-fail] SetEngineVisualTransform: degenerate w/h");
        return E_INVALIDARG;
    }

    // Idempotence — same scene-rect, no-op. Silent (no log line) so
    // resize storms during a pane drag at 60+ Hz don't flood host.log.
    // Compare against the cache (last actually-applied) AND any pending
    // queued transform, so back-to-back identical SetSceneRect events
    // collapse to nothing.
    const bool sameAsApplied =
        (x == m_impl->engineLastX && y == m_impl->engineLastY &&
         w == m_impl->engineLastTransformW &&
         h == m_impl->engineLastTransformH);
    const bool sameAsPending =
        (m_impl->pendingTransform &&
         x == m_impl->pendingX && y == m_impl->pendingY &&
         w == m_impl->pendingW && h == m_impl->pendingH);
    if (sameAsApplied && !m_impl->pendingTransform) return S_OK;
    if (sameAsPending) return S_OK;

    if (immediate)
    {
        // Apply right now — for the attach-time seed where no engine
        // render has happened yet and there's nothing to coordinate
        // with. Clear any stale pending queue too.
        m_impl->pendingTransform = false;
        return m_impl->ApplyTransform(x, y, w, h, quiet);
    }

    // Default: queue for application at the end of the next
    // CompositeEngineFrame, AFTER Present1 has pushed fresh swapchain
    // pixels for the new viewport. Eliminates the transient
    // engine-clear-color strip at new clip edges during pane-drag
    // resize storms (visible because DComp's clip widens on Commit
    // while the engine's render of the new viewport lags by one frame).
    //
    // The queue holds only the MOST RECENT pending transform — earlier
    // queued transforms are replaced. With drag dispatching at 60+ Hz
    // and engine rendering at ~100 Hz, this means at most a 1-frame
    // delay between dispatch and visible clip update, never a backlog.
    m_impl->pendingTransform = true;
    m_impl->pendingQuiet = quiet;
    m_impl->pendingX = x;
    m_impl->pendingY = y;
    m_impl->pendingW = w;
    m_impl->pendingH = h;
    return S_OK;
}

// ---------------------------------------------------------------

bool Compositor::IsReady() const noexcept
{
    return m_impl && m_impl->treeBuilt;
}

HWND Compositor::HostHwnd() const noexcept
{
    return m_impl ? m_impl->hwnd : nullptr;
}

} // namespace host
