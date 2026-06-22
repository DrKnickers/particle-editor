// cdp_split_spike.cpp — process-split feasibility spike
// ======================================================
//
// Scope: docs/experiments/process-split-feasibility-spike-scope.md (PR #295).
// Resolves the CRUX the scope named: can a D3D9-rendered frame produced in a
// SEPARATE process be shared into ANOTHER process's D3D11 for compositing?
//
// The CDP↔viewport probe (#294) proved single-process CDP+viewport can't coexist
// (CDP attach blacks the in-process D3D9 RT). The only path left is a process
// split: engine (D3D9) in its own non-CDP process, UI (WebView2/CDP) in another,
// sharing the rendered surface. Sub-claim "CDP tolerance" is already evidenced
// (non-CDP D3D9 renders fine via --capture; the live editor composites the React
// UI fine under --test-host). The UNKNOWN is the cross-process SHARE — this spike.
//
// Two modes of one exe (launch twice; same session):
//
//   cdp_split_spike.exe --engine [--w N --h N --color 0xAARRGGBB]
//     1. Direct3DCreate9Ex + CreateDeviceEx (Engine's flags), render a known
//        colour to a D3D9 shared RT (the AlphaCompositor idiom).
//     2. D3D11CreateDevice (same process), OpenSharedResource(d3d9 legacy handle)
//        -> D3D11 alias of the D3D9 RT. (The PROVEN in-process hop.)
//     3. Create a D3D11 texture with NT-handle + keyed-mutex sharing, CopyResource
//        the alias into it, and CreateSharedHandle it under a NAME. (The new bit —
//        a genuine cross-process DXGI surface, the scope's predicted "extra hop".)
//     4. Signal a named "ready" event; hold everything alive until a "done" event
//        (or a 30s timeout) so the UI process can open it.
//
//   cdp_split_spike.exe --ui [--out path.bmp]
//     1. D3D11CreateDevice; QI ID3D11Device1.
//     2. Wait the named "ready" event (engine created the texture).
//     3. OpenSharedResourceByName(name) -> the SAME VRAM the engine's D3D9 wrote.
//     4. Keyed-mutex acquire, CopyResource to a STAGING texture, readback, Release.
//     5. Assert the pixels are NON-BLACK and match the engine's colour; dump a BMP
//        for visual confirmation; signal "done". PASS => cross-process share works.
//
// Exit codes: 0 PASS · 1 FAIL (mismatch/black) · 2 API/init failure · 3 timeout.
//
// Forked from shared_texture_test.cpp (the single-process D3D9Ex->D3D11 readback).
// Standalone vcxproj; build with: msbuild src/host/spike/cdp_split_spike.vcxproj.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_1.h>   // ID3D11Device1::OpenSharedResourceByName
#include <dxgi1_2.h>   // IDXGIResource1::CreateSharedHandle

#include <wrl.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

// Per-session named kernel objects shared by the two processes. "Local\\" =
// current-session namespace (no privilege needed, unlike "Global\\").
constexpr wchar_t kTexName[]   = L"Local\\AloSpikeSharedTex";
constexpr wchar_t kReadyName[] = L"Local\\AloSpikeReady";
constexpr wchar_t kDoneName[]  = L"Local\\AloSpikeDone";

// Keyed-mutex keys: producer Acquire(kKeyEngine)/Release(kKeyUi);
// consumer Acquire(kKeyUi)/Release(kKeyEngine). Clean single-frame handoff.
constexpr UINT64 kKeyEngine = 0;
constexpr UINT64 kKeyUi     = 1;

struct Args {
    bool  isEngine = false;
    bool  isUi     = false;
    int   width    = 512;
    int   height   = 512;
    DWORD color    = 0xFF3A6EA5u;  // ARGB: opaque blue (same as the #294 probe)
    std::wstring out = L"cdp_split_spike.bmp";
};

void Logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
}

