// Step-0 spike] Does a real game dome shader's precompiled technique
// VALIDATE + Begin() on the editor's D3D9Ex HAL device?
//
// The game's Skydome.fx / MeshAdditive.fx / MeshGloss.fx ship only DX8
// (vs_1_1/ps_1_1) + FIXEDFUNCTION LOD techniques -- no DX9. This spike creates
// a D3D9Ex HAL device the same way Engine does (Direct3DCreate9Ex +
// CreateDeviceEx, MULTITHREADED + HWVP, windowed), loads each .fxo passed on
// the command line via D3DXCreateEffect, and for each:
//   - enumerates techniques (name, LOD annotation, ValidateTechnique result)
//   - replicates Engine's Effect-ctor selection (most-advanced validating
//     technique whose LOD annotation matches DX9->DX8->DX8ATI->FIXEDFUNCTION)
//   - runs Begin() and reports the pass count.
//
// PASS = a technique is selected and Begin() yields >0 passes (dome renders).
// If only FIXEDFUNCTION validates, that's still a render path (the shaders ship
// it). NONE selected => blank-dome risk => re-scope trigger.
//
// This is a throwaway diagnostic (not a CI unit test); build via
// tests/build_spike_skydome_technique.bat. Usage:
//   spike_skydome_technique <shader.fxo> [more.fxo ...]

#include <windows.h>
#include <d3d9.h>
#include <d3dx9.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static std::vector<unsigned char> readFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0);
    std::vector<unsigned char> b((size_t)n);
    f.read((char*)b.data(), n);
    return b;
}

static const char* okfail(HRESULT hr) { return SUCCEEDED(hr) ? "OK" : "FAIL"; }

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: spike_skydome_technique <shader.fxo> [more...]\n"); return 2; }

    WNDCLASSW wc = {};
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SkySpikeWnd";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(L"SkySpikeWnd", L"spike", WS_OVERLAPPEDWINDOW,
                              0, 0, 64, 64, NULL, NULL, wc.hInstance, NULL);

    IDirect3D9Ex* d3d = NULL;
    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d)) || !d3d) {
        std::printf("Direct3DCreate9Ex failed\n"); return 1;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth       = 64;
    pp.BackBufferHeight      = 64;
    pp.BackBufferFormat      = D3DFMT_UNKNOWN;
    pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow         = hwnd;
    pp.Windowed              = TRUE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.PresentationInterval  = D3DPRESENT_INTERVAL_DEFAULT;

    IDirect3DDevice9Ex* dev = NULL;
    HRESULT dhr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, NULL, &dev);
    if (FAILED(dhr)) {
        dhr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_MULTITHREADED | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &dev);
    }
    if (FAILED(dhr) || !dev) { std::printf("CreateDeviceEx failed 0x%08lx\n", dhr); return 1; }

    D3DCAPS9 caps; dev->GetDeviceCaps(&caps);
    std::printf("Device caps: VertexShader vs_%d_%d  PixelShader ps_%d_%d\n",
        D3DSHADER_VERSION_MAJOR(caps.VertexShaderVersion), D3DSHADER_VERSION_MINOR(caps.VertexShaderVersion),
        D3DSHADER_VERSION_MAJOR(caps.PixelShaderVersion),  D3DSHADER_VERSION_MINOR(caps.PixelShaderVersion));

    static const char* LODs[] = { "DX9", "DX8", "DX8ATI", "FIXEDFUNCTION" };

    for (int a = 1; a < argc; ++a) {
        std::vector<unsigned char> bytes = readFile(argv[a]);
        std::printf("\n=== %s (%zu bytes) ===\n", argv[a], bytes.size());
        if (bytes.empty()) { std::printf("  could not read file\n"); continue; }

        ID3DXEffect* fx = NULL; ID3DXBuffer* err = NULL;
        HRESULT chr = D3DXCreateEffect(dev, bytes.data(), (UINT)bytes.size(),
                                       NULL, NULL, 0, NULL, &fx, &err);
        if (FAILED(chr) || !fx) {
            std::printf("  D3DXCreateEffect FAILED 0x%08lx %s\n", chr,
                        err ? (const char*)err->GetBufferPointer() : "");
            if (err) err->Release();
            continue;
        }
        if (err) err->Release();

        for (UINT i = 0; ; ++i) {
            D3DXHANDLE h = fx->GetTechnique(i);
            if (!h) break;
            D3DXTECHNIQUE_DESC td; fx->GetTechniqueDesc(h, &td);
            const char* lod = NULL;
            D3DXHANDLE la = fx->GetAnnotationByName(h, "LOD");
            if (la) fx->GetString(la, &lod);
            HRESULT vhr = fx->ValidateTechnique(h);
            std::printf("  tech[%u] %-22s LOD=%-13s validate=%s\n",
                        i, td.Name ? td.Name : "?", lod ? lod : "(none)", okfail(vhr));
        }

        // Replicate Engine's Effect-ctor selection.
        D3DXHANDLE sel = NULL; const char* selLod = NULL;
        for (int lod = 0; lod < 4 && !sel; ++lod) {
            for (UINT i = 0; ; ++i) {
                D3DXHANDLE h = fx->GetTechnique(i);
                if (!h) break;
                if (SUCCEEDED(fx->ValidateTechnique(h))) {
                    const char* v = NULL;
                    D3DXHANDLE la = fx->GetAnnotationByName(h, "LOD");
                    if (la) fx->GetString(la, &v);
                    if (v && std::strcmp(v, LODs[lod]) == 0) { sel = h; selLod = LODs[lod]; break; }
                }
            }
        }
        if (sel) {
            fx->SetTechnique(sel);
            D3DXTECHNIQUE_DESC td; fx->GetTechniqueDesc(sel, &td);
            UINT passes = 0; HRESULT bhr = fx->Begin(&passes, 0);
            std::printf("  -> selects %s (LOD=%s); Begin=%s passes=%u  ==> %s\n",
                        td.Name ? td.Name : "?", selLod, okfail(bhr), passes,
                        (SUCCEEDED(bhr) && passes > 0) ? "RENDERS" : "NO-RENDER");
            if (SUCCEEDED(bhr)) fx->End();
        } else {
            std::printf("  -> selects NONE -> blank-dome risk (re-scope trigger)\n");
        }
        fx->Release();
    }

    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
