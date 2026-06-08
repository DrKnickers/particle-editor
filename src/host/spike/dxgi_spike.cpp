// dxgi_spike.cpp — Phase 3 Stage 0 spike app
// =====================================================
//
// Standalone exe that proves (or disproves) the DXGI shared-handle visual-
// hosting pipeline end-to-end. Render pipeline:
//
//   D3D9Ex device
//     creates shared-handle render-target texture (D3DFMT_A8R8G8B8)
//        ↓ (shared HANDLE, NT-handle semantics)
//   D3D11 device
//     OpenSharedResource → ID3D11Texture2D wrapping the same VRAM
//     creates DXGI flip-model swapchain (CreateSwapChainForComposition)
//        ↓ per frame: CopyResource(swapchain back, sharedTex); Present
//   IDCompositionDevice (V1 — matches WebView2APISample topology)
//     IDCompositionTarget bound to host HWND
//     rootVisual
//       ├─ engineVisual   ← SetContent(swapchain)
//       └─ webviewVisual  ← put_RootVisualTarget (CompositionController)
//
// On every frame the spike:
//   1. D3D9Ex SetRenderTarget(sharedTex), Clear() to a time-varying colour,
//      EndScene(), then issues a D3D9 event query and polls until GPU
//      completion (cross-device sync without an ID3D11Fence — works on
//      Win10+ without requiring D3D11.4).
//   2. D3D11 CopyResource(swapchainBack, sharedTex) then Present(0,0).
//   3. DComp picks up the new swapchain content automatically (the visual
//      tree is committed once at startup; no per-frame Commit needed).
//
// Heavy logging: every API HRESULT, shared handle value, device LUIDs,
// visual tree topology, per-frame phase timings via QueryPerformanceCounter.
//
// CRITICAL: this is the FOURTH attempt at WebView2 visual hosting on this
// codebase. The first three (v1/v2/v3) all produced opaque-white output
// despite every API returning S_OK. See
//   docs/superpowers/research/dxgi---history.md
// for the post-mortem. The differential here vs: both engine and
// WebView2 are DComp visuals, NOT one-DComp-one-Win32-child. That mixed
// paradigm is what v1/v2/v3 and SetWindowRgn both hit. If THIS
// spike also produces white, mark NO-GO and revert. Do not iterate.
//
// Build: dxgi_spike.vcxproj (this dir). Output: x64/Debug/dxgi_spike.exe.
// Run:   dxgi_spike.exe [--w=N] [--h=N] [--no-webview2] [--no-engine]
//                       [--res=720p|1080p|1440p|3440x1440] [--log=path]
// Log:   %TEMP%\dxgi_spike.log (also OutputDebugString — DbgView++ to
//        observe live).
//
// What to look for on a successful run:
//   - Window title shows live FPS, e.g. "DXGI Spike — 142 FPS @ 1440p"
//   - Centre of window shows a slowly-rotating colour
//   - Top + bottom 40px show semi-transparent dark bars with text (WebView2)
//   - Clicking the bottom-right "click probe" button changes its text
//   - %TEMP%\dxgi_spike.log: [SPIKE] frame=N total=X.XXms fps=YYY.Y …
//   - Per CLAUDE.md: don't conclude from surface symptoms — verify
//     log shows non-zero phase timings AND visual surface matches.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>

#include "WebView2.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Callback;