bool ParseHex(const wchar_t* s, DWORD& out) {
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    wchar_t* end = nullptr;
    unsigned long long v = wcstoull(s, &end, 16);
    if (!end || *end != L'\0') return false;
    out = static_cast<DWORD>(v);
    return true;
}

Args ParseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return a;
    for (int i = 1; i < argc; ++i) {
        const wchar_t* s = argv[i];
        if      (wcscmp(s, L"--engine") == 0) a.isEngine = true;
        else if (wcscmp(s, L"--ui") == 0)     a.isUi = true;
        else if (wcscmp(s, L"--w") == 0 && i + 1 < argc)     a.width  = _wtoi(argv[++i]);
        else if (wcscmp(s, L"--h") == 0 && i + 1 < argc)     a.height = _wtoi(argv[++i]);
        else if (wcscmp(s, L"--color") == 0 && i + 1 < argc) { DWORD c; if (ParseHex(argv[++i], c)) a.color = c; }
        else if (wcscmp(s, L"--out") == 0 && i + 1 < argc)   a.out = argv[++i];
    }
    LocalFree(argv);
    return a;
}

// Write a 32bpp top-down BGRA BMP. D3D11 BGRA byte order == BMP 32bpp byte order,
// so we can copy rows straight across (honouring the source row pitch).
bool WriteBmp(const std::wstring& path, const uint8_t* bgra, int w, int h, int srcPitch) {
    const int dstPitch = w * 4;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    fh.bfType    = 0x4D42;  // 'BM'
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + static_cast<DWORD>(dstPitch) * h;
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = -h;   // negative => top-down
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;

    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, &fh, sizeof(fh), &wrote, nullptr) &&
              WriteFile(f, &ih, sizeof(ih), &wrote, nullptr);
    for (int y = 0; ok && y < h; ++y)
        ok = WriteFile(f, bgra + static_cast<size_t>(y) * srcPitch, dstPitch, &wrote, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

// Create a D3D11 hardware device with BGRA support (matches the host compositor).
HRESULT MakeD3D11(ComPtr<ID3D11Device>& dev, ComPtr<ID3D11DeviceContext>& ctx) {
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                             D3D11_SDK_VERSION, dev.GetAddressOf(), &fl,
                             ctx.GetAddressOf());
}

void LogAdapterLuid(const char* tag, ID3D11Device* dev) {
    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) return;
    ComPtr<IDXGIAdapter> ad;
    if (FAILED(dxgiDev->GetAdapter(ad.GetAddressOf()))) return;
    DXGI_ADAPTER_DESC d = {};
    ad->GetDesc(&d);
    char desc[256];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, desc, sizeof(desc), nullptr, nullptr);
    Logf("[%s] D3D11 adapter: %s (LUID=%lX-%lX)\n", tag, desc,
         (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart);
}

