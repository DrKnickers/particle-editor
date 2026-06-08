// shared_texture_test.cpp — Phase 3 Stage 2c
// ====================================================
//
// Standalone CLI exe that verifies the D3D9Ex shared-handle pipeline
// end-to-end without involving the real Engine class or WebView2:
//
//   1. Direct3DCreate9Ex + CreateDeviceEx (same flags Engine uses post-
//      Stage 1: HARDWARE_VERTEXPROCESSING | MULTITHREADED).
//   2. CreateTexture(USAGE_RENDERTARGET, D3DPOOL_DEFAULT, &sharedHandle) —
//      the exact API call AlphaCompositor::Resize now makes.
//   3. SetRenderTarget(level0(sharedTex)); Clear(knownColor); EndScene;
//      Issue+Flush a D3D9 query event for cross-device GPU completion.
//   4. D3D11CreateDevice + OpenSharedResource(sharedHandle) — D3D11 side
//      sees the same VRAM the D3D9 side just cleared.
//   5. CopyResource into a D3D11 staging texture (D3D11_USAGE_STAGING +
//      CPU_ACCESS_READ) so we can Map and inspect from CPU.
//   6. Compare every pixel against the expected packed DWORD. Exit code
//      0 on bit-exact match, 1 on mismatch, 2 on any API/init failure.
//
// Stage 0's dxgi_spike already proved this pipeline can produce visible
// frames; this test specifically locks the BIT-EXACT contract: the
// pixels D3D9 wrote ARE the pixels D3D11 reads, byte-for-byte. That's
// the load-bearing invariant for Stage 4 — the DComp visual will show
// whatever D3D11 reads, and the engine writes via D3D9. If they don't
// match, the entire phase 3 architecture breaks.
//
// Usage:
//   shared_texture_test.exe [--w=N] [--h=N] [--color=0xAARRGGBB]
//
// Defaults: 256×256, color 0xFF0040FF (opaque orange-blue mix).
// Output: a single PASS/FAIL line on stdout + exit code.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <wrl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

struct Config {
    int   width  = 256;
    int   height = 256;
    DWORD color  = 0xFF0040FFu;  // ARGB: opaque, R=0, G=40h, B=FFh
};

bool ParseHex(const wchar_t* s, DWORD& out) {
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    wchar_t* end = nullptr;
    unsigned long long v = wcstoull(s, &end, 16);
    if (!end || *end != L'\0') return false;
    out = static_cast<DWORD>(v);
    return true;
}

Config ParseArgs() {
    Config cfg;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return cfg;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (wcsncmp(a, L"--w=", 4) == 0)        cfg.width  = static_cast<int>(_wtoi(a + 4));
        else if (wcsncmp(a, L"--h=", 4) == 0)   cfg.height = static_cast<int>(_wtoi(a + 4));
        else if (wcsncmp(a, L"--color=", 8) == 0) {
            DWORD c;
            if (ParseHex(a + 8, c)) cfg.color = c;
        }
    }
    LocalFree(argv);
    return cfg;
}

void Logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

} // namespace