namespace {

// ---------------------------------------------------------------------------
// Configuration / globals
// ---------------------------------------------------------------------------

constexpr wchar_t kWindowClassName[] = L"DxgiSpikeMain";

struct SpikeConfig {
    int  width        = 1280;
    int  height       = 800;
    bool enableWebView2 = true;
    bool enableEngine   = true;          // --no-engine: clears swapchain only
    const wchar_t* resLabel = L"custom";
    std::wstring logPath;                 // empty → %TEMP%\dxgi_spike.log
};

SpikeConfig g_cfg;
FILE* g_logFile = nullptr;

HWND g_mainWnd = nullptr;

// D3D9Ex side (the "engine" stand-in)
ComPtr<IDirect3D9Ex>          g_d3d9;
ComPtr<IDirect3DDevice9Ex>    g_d3d9Device;
ComPtr<IDirect3DTexture9>     g_sharedTexD3D9;       // shared-handle RT
ComPtr<IDirect3DSurface9>     g_sharedSurfD3D9;      // level-0 surface view
ComPtr<IDirect3DQuery9>       g_d3d9SyncQuery;       // GPU-fence-ish
HANDLE                        g_sharedHandle = nullptr;

// D3D11 side (the bridge to DComp)
ComPtr<ID3D11Device>          g_d3d11Device;
ComPtr<ID3D11DeviceContext>   g_d3d11Context;
ComPtr<ID3D11Texture2D>       g_sharedTexD3D11;      // OpenSharedResource view
ComPtr<IDXGIFactory2>         g_dxgiFactory;
ComPtr<IDXGISwapChain1>       g_engineSwapChain;
ComPtr<ID3D11Texture2D>       g_engineBackBuffer;    // current swapchain back

// DComp tree
ComPtr<IDCompositionDevice>   g_dcompDevice;         // V1, matches sample
ComPtr<IDCompositionTarget>   g_dcompTarget;
ComPtr<IDCompositionVisual>   g_rootVisual;
ComPtr<IDCompositionVisual>   g_engineVisual;
ComPtr<IDCompositionVisual>   g_webviewVisual;

// WebView2
ComPtr<ICoreWebView2Environment>             g_webviewEnv;
ComPtr<ICoreWebView2CompositionController>   g_compositionController;
ComPtr<ICoreWebView2Controller>              g_webController;
ComPtr<ICoreWebView2>                        g_webview;
bool g_webviewReady = false;
bool g_treeBuilt    = false;

// Per-frame state
LARGE_INTEGER g_qpcFreq = {};
uint64_t      g_frameCount = 0;
double        g_emaFps = 0.0;
double        g_emaTotalMs = 0.0;
DWORD         g_lastTitleUpdateMs = 0;

// ---------------------------------------------------------------------------
// Logging — log file + OutputDebugString
// ---------------------------------------------------------------------------

void LogDbg(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fflush(g_logFile);
    }
}

void OpenLogFile(const std::wstring& explicitPath) {
    std::wstring logPath = explicitPath;
    if (logPath.empty()) {
        wchar_t tempDir[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tempDir) == 0) return;
        logPath = std::wstring(tempDir) + L"dxgi_spike.log";
    }
    _wfopen_s(&g_logFile, logPath.c_str(), L"w");
    if (g_logFile) {
        LogDbg("[SPIKE] === dxgi_spike started ===\n");
        char ascii[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, logPath.c_str(), -1, ascii, MAX_PATH, nullptr, nullptr);
        LogDbg("[SPIKE] log file: %s\n", ascii);
    }
}

void CloseLogFile() {
    if (g_logFile) {
        LogDbg("[SPIKE] === dxgi_spike exiting (frames=%llu, ema fps=%.1f) ===\n",
               static_cast<unsigned long long>(g_frameCount), g_emaFps);
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Command-line parsing
// ---------------------------------------------------------------------------

void ApplyResolution(const wchar_t* preset, SpikeConfig& cfg) {
    if (wcscmp(preset, L"720p") == 0)        { cfg.width = 1280; cfg.height = 720;  cfg.resLabel = L"720p"; }
    else if (wcscmp(preset, L"1080p") == 0)  { cfg.width = 1920; cfg.height = 1080; cfg.resLabel = L"1080p"; }
    else if (wcscmp(preset, L"1440p") == 0)  { cfg.width = 2560; cfg.height = 1440; cfg.resLabel = L"1440p"; }
    else if (wcscmp(preset, L"3440x1440") == 0) {
        cfg.width = 3440; cfg.height = 1440; cfg.resLabel = L"3440x1440";
    } else {
        LogDbg("[SPIKE] unknown --res preset %ls; keeping defaults\n", preset);
    }
}

void ParseCommandLine(SpikeConfig& cfg) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcsncmp(a, L"--w=", 4) == 0)         cfg.width  = static_cast<int>(_wtoi(a + 4));
        else if (wcsncmp(a, L"--h=", 4) == 0)    cfg.height = static_cast<int>(_wtoi(a + 4));
        else if (wcsncmp(a, L"--res=", 6) == 0)  ApplyResolution(a + 6, cfg);
        else if (wcscmp(a, L"--no-webview2") == 0) cfg.enableWebView2 = false;
        else if (wcscmp(a, L"--no-engine") == 0)   cfg.enableEngine   = false;
        else if (wcsncmp(a, L"--log=", 6) == 0)    cfg.logPath = a + 6;
    }
    LocalFree(argv);
}

// ---------------------------------------------------------------------------
// D3D9Ex device + shared-handle render-target texture
// ---------------------------------------------------------------------------