// ---- ENGINE process -------------------------------------------------------
int RunEngine(const Args& a) {
    Logf("[engine] start %dx%d color=0x%08lX\n", a.width, a.height, (unsigned long)a.color);

    // Create the sync events FIRST and RESET them, so a stale signalled state
    // left by a prior/overlapping engine can't let a fresh --ui proceed against
    // the wrong texture (a false PASS). NOTE: this spike assumes ONE engine at a
    // time — don't leave a stale engine holding while starting another run.
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, kReadyName);
    HANDLE done  = CreateEventW(nullptr, TRUE, FALSE, kDoneName);
    if (ready) ResetEvent(ready);
    if (done)  ResetEvent(done);

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"cdp_split_spike_engine", 0, 0, 0, 1, 1,
                                HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) { Logf("[engine-ERROR] CreateWindowExW gle=%lu\n", GetLastError()); return 2; }

    // -- D3D9Ex: render a known colour to a shared RT (the AlphaCompositor idiom) --
    ComPtr<IDirect3D9Ex> d3d9;
    HRESULT hr = Direct3DCreate9Ex(D3D_SDK_VERSION, d3d9.GetAddressOf());
    if (FAILED(hr)) { Logf("[engine-ERROR] Direct3DCreate9Ex 0x%08lX\n", hr); return 2; }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN; pp.BackBufferWidth = 16; pp.BackBufferHeight = 16;
    pp.hDeviceWindow = hwnd; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    ComPtr<IDirect3DDevice9Ex> d9dev;
    hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                              D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                              &pp, nullptr, d9dev.GetAddressOf());
    if (FAILED(hr))
        hr = d3d9->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                  D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                  &pp, nullptr, d9dev.GetAddressOf());
    if (FAILED(hr)) { Logf("[engine-ERROR] CreateDeviceEx 0x%08lX\n", hr); return 2; }

    ComPtr<IDirect3DTexture9> d9tex;
    HANDLE d9legacyHandle = nullptr;
    hr = d9dev->CreateTexture(a.width, a.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                              D3DPOOL_DEFAULT, d9tex.GetAddressOf(), &d9legacyHandle);
    if (FAILED(hr)) { Logf("[engine-ERROR] CreateTexture(shared RT) 0x%08lX\n", hr); return 2; }
    Logf("[engine] D3D9 shared RT created, legacy handle=%p\n", d9legacyHandle);

    ComPtr<IDirect3DSurface9> d9surf;
    d9tex->GetSurfaceLevel(0, d9surf.GetAddressOf());
    d9dev->SetRenderTarget(0, d9surf.Get());
    d9dev->Clear(0, nullptr, D3DCLEAR_TARGET, a.color, 1.0f, 0);
    d9dev->BeginScene();
    d9dev->EndScene();

    // Cross-device GPU sync: spin the D3D9 event query so the Clear has hit VRAM
    // before D3D11 reads it (same pattern as shared_texture_test).
    ComPtr<IDirect3DQuery9> q;
    if (SUCCEEDED(d9dev->CreateQuery(D3DQUERYTYPE_EVENT, q.GetAddressOf()))) {
        q->Issue(D3DISSUE_END);
        BOOL done = FALSE; int spins = 0;
        while (q->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE)
            if (++spins > 100000) { Logf("[engine-WARN] D3D9 sync never signalled\n"); break; }
    }

    // -- D3D11 (same process): open the D3D9 RT (proven in-proc hop) --
    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(MakeD3D11(dev, ctx))) { Logf("[engine-ERROR] D3D11CreateDevice\n"); return 2; }
    LogAdapterLuid("engine", dev.Get());

    ComPtr<ID3D11Texture2D> d11alias;
    hr = dev->OpenSharedResource(d9legacyHandle, IID_PPV_ARGS(d11alias.GetAddressOf()));
    if (FAILED(hr)) { Logf("[engine-ERROR] in-proc OpenSharedResource(d3d9) 0x%08lX\n", hr); return 2; }
    D3D11_TEXTURE2D_DESC aliasDesc = {};
    d11alias->GetDesc(&aliasDesc);
    Logf("[engine] D3D9 RT opened in D3D11: %ux%u fmt=%u\n",
         aliasDesc.Width, aliasDesc.Height, aliasDesc.Format);

    // -- Cross-process NT-handle + keyed-mutex texture (the new bit) --
    D3D11_TEXTURE2D_DESC ntDesc = aliasDesc;
    ntDesc.Usage          = D3D11_USAGE_DEFAULT;
    ntDesc.BindFlags      = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ntDesc.CPUAccessFlags = 0;
    ntDesc.MiscFlags      = D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ComPtr<ID3D11Texture2D> ntTex;
    hr = dev->CreateTexture2D(&ntDesc, nullptr, ntTex.GetAddressOf());
    if (FAILED(hr)) { Logf("[engine-ERROR] CreateTexture2D(NT shared) 0x%08lX\n", hr); return 2; }

    ComPtr<IDXGIKeyedMutex> km;
    if (FAILED(ntTex.As(&km))) { Logf("[engine-ERROR] QI IDXGIKeyedMutex\n"); return 2; }

    // Copy the engine's rendered frame into the cross-process texture.
    hr = km->AcquireSync(kKeyEngine, 5000);
    if (hr != S_OK) { Logf("[engine-ERROR] AcquireSync(engine) 0x%08lX\n", hr); return 3; }
    ctx->CopyResource(ntTex.Get(), d11alias.Get());
    ctx->Flush();
    km->ReleaseSync(kKeyUi);   // hand ownership to the UI process

    // Publish the texture under a name so the UI process can open it.
    ComPtr<IDXGIResource1> res1;
    if (FAILED(ntTex.As(&res1))) { Logf("[engine-ERROR] QI IDXGIResource1\n"); return 2; }
    HANDLE ntHandle = nullptr;
    hr = res1->CreateSharedHandle(nullptr,
                                  DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                  kTexName, &ntHandle);
    if (FAILED(hr)) { Logf("[engine-ERROR] CreateSharedHandle(named) 0x%08lX\n", hr); return 2; }
    Logf("[engine] published NT shared handle %p under name %ls\n", ntHandle, kTexName);

    // Signal ready; hold everything alive until the UI signals done (or 30s).
    if (ready) SetEvent(ready);
    Logf("[engine] ready signalled; holding texture alive (<=30s)\n");
    DWORD w = done ? WaitForSingleObject(done, 30000) : WAIT_FAILED;
    Logf("[engine] %s; exiting\n", w == WAIT_OBJECT_0 ? "UI signalled done" : "timeout/no-done");

    if (ntHandle) CloseHandle(ntHandle);  // NT handle: independently closed (cross-proc kind)
    if (ready) CloseHandle(ready);
    if (done)  CloseHandle(done);
    return 0;
}