int wmain(int /*argc*/, wchar_t** /*argv*/) {
    const Config cfg = ParseArgs();
    Logf("[Test] shared_texture_test %dx%d color=0x%08lX\n",
         cfg.width, cfg.height, static_cast<unsigned long>(cfg.color));

    // Hidden message-only window — D3D9 requires an HWND but we never
    // show it. HWND_MESSAGE puts the window in the message-only tree
    // (no taskbar, no z-order, no visible chrome).
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"shared_texture_test",
                                0, 0, 0, 1, 1,
                                HWND_MESSAGE, NULL,
                                GetModuleHandleW(NULL), NULL);
    if (!hwnd) {
        Logf("[Test-ERROR] CreateWindowExW failed gle=%lu\n", GetLastError());
        return 2;
    }

    // -- D3D9Ex side -----------------------------------------------------
    ComPtr<IDirect3D9Ex> d3d9;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, d3d9.GetAddressOf());
    if (FAILED(hr) || !d3d9) {
        Logf("[Test-ERROR] Direct3DCreate9Ex failed hr=0x%08lX\n", hr);
        return 2;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed              = TRUE;
    pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat      = D3DFMT_UNKNOWN;
    pp.BackBufferWidth       = 16;   // dummy; we render to the shared RT
    pp.BackBufferHeight      = 16;
    pp.hDeviceWindow         = hwnd;
    pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    ComPtr<IDirect3DDevice9Ex> d3d9Device;
    hr = d3d9->CreateDeviceEx(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
        &pp, nullptr, d3d9Device.GetAddressOf());
    if (FAILED(hr)) {
        Logf("[Test] HWVP failed (hr=0x%08lX), trying SOFTWARE_VP\n", hr);
        hr = d3d9->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &pp, nullptr, d3d9Device.GetAddressOf());
    }
    if (FAILED(hr) || !d3d9Device) {
        Logf("[Test-ERROR] CreateDeviceEx failed hr=0x%08lX\n", hr);
        return 2;
    }

    D3DADAPTER_IDENTIFIER9 ident = {};
    d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &ident);
    Logf("[Test] D3D9Ex adapter: %s (VendorId=0x%lX DeviceId=0x%lX)\n",
         ident.Description,
         static_cast<unsigned long>(ident.VendorId),
         static_cast<unsigned long>(ident.DeviceId));

    // Shared-handle render-target texture. Same call AlphaCompositor::
    // Resize now makes for the editor's offscreen RT.
    ComPtr<IDirect3DTexture9> sharedTex;
    HANDLE sharedHandle = nullptr;
    hr = d3d9Device->CreateTexture(
        static_cast<UINT>(cfg.width), static_cast<UINT>(cfg.height),
        1 /*levels*/, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
        D3DPOOL_DEFAULT, sharedTex.GetAddressOf(), &sharedHandle);
    if (FAILED(hr) || !sharedTex || !sharedHandle) {
        Logf("[Test-ERROR] CreateTexture(shared) failed hr=0x%08lX handle=%p\n",
             hr, sharedHandle);
        return 2;
    }
    Logf("[Test] shared tex created handle=%p\n", sharedHandle);

    ComPtr<IDirect3DSurface9> sharedSurf;
    hr = sharedTex->GetSurfaceLevel(0, sharedSurf.GetAddressOf());
    if (FAILED(hr)) {
        Logf("[Test-ERROR] GetSurfaceLevel(0) failed hr=0x%08lX\n", hr);
        return 2;
    }

    // Render: set the shared RT, clear to known colour, EndScene.
    d3d9Device->SetRenderTarget(0, sharedSurf.Get());
    d3d9Device->Clear(0, nullptr, D3DCLEAR_TARGET, cfg.color, 1.0f, 0);
    d3d9Device->BeginScene();
    d3d9Device->EndScene();

    // Cross-device GPU sync via event query — same pattern dxgi_spike uses.
    // Spins until D3D9 reports the Clear has hit the GPU, so the D3D11
    // side sees the cleared pixels rather than stale VRAM.
    ComPtr<IDirect3DQuery9> q;
    hr = d3d9Device->CreateQuery(D3DQUERYTYPE_EVENT, q.GetAddressOf());
    if (SUCCEEDED(hr)) {
        q->Issue(D3DISSUE_END);
        BOOL done = FALSE;
        int spins = 0;
        while (q->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE) {
            if (++spins > 100000) {
                Logf("[Test-WARN] D3D9 sync query never signalled after 100k spins\n");
                break;
            }
        }
    }

    // -- D3D11 side ------------------------------------------------------
    UINT d3d11Flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL chosenLevel = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> d3d11Device;
    ComPtr<ID3D11DeviceContext> d3d11Context;
    hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, d3d11Flags,
        nullptr, 0, D3D11_SDK_VERSION,
        d3d11Device.GetAddressOf(), &chosenLevel, d3d11Context.GetAddressOf());
    if (FAILED(hr) || !d3d11Device) {
        Logf("[Test-ERROR] D3D11CreateDevice failed hr=0x%08lX\n", hr);
        return 2;
    }
    Logf("[Test] D3D11 device created (level=0x%X)\n",
         static_cast<unsigned>(chosenLevel));

    // LUID match check — if D3D9 and D3D11 picked different adapters
    // (e.g. multi-GPU laptop on a discrete switching device) the shared
    // handle won't work and we should report that clearly.
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        if (SUCCEEDED(d3d11Device.As(&dxgiDevice))) {
            ComPtr<IDXGIAdapter> adapter;
            if (SUCCEEDED(dxgiDevice->GetAdapter(adapter.GetAddressOf()))) {
                DXGI_ADAPTER_DESC desc = {};
                adapter->GetDesc(&desc);
                char descA[256];
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                                    descA, sizeof(descA), nullptr, nullptr);
                Logf("[Test] D3D11 adapter: %s (LUID=%lX-%lX)\n",
                     descA,
                     static_cast<unsigned long>(desc.AdapterLuid.HighPart),
                     static_cast<unsigned long>(desc.AdapterLuid.LowPart));
            }
        }
    }

    ComPtr<ID3D11Texture2D> sharedTex11;
    hr = d3d11Device->OpenSharedResource(
        sharedHandle, IID_PPV_ARGS(sharedTex11.GetAddressOf()));
    if (FAILED(hr) || !sharedTex11) {
        Logf("[Test-ERROR] D3D11 OpenSharedResource failed hr=0x%08lX\n", hr);
        return 2;
    }

    D3D11_TEXTURE2D_DESC openedDesc = {};
    sharedTex11->GetDesc(&openedDesc);
    Logf("[Test] opened in D3D11: %ux%u fmt=%u bind=0x%X misc=0x%X\n",
         openedDesc.Width, openedDesc.Height, openedDesc.Format,
         openedDesc.BindFlags, openedDesc.MiscFlags);
    if (openedDesc.Width != static_cast<UINT>(cfg.width) ||
        openedDesc.Height != static_cast<UINT>(cfg.height)) {
        Logf("[Test-ERROR] Dimension mismatch: expected %dx%d got %ux%u\n",
             cfg.width, cfg.height, openedDesc.Width, openedDesc.Height);
        return 2;
    }

    // Staging copy for CPU readback. The opened resource is DEFAULT
    // (matches D3D9 side), so we can't Map it directly — copy to a
    // STAGING + CPU_ACCESS_READ texture first.
    D3D11_TEXTURE2D_DESC stagingDesc = openedDesc;
    stagingDesc.Usage           = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags       = 0;
    stagingDesc.CPUAccessFlags  = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags       = 0;

    ComPtr<ID3D11Texture2D> stagingTex;
    hr = d3d11Device->CreateTexture2D(&stagingDesc, nullptr, stagingTex.GetAddressOf());
    if (FAILED(hr) || !stagingTex) {
        Logf("[Test-ERROR] CreateTexture2D(staging) failed hr=0x%08lX\n", hr);
        return 2;
    }

    d3d11Context->CopyResource(stagingTex.Get(), sharedTex11.Get());
    d3d11Context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        Logf("[Test-ERROR] Map(staging) failed hr=0x%08lX\n", hr);
        return 2;
    }

    // Bit-exact compare. D3DFMT_A8R8G8B8 stores as BGRA bytes; D3DCOLOR
    // packs as 0xAARRGGBB which on little-endian x86 is also 0xBB 0xGG
    // 0xRR 0xAA in memory. D3D11's DXGI_FORMAT_B8G8R8A8_UNORM uses the
    // same memory layout, so reading as DWORD yields the original
    // D3DCOLOR value. Mismatch = the shared-handle contract is broken.
    int errors = 0;
    int firstErrX = -1, firstErrY = -1;
    DWORD firstErrGot = 0;
    for (int y = 0; y < cfg.height; ++y) {
        const DWORD* row = reinterpret_cast<const DWORD*>(
            static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch);
        for (int x = 0; x < cfg.width; ++x) {
            if (row[x] != cfg.color) {
                if (errors == 0) { firstErrX = x; firstErrY = y; firstErrGot = row[x]; }
                ++errors;
            }
        }
    }
    d3d11Context->Unmap(stagingTex.Get(), 0);

    if (errors == 0) {
        Logf("[Test] PASS: %dx%d bit-exact match on color 0x%08lX\n",
             cfg.width, cfg.height, static_cast<unsigned long>(cfg.color));
        return 0;
    } else {
        Logf("[Test] FAIL: %d / %d pixels mismatched, first at (%d,%d) "
             "expected 0x%08lX got 0x%08lX\n",
             errors, cfg.width * cfg.height, firstErrX, firstErrY,
             static_cast<unsigned long>(cfg.color),
             static_cast<unsigned long>(firstErrGot));
        return 1;
    }
}