bool InitD3D9Ex(HWND hwnd) {
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, g_d3d9.GetAddressOf());
    if (FAILED(hr) || !g_d3d9) {
        LogDbg("[SPIKE-ERROR] Direct3DCreate9Ex failed hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] Direct3DCreate9Ex OK\n");

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed              = TRUE;
    pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat      = D3DFMT_A8R8G8B8;
    pp.BackBufferWidth       = 16;   // dummy — we render to shared RT, not back buffer
    pp.BackBufferHeight      = 16;
    pp.hDeviceWindow         = hwnd;
    pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    hr = g_d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &pp, nullptr, g_d3d9Device.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE] CreateDeviceEx HWVP failed hr=0x%08lX, trying SOFTWARE_VP\n", hr);
        hr = g_d3d9->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &pp, nullptr, g_d3d9Device.GetAddressOf());
    }
    if (FAILED(hr) || !g_d3d9Device) {
        LogDbg("[SPIKE-ERROR] CreateDeviceEx failed all paths hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] D3D9Ex device created OK\n");

    // GPU adapter LUID — useful for diagnosing multi-GPU mismatch.
    D3DADAPTER_IDENTIFIER9 ident = {};
    g_d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    LogDbg("[SPIKE] D3D9 adapter: %s (VendorId=0x%lX DeviceId=0x%lX)\n",
           ident.Description, ident.VendorId, ident.DeviceId);

    // Create the SHARED-HANDLE render-target texture. The shared handle is
    // populated as an NT handle by D3D9Ex when the last param is non-null
    // AND the device is D3D9Ex (not vanilla D3D9). D3DFMT_A8R8G8B8 maps to
    // DXGI_FORMAT_B8G8R8A8_UNORM on the D3D11 side (BGRA byte order).
    g_sharedHandle = nullptr;
    hr = g_d3d9Device->CreateTexture(
        static_cast<UINT>(g_cfg.width),
        static_cast<UINT>(g_cfg.height),
        1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
        g_sharedTexD3D9.GetAddressOf(),
        &g_sharedHandle);
    if (FAILED(hr) || !g_sharedTexD3D9 || !g_sharedHandle) {
        LogDbg("[SPIKE-ERROR] D3D9Ex CreateTexture(shared) failed hr=0x%08lX handle=%p\n",
               hr, g_sharedHandle);
        return false;
    }
    LogDbg("[SPIKE] shared texture created: %dx%d A8R8G8B8 handle=%p\n",
           g_cfg.width, g_cfg.height, g_sharedHandle);

    hr = g_sharedTexD3D9->GetSurfaceLevel(0, g_sharedSurfD3D9.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] GetSurfaceLevel(0) failed hr=0x%08lX\n", hr);
        return false;
    }

    // GPU-fence-ish: an event query lets us flush + wait without the heavier
    // ID3D11Fence cross-device infrastructure. The D3D9 driver guarantees
    // that when the query reports SIGNALED, the texture's backing VRAM is
    // safe for the D3D11 side to read.
    hr = g_d3d9Device->CreateQuery(D3DQUERYTYPE_EVENT, g_d3d9SyncQuery.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-WARN] CreateQuery(EVENT) failed hr=0x%08lX — sync degraded\n", hr);
    }

    return true;
}

// ---------------------------------------------------------------------------
// D3D11 device + DXGI factory + swapchain for composition
// ---------------------------------------------------------------------------