// ---- UI process -----------------------------------------------------------
int RunUi(const Args& a) {
    Logf("[ui] start\n");

    ComPtr<ID3D11Device> dev; ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(MakeD3D11(dev, ctx))) { Logf("[ui-ERROR] D3D11CreateDevice\n"); return 2; }
    LogAdapterLuid("ui", dev.Get());
    ComPtr<ID3D11Device1> dev1;
    if (FAILED(dev.As(&dev1))) { Logf("[ui-ERROR] QI ID3D11Device1\n"); return 2; }

    // Wait for the engine to publish the texture.
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, kReadyName);
    if (!ready) { Logf("[ui-ERROR] CreateEvent(ready) gle=%lu\n", GetLastError()); return 2; }
    if (WaitForSingleObject(ready, 15000) != WAIT_OBJECT_0) {
        Logf("[ui-ERROR] engine never signalled ready within 15s (engine not running?)\n");
        CloseHandle(ready);
        return 3;
    }

    // Open the engine's texture BY NAME — the cross-process surface share.
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = dev1->OpenSharedResourceByName(kTexName, DXGI_SHARED_RESOURCE_READ,
                                                IID_PPV_ARGS(tex.GetAddressOf()));
    if (FAILED(hr)) {
        Logf("[ui-ERROR] OpenSharedResourceByName 0x%08lX -- cross-process share FAILED\n", hr);
        CloseHandle(ready);
        return 1;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    tex->GetDesc(&desc);
    Logf("[ui] opened shared tex by name: %ux%u fmt=%u\n", desc.Width, desc.Height, desc.Format);

    // Sanity-check the opened surface matches what the engine should have
    // published. Catches opening a STALE/wrong texture from an overlapping run
    // (would otherwise sample a valid-but-wrong surface and risk a false PASS).
    if (desc.Width != static_cast<UINT>(a.width) || desc.Height != static_cast<UINT>(a.height) ||
        desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        Logf("[ui-ERROR] opened surface %ux%u fmt=%u != expected %dx%d fmt=%u "
             "(stale/wrong texture?)\n", desc.Width, desc.Height, desc.Format,
             a.width, a.height, (unsigned)DXGI_FORMAT_B8G8R8A8_UNORM);
        CloseHandle(ready);
        return 1;
    }

    ComPtr<IDXGIKeyedMutex> km;
    if (FAILED(tex.As(&km))) { Logf("[ui-ERROR] QI IDXGIKeyedMutex\n"); CloseHandle(ready); return 2; }
    hr = km->AcquireSync(kKeyUi, 5000);
    if (hr != S_OK) { Logf("[ui-ERROR] AcquireSync(ui) 0x%08lX\n", hr); CloseHandle(ready); return 3; }

    // Staging copy for CPU readback.
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    hr = dev->CreateTexture2D(&sd, nullptr, staging.GetAddressOf());
    if (FAILED(hr)) { Logf("[ui-ERROR] CreateTexture2D(staging) 0x%08lX\n", hr); km->ReleaseSync(kKeyEngine); CloseHandle(ready); return 2; }
    ctx->CopyResource(staging.Get(), tex.Get());
    ctx->Flush();

    D3D11_MAPPED_SUBRESOURCE m = {};
    hr = ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(hr) || !m.pData) { Logf("[ui-ERROR] Map 0x%08lX\n", hr); km->ReleaseSync(kKeyEngine); CloseHandle(ready); return 2; }

    // Sample center + the four corners; assert non-black and == engine colour.
    auto px = [&](int x, int y) -> DWORD {
        return *reinterpret_cast<const DWORD*>(
            static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch + x * 4);
    };
    const int W = static_cast<int>(desc.Width), H = static_cast<int>(desc.Height);
    DWORD center = px(W / 2, H / 2);
    DWORD samples[5] = { center, px(1,1), px(W-2,1), px(1,H-2), px(W-2,H-2) };
    int mism = 0, black = 0;
    for (DWORD s : samples) {
        if (s != a.color) ++mism;
        if ((s & 0x00FFFFFFu) <= 0x010101u) ++black;  // near-black (alpha-agnostic)
    }
    Logf("[ui] sampled center=0x%08lX (expected 0x%08lX); mismatches=%d near-black=%d\n",
         (unsigned long)center, (unsigned long)a.color, mism, black);

    // BMP dump (whole RT) for visual confirmation.
    bool bmpOk = WriteBmp(a.out, static_cast<const uint8_t*>(m.pData), W, H, static_cast<int>(m.RowPitch));
    ctx->Unmap(staging.Get(), 0);
    km->ReleaseSync(kKeyEngine);

    // Tell the engine it can exit.
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, kDoneName);
    if (done) { SetEvent(done); CloseHandle(done); }
    CloseHandle(ready);

    char outA[512]; WideCharToMultiByte(CP_UTF8, 0, a.out.c_str(), -1, outA, sizeof(outA), nullptr, nullptr);
    Logf("[ui] BMP %s -> %s\n", bmpOk ? "wrote" : "FAILED", outA);

    if (black == 5) { Logf("[ui] FAIL: shared surface is BLACK across all samples\n"); return 1; }
    if (mism != 0)  {
        // Expected value is THIS process's --color (default 0x%08lX). Engine and
        // UI parse --color independently, so a real share can still "mismatch"
        // here if the two invocations were given different colours.
        Logf("[ui] FAIL: %d/5 samples != expected 0x%08lX -- if the share worked, "
             "check both --engine and --ui were given the SAME --color\n",
             mism, (unsigned long)a.color);
        return 1;
    }
    Logf("[ui] PASS: cross-process share works -- UI read the engine's exact colour\n");
    return 0;
}

} // namespace

int wmain() {
    Args a = ParseArgs();
    if (a.isEngine == a.isUi) {  // neither or both
        Logf("usage: cdp_split_spike.exe (--engine | --ui) [--w N --h N --color 0xAARRGGBB --out f.bmp]\n");
        return 2;
    }
    return a.isEngine ? RunEngine(a) : RunUi(a);
}