bool InitD3D11AndSwapchain() {
    // Try with the debug layer first; fall back if SDK layers aren't installed.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL wanted[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        wanted, _countof(wanted), D3D11_SDK_VERSION,
        g_d3d11Device.GetAddressOf(), &chosenLevel, g_d3d11Context.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-WARN] D3D11CreateDevice with DEBUG failed hr=0x%08lX, retrying without\n", hr);
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            wanted, _countof(wanted), D3D11_SDK_VERSION,
            g_d3d11Device.GetAddressOf(), &chosenLevel, g_d3d11Context.GetAddressOf());
    }
    if (FAILED(hr) || !g_d3d11Device) {
        LogDbg("[SPIKE-ERROR] D3D11CreateDevice failed all paths hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] D3D11 device created (level=0x%X flags=0x%X)\n",
           static_cast<unsigned>(chosenLevel), flags);

    // Adapter LUID — match check against D3D9Ex. If they differ we're on a
    // multi-GPU laptop with mismatched adapters and shared handles won't work.
    ComPtr<IDXGIDevice> dxgiDevice;
    hr = g_d3d11Device.As(&dxgiDevice);
    if (SUCCEEDED(hr)) {
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
            DXGI_ADAPTER_DESC desc = {};
            adapter->GetDesc(&desc);
            char descA[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, descA, 256, nullptr, nullptr);
            LogDbg("[SPIKE] D3D11 adapter: %s (LUID=%lX-%lX)\n",
                   descA,
                   static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                   static_cast<unsigned long>(desc.AdapterLuid.LowPart));
        }
    }

    // OpenSharedResource pulls the same VRAM that D3D9Ex's CreateTexture
    // allocated. The handle MUST be the NT handle from CreateTexture's
    // out-param (g_sharedHandle); passing a different handle silently
    // returns a different texture.
    hr = g_d3d11Device->OpenSharedResource(
        g_sharedHandle, IID_PPV_ARGS(g_sharedTexD3D11.GetAddressOf()));
    if (FAILED(hr) || !g_sharedTexD3D11) {
        LogDbg("[SPIKE-ERROR] D3D11 OpenSharedResource failed hr=0x%08lX\n", hr);
        return false;
    }
    D3D11_TEXTURE2D_DESC sharedDesc = {};
    g_sharedTexD3D11->GetDesc(&sharedDesc);
    LogDbg("[SPIKE] D3D11 opened shared resource: %ux%u fmt=%u bind=0x%X share=0x%X\n",
           sharedDesc.Width, sharedDesc.Height, sharedDesc.Format,
           sharedDesc.BindFlags, sharedDesc.MiscFlags);

    // DXGI factory — need IDXGIFactory2 for CreateSwapChainForComposition.
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(g_dxgiFactory.GetAddressOf()));
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] CreateDXGIFactory2 failed hr=0x%08lX\n", hr);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.Width  = static_cast<UINT>(g_cfg.width);
    scDesc.Height = static_cast<UINT>(g_cfg.height);
    scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;       // matches D3DFMT_A8R8G8B8
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = 2;
    scDesc.SampleDesc.Count = 1;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scDesc.AlphaMode  = DXGI_ALPHA_MODE_PREMULTIPLIED; // engine pixels carry their own alpha
    scDesc.Scaling    = DXGI_SCALING_STRETCH;

    hr = g_dxgiFactory->CreateSwapChainForComposition(
        g_d3d11Device.Get(), &scDesc, nullptr, g_engineSwapChain.GetAddressOf());
    if (FAILED(hr) || !g_engineSwapChain) {
        LogDbg("[SPIKE-ERROR] CreateSwapChainForComposition failed hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] D3D11 composition swapchain created (%dx%d FLIP_SEQ premul)\n",
           g_cfg.width, g_cfg.height);

    // Cache the swapchain's back buffer once — composition swapchains keep
    // the same back-buffer object across frames in flip-model.
    hr = g_engineSwapChain->GetBuffer(0, IID_PPV_ARGS(g_engineBackBuffer.GetAddressOf()));
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] swapchain GetBuffer(0) failed hr=0x%08lX\n", hr);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// DComp device (V1 IDCompositionDevice — matches WebView2APISample)
// ---------------------------------------------------------------------------

bool InitDComp() {
    // V2 factory function, V1 IID — same shape the sample uses successfully
    // on this exact machine. v2 tried V2 IDCompositionDesktopDevice and
    // it still produced white; v3 reverted to V1 and didn't help either but
    // matches the known-good sample.
    HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(g_dcompDevice.GetAddressOf()));
    if (FAILED(hr) || !g_dcompDevice) {
        LogDbg("[SPIKE-ERROR] DCompositionCreateDevice2 failed hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] DComp V1 device created\n");
    return true;
}

// ---------------------------------------------------------------------------
// Build the visual tree (called ONLY when both swapchain + WebView2 are ready,
// per v3 lesson that early tree construction may have been the bug).
// ---------------------------------------------------------------------------

bool BuildVisualTree() {
    if (g_treeBuilt) return true;
    if (!g_dcompDevice) {
        LogDbg("[SPIKE-ERROR] BuildVisualTree called before DComp init\n");
        return false;
    }
    HRESULT hr;

    // Create target after device is ready and we know we have content to show.
    // Topmost=TRUE matches sample line 919.
    hr = g_dcompDevice->CreateTargetForHwnd(g_mainWnd, TRUE, g_dcompTarget.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] CreateTargetForHwnd failed hr=0x%08lX\n", hr);
        return false;
    }

    hr = g_dcompDevice->CreateVisual(g_rootVisual.GetAddressOf());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] CreateVisual(root) failed hr=0x%08lX\n", hr);
        return false;
    }
    hr = g_dcompTarget->SetRoot(g_rootVisual.Get());
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] SetRoot failed hr=0x%08lX\n", hr);
        return false;
    }

    // Engine visual — contains the D3D11 composition swapchain. Z-order:
    // adding this FIRST means subsequent children draw above it (we add the
    // WebView2 visual next, which should sit ABOVE the engine).
    if (g_cfg.enableEngine) {
        hr = g_dcompDevice->CreateVisual(g_engineVisual.GetAddressOf());
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] CreateVisual(engine) failed hr=0x%08lX\n", hr);
            return false;
        }
        hr = g_engineVisual->SetContent(g_engineSwapChain.Get());
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] engineVisual->SetContent(swapchain) failed hr=0x%08lX\n", hr);
            return false;
        }
        hr = g_rootVisual->AddVisual(g_engineVisual.Get(), TRUE, nullptr);
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] root->AddVisual(engine) failed hr=0x%08lX\n", hr);
            return false;
        }
        LogDbg("[SPIKE] engine visual attached (swapchain content)\n");
    }

    // WebView2 visual — attached above engine. WebView2's RootVisualTarget
    // becomes this visual's content via put_RootVisualTarget on the
    // composition controller.
    if (g_cfg.enableWebView2 && g_compositionController) {
        hr = g_dcompDevice->CreateVisual(g_webviewVisual.GetAddressOf());
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] CreateVisual(webview) failed hr=0x%08lX\n", hr);
            return false;
        }
        // AddVisual with insertAbove=TRUE places this above prior children
        // when referenceVisual is null (it's documented as "above all").
        // insertAbove=FALSE with referenceVisual=NULL puts this at the END
        // of the children list, which renders it IN FRONT of all siblings
        // (DComp draws children list-order, last-drawn-on-top). The MSDN
        // naming is counterintuitive: "insertAbove=TRUE + NULL ref" means
        // "behind all," NOT "above all" — bisected via --no-engine smoke.
        hr = g_rootVisual->AddVisual(g_webviewVisual.Get(), FALSE, nullptr);
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] root->AddVisual(webview) failed hr=0x%08lX\n", hr);
            return false;
        }
        hr = g_compositionController->put_RootVisualTarget(g_webviewVisual.Get());
        if (FAILED(hr)) {
            LogDbg("[SPIKE-ERROR] put_RootVisualTarget failed hr=0x%08lX\n", hr);
            return false;
        }
        LogDbg("[SPIKE] webview visual attached (RootVisualTarget set)\n");
    }

    hr = g_dcompDevice->Commit();
    if (FAILED(hr)) {
        LogDbg("[SPIKE-ERROR] DComp Commit failed hr=0x%08lX\n", hr);
        return false;
    }
    LogDbg("[SPIKE] DComp tree committed (engine=%d webview=%d)\n",
           static_cast<int>(g_cfg.enableEngine), static_cast<int>(g_cfg.enableWebView2));
    g_treeBuilt = true;
    return true;
}

// ---------------------------------------------------------------------------
// WebView2 init via composition controller. The completion handler defers
// to BuildVisualTree once both sides are ready.
// ---------------------------------------------------------------------------

const wchar_t* kOverlayHtml =
L"<!doctype html><html><head><meta charset=utf-8><title>spike</title>"
L"<style>"
L"html,body{margin:0;padding:0;background:transparent;font-family:Segoe UI,sans-serif;color:#fff;}"
L".bar{position:fixed;left:0;right:0;height:40px;background:rgba(20,20,30,0.85);"
L"display:flex;align-items:center;padding:0 20px;backdrop-filter:blur(8px);font-size:14px;}"
L".top{top:0;border-bottom:1px solid rgba(255,255,255,0.1);}"
L".bot{bottom:0;border-top:1px solid rgba(255,255,255,0.1);justify-content:flex-end;gap:12px;}"
L"#probe{background:#3b3b48;color:#fff;border:1px solid #6a6a78;padding:6px 14px;"
L"border-radius:4px;cursor:pointer;font-family:inherit;font-size:13px;}"
L"#probe:hover{background:#4a4a58;}"
L"#status{color:#aaa;font-size:12px;}"
L"</style></head><body>"
L"<div class='bar top'>DXGI Spike &mdash; WebView2 chrome over D3D11 engine visual</div>"
L"<div class='bar bot'>"
L"<span id=status>transparency probe: middle should show engine pixels</span>"
L"<button id=probe>click probe</button>"
L"</div>"
L"<script>"
L"let n=0;"
L"document.getElementById('probe').addEventListener('click',function(){"
L"  n++;"
L"  this.textContent='clicked '+n+' at '+new Date().toISOString().slice(11,19);"
L"});"
L"</script></body></html>";

void OnCompositionControllerReady(HRESULT hr, ICoreWebView2CompositionController* ctl) {
    if (FAILED(hr) || !ctl) {
        LogDbg("[SPIKE-ERROR] CompositionController completion hr=0x%08lX ctl=%p\n", hr, ctl);
        return;
    }
    g_compositionController = ctl;

    // QI to base ICoreWebView2Controller for put_Bounds + Close + DefaultBackgroundColor
    hr = ctl->QueryInterface(IID_PPV_ARGS(g_webController.GetAddressOf()));
    if (FAILED(hr) || !g_webController) {
        LogDbg("[SPIKE-ERROR] QI ICoreWebView2Controller hr=0x%08lX\n", hr);
        return;
    }

    // Transparent default background so the engine visual shows through where
    // the overlay HTML is transparent.
    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(g_webController.As(&ctrl2)) && ctrl2) {
        COREWEBVIEW2_COLOR transparent = {0, 0, 0, 0};   // A, R, G, B all zero
        ctrl2->put_DefaultBackgroundColor(transparent);
        LogDbg("[SPIKE] WebView2 default bg set to ARGB(0,0,0,0)\n");
    }

    // Bounds = full client rect. put_Bounds is inherited from
    // ICoreWebView2Controller even on the composition controller.
    RECT bounds;
    GetClientRect(g_mainWnd, &bounds);
    g_webController->put_Bounds(bounds);

    // Grab the underlying ICoreWebView2 for navigation.
    hr = g_webController->get_CoreWebView2(g_webview.GetAddressOf());
    if (FAILED(hr) || !g_webview) {
        LogDbg("[SPIKE-ERROR] get_CoreWebView2 hr=0x%08lX\n", hr);
        return;
    }

    g_webview->NavigateToString(kOverlayHtml);
    LogDbg("[SPIKE] overlay HTML navigation dispatched\n");

    g_webviewReady = true;

    // Now both sides exist → build the visual tree.
    if (!BuildVisualTree()) {
        LogDbg("[SPIKE-ERROR] BuildVisualTree after webview-ready FAILED\n");
    }
}

HRESULT InitWebView2() {
    if (!g_cfg.enableWebView2) {
        LogDbg("[SPIKE] WebView2 disabled via --no-webview2\n");
        // No webview, but we can still build the engine-only tree.
        BuildVisualTree();
        return S_OK;
    }

    // Per-PID user-data folder — back-to-back spike runs (or multiple
    // concurrent instances) otherwise collide on the WebView2 folder lock
    // and the second instance gets HRESULT_FROM_WIN32(ERROR_BUSY)=0x800700AA
    // from CreateCoreWebView2CompositionController. Observed during smoke
    // testing when a killed prior run's lock had not yet released.
    std::wstring userDataFolder;
    {
        wchar_t buf[MAX_PATH];
        GetTempPathW(MAX_PATH, buf);
        userDataFolder = buf;
        userDataFolder += L"DxgiSpikeWebView2Data_";
        wchar_t pidBuf[16];
        swprintf(pidBuf, _countof(pidBuf), L"%lu", GetCurrentProcessId());
        userDataFolder += pidBuf;
    }

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || !env) {
                    LogDbg("[SPIKE-ERROR] WebView2 env failed hr=0x%08lX\n", envHr);
                    return E_FAIL;
                }
                LogDbg("[SPIKE] WebView2 environment created\n");
                g_webviewEnv = env;

                // QI to Environment3 — the interface that has
                // CreateCoreWebView2CompositionController (confirmed in
                // SDK 1.0.3967.48 at WebView2.h:42610).
                ComPtr<ICoreWebView2Environment3> env3;
                HRESULT hr = env->QueryInterface(IID_PPV_ARGS(env3.GetAddressOf()));
                if (FAILED(hr) || !env3) {
                    LogDbg("[SPIKE-ERROR] QI ICoreWebView2Environment3 hr=0x%08lX (SDK too old?)\n", hr);
                    return hr;
                }

                hr = env3->CreateCoreWebView2CompositionController(
                    g_mainWnd,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [](HRESULT cHr, ICoreWebView2CompositionController* ctl) -> HRESULT {
                            OnCompositionControllerReady(cHr, ctl);
                            return S_OK;
                        }).Get());
                if (FAILED(hr)) {
                    LogDbg("[SPIKE-ERROR] CreateCoreWebView2CompositionController hr=0x%08lX\n", hr);
                }
                return S_OK;
            }).Get());
}

// ---------------------------------------------------------------------------
// Frame loop — D3D9 draw → sync → D3D11 copy → present
// ---------------------------------------------------------------------------

double QpcToMs(LARGE_INTEGER start, LARGE_INTEGER end) {
    return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0)
         / static_cast<double>(g_qpcFreq.QuadPart);
}

void RenderD3D9Frame() {
    if (!g_d3d9Device || !g_sharedSurfD3D9) return;

    // Time-varying colour — gives the user a visual proof of liveness.
    // Cycles full RGB at ~0.5 Hz.
    double t = static_cast<double>(g_frameCount) / 120.0;     // ~2s loop @ 60fps
    auto cyc = [](double phase) -> BYTE {
        double s = 0.5 + 0.5 * sin(phase);
        return static_cast<BYTE>(s * 255.0);
    };
    DWORD r = cyc(t * 6.28);
    DWORD g = cyc(t * 6.28 + 2.094);    // +120°
    DWORD b = cyc(t * 6.28 + 4.188);    // +240°

    g_d3d9Device->SetRenderTarget(0, g_sharedSurfD3D9.Get());
    g_d3d9Device->Clear(0, nullptr, D3DCLEAR_TARGET,
                        D3DCOLOR_ARGB(255, r, g, b), 1.0f, 0);
    g_d3d9Device->BeginScene();
    g_d3d9Device->EndScene();
    // No Present — we render INTO the shared texture, not a back buffer.
    // The query below ensures the GPU has finished before D3D11 reads.

    if (g_d3d9SyncQuery) {
        g_d3d9SyncQuery->Issue(D3DISSUE_END);
        // Spin until signaled; this is the cross-device sync cost.
        BOOL done = FALSE;
        int spins = 0;
        while (g_d3d9SyncQuery->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) {
            if (++spins > 100000) {
                LogDbg("[SPIKE-WARN] D3D9 sync query never signalled after 100k spins\n");
                break;
            }
        }
    }
}

void CompositeD3D11Frame() {
    if (!g_d3d11Context || !g_engineBackBuffer || !g_sharedTexD3D11 || !g_engineSwapChain) return;

    g_d3d11Context->CopyResource(g_engineBackBuffer.Get(), g_sharedTexD3D11.Get());

    DXGI_PRESENT_PARAMETERS pp = {};
    g_engineSwapChain->Present1(0, 0, &pp);
}

void RenderFrame() {
    if (!g_treeBuilt) return;   // tree not assembled yet; nothing to render

    LARGE_INTEGER t0, t1, t2, t3, t4;
    QueryPerformanceCounter(&t0);

    if (g_cfg.enableEngine) {
        RenderD3D9Frame();
        QueryPerformanceCounter(&t1);
        // Sync is inside RenderD3D9Frame — captured collectively
        t2 = t1;
        CompositeD3D11Frame();
        QueryPerformanceCounter(&t3);
    } else {
        // --no-engine: skip the engine path entirely (swapchain stays at its
        // default-cleared content). Useful for bisecting if the combined run
        // produces white.
        t1 = t0; t2 = t0; t3 = t0;
    }
    t4 = t3;

    double frameMs = QpcToMs(t0, t4);
    g_emaTotalMs = (g_emaTotalMs == 0.0) ? frameMs : (g_emaTotalMs * 0.95 + frameMs * 0.05);
    double fps = (frameMs > 0.0) ? (1000.0 / frameMs) : 0.0;
    g_emaFps = (g_emaFps == 0.0) ? fps : (g_emaFps * 0.95 + fps * 0.05);

    ++g_frameCount;

    // Per-frame log throttled: every 60 frames.
    if ((g_frameCount % 60) == 0) {
        LogDbg("[SPIKE] frame=%llu d3d9=%.2fms copy+present=%.2fms total=%.2fms emaFps=%.1f\n",
               static_cast<unsigned long long>(g_frameCount),
               QpcToMs(t0, t1),
               QpcToMs(t2, t3),
               frameMs, g_emaFps);
    }

    // Window-title FPS update — every ~250ms wall-clock.
    DWORD nowMs = GetTickCount();
    if (nowMs - g_lastTitleUpdateMs >= 250) {
        wchar_t title[128];
        // — = em-dash, escape form so the source survives MSVC's
        // source-charset interpretation regardless of BOM presence.
        // Literal em-dash gets re-encoded as UTF-8 bytes interpreted as
        // CP-1252 if the compiler doesn't see a BOM — produces "DXGI Spike â€"".
        swprintf(title, _countof(title),
                 L"DXGI Spike -- %.1f FPS @ %ls  (%dx%d  %.2fms)",
                 g_emaFps, g_cfg.resLabel, g_cfg.width, g_cfg.height, g_emaTotalMs);
        SetWindowTextW(g_mainWnd, title);
        g_lastTitleUpdateMs = nowMs;
    }
}

// ---------------------------------------------------------------------------
// Resize handling — defer for spike scope (production needs it; spike runs
// at fixed --w/--h to keep measurement variables low).
// ---------------------------------------------------------------------------

void OnResize() {
    // Only resize WebView2 bounds. Engine swapchain stays at the boot
    // resolution — this is intentional for the spike (we want stable perf
    // numbers at known sizes).
    if (g_webController) {
        RECT r;
        GetClientRect(g_mainWnd, &r);
        g_webController->put_Bounds(r);
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

void Shutdown() {
    LogDbg("[SPIKE] shutdown begin\n");

    if (g_webController) {
        g_webController->Close();
    }
    g_compositionController.Reset();
    g_webController.Reset();
    g_webview.Reset();
    g_webviewEnv.Reset();

    g_webviewVisual.Reset();
    g_engineVisual.Reset();
    g_rootVisual.Reset();
    g_dcompTarget.Reset();
    g_dcompDevice.Reset();

    g_engineBackBuffer.Reset();
    g_engineSwapChain.Reset();
    g_dxgiFactory.Reset();
    g_sharedTexD3D11.Reset();
    g_d3d11Context.Reset();
    g_d3d11Device.Reset();

    g_d3d9SyncQuery.Reset();
    g_sharedSurfD3D9.Reset();
    g_sharedTexD3D9.Reset();
    g_d3d9Device.Reset();
    g_d3d9.Reset();

    // Shared HANDLE is freed when the originating texture (D3D9) releases —
    // do NOT CloseHandle here, that'd be a double-free.
    g_sharedHandle = nullptr;

    LogDbg("[SPIKE] shutdown complete\n");
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        OnResize();
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        // DComp + swapchain own the surface; suppress GDI erase so we don't
        // flash to the class brush mid-frame.
        return 1;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ---------------------------------------------------------------------------
// WinMain — assemble: window → D3D9Ex → shared tex → D3D11 + swapchain →
// DComp → WebView2 → tree → render loop
// ---------------------------------------------------------------------------

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    ParseCommandLine(g_cfg);
    OpenLogFile(g_cfg.logPath);

    LogDbg("[SPIKE] config: %dx%d (%ls) webview2=%d engine=%d\n",
           g_cfg.width, g_cfg.height, g_cfg.resLabel,
           static_cast<int>(g_cfg.enableWebView2),
           static_cast<int>(g_cfg.enableEngine));

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&g_qpcFreq);

    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogDbg("[SPIKE] CoInitializeEx hr=0x%08lX\n", coHr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = nullptr;   // DComp + swapchain own pixels
    RegisterClassExW(&wc);

    // Window size = client size + non-client. AdjustWindowRect gives us the
    // outer dimensions for the requested client size.
    RECT rect = { 0, 0, g_cfg.width, g_cfg.height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    g_mainWnd = CreateWindowExW(
        0,
        kWindowClassName, L"DXGI Spike -- initializing",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rect.right - rect.left, rect.bottom - rect.top,
        nullptr, nullptr, hInst, nullptr);
    if (!g_mainWnd) {
        LogDbg("[SPIKE-FATAL] CreateWindow failed gle=%lu\n", GetLastError());
        CloseLogFile();
        return 1;
    }
    LogDbg("[SPIKE] host HWND created (no WS_EX_LAYERED)\n");

    if (!InitD3D9Ex(g_mainWnd))          { Shutdown(); CloseLogFile(); return 2; }
    if (!InitD3D11AndSwapchain())        { Shutdown(); CloseLogFile(); return 3; }
    if (!InitDComp())                    { Shutdown(); CloseLogFile(); return 4; }

    // WebView2 init kicks off async work; tree assembly happens in the
    // completion callback (or immediately if --no-webview2). After this
    // returns, the message pump must run to deliver the env + controller
    // completions.
    HRESULT wvHr = InitWebView2();
    if (FAILED(wvHr)) {
        LogDbg("[SPIKE-WARN] InitWebView2 returned hr=0x%08lX — continuing with engine-only path\n", wvHr);
        g_cfg.enableWebView2 = false;
        BuildVisualTree();
    }

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);

    // PeekMessage idle loop — drives WebView2 message dispatch + per-frame
    // rendering. Render only runs once the visual tree exists.
    MSG mmsg = {};
    while (mmsg.message != WM_QUIT) {
        if (PeekMessageW(&mmsg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&mmsg);
            DispatchMessageW(&mmsg);
        } else {
            RenderFrame();
            // Don't burn 100% CPU when there's nothing to render.
            if (!g_treeBuilt) Sleep(8);
        }
    }

    Shutdown();
    CoUninitialize();
    CloseLogFile();
    return static_cast<int>(mmsg.wParam);
}
