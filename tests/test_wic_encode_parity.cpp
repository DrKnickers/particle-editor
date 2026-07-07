// [R3b] Pixel-parity + format pin for the WIC PNG encoder
// (src/host/WicEncode.cpp) against the GDI+ encoder it replaces in
// AsyncFrameEncoder (src/host/WindowCapture.cpp EncodeBgraToPng).
//
// Pins (tasks/2026-07-07-perf-followups-plan.md §5):
//  1. Same deterministic BGRA buffer through BOTH encoders -> decoded RGB is
//     byte-identical per pixel (PNG is lossless; only compression may differ).
//     Odd dimensions (257x131) so a stride/rounding bug can't hide.
//  2. The WIC output's IHDR stays inside what scripts/wiki-media/build.mjs's
//     raw PNG-chunk parser supports: bit depth 8, color type 0/2/6,
//     non-interlaced.
//  3. Alpha is dropped identically (X8 in -> opaque out) on both paths.

#include "../src/host/WicEncode.h"
#include "../src/host/WindowCapture.h"

#include <windows.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

static int g_fail = 0;
static void ok(bool c, const char* m)
{
    printf(c ? "  ok: %s\n" : "  FAIL: %s\n", m);
    if (!c) ++g_fail;
}

// Decode any PNG to 32bppBGRA via WIC (independent of both encoders' choices).
static bool DecodePngBgra(const std::wstring& path, int& w, int& h,
                          std::vector<unsigned char>& out)
{
    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(fac->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &dec)))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(fac->CreateFormatConverter(&conv))) return false;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw = 0, uh = 0;
    if (FAILED(conv->GetSize(&uw, &uh))) return false;
    w = static_cast<int>(uw);
    h = static_cast<int>(uh);
    out.resize(static_cast<size_t>(w) * h * 4);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4,
                                      static_cast<UINT>(out.size()), out.data()));
}

int main()
{
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Gdiplus::GdiplusStartupInput gdiIn;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gdiIn, nullptr);

    // Deterministic BGRA test card: gradients, hard edges, and a GARBAGE
    // alpha channel (the record grab leaves X8 undefined — both encoders
    // must ignore it identically).
    const int W = 257, H = 131;
    std::vector<unsigned char> bgra(static_cast<size_t>(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            unsigned char* p = &bgra[(static_cast<size_t>(y) * W + x) * 4];
            p[0] = static_cast<unsigned char>(x);            // B gradient
            p[1] = static_cast<unsigned char>(y * 2);        // G gradient
            p[2] = static_cast<unsigned char>((x ^ y));      // R xor texture
            p[3] = static_cast<unsigned char>(x * 7 + y);    // garbage alpha
        }
    // hard vertical edge (filter-choice stressor)
    for (int y = 0; y < H; ++y)
    {
        unsigned char* p = &bgra[(static_cast<size_t>(y) * W + W / 2) * 4];
        p[0] = 255; p[1] = 0; p[2] = 255;
    }

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring pGdi = std::wstring(tmp) + L"wicparity-gdi.png";
    const std::wstring pWic = std::wstring(tmp) + L"wicparity-wic.png";

    ok(host::EncodeBgraToPng(bgra.data(), W, H, pGdi), "GDI+ encode succeeds");
    ok(host::EncodeBgraToPngWic(bgra.data(), W, H, pWic), "WIC encode succeeds");

    // --- decoded pixel parity ---
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    std::vector<unsigned char> d1, d2;
    ok(DecodePngBgra(pGdi, w1, h1, d1), "GDI+ png decodes");
    ok(DecodePngBgra(pWic, w2, h2, d2), "WIC png decodes");
    ok(w1 == W && h1 == H && w2 == W && h2 == H, "both decode at source size");
    if (d1.size() == d2.size() && !d1.empty())
    {
        size_t rgbDiff = 0, alphaNotOpaque = 0;
        for (size_t i = 0; i < d1.size(); i += 4)
        {
            if (d1[i] != d2[i] || d1[i + 1] != d2[i + 1] || d1[i + 2] != d2[i + 2])
                ++rgbDiff;
            if (d1[i + 3] != 255 || d2[i + 3] != 255)
                ++alphaNotOpaque;
        }
        ok(rgbDiff == 0, "decoded RGB byte-identical across encoders");
        ok(alphaNotOpaque == 0, "alpha dropped to opaque on BOTH paths");
    }
    else
    {
        ok(false, "decoded buffers comparable");
    }

    // --- IHDR pins on the WIC output (wiki-media raw-parser envelope) ---
    {
        FILE* f = _wfopen(pWic.c_str(), L"rb");
        ok(f != nullptr, "WIC png readable");
        if (f)
        {
            unsigned char head[33] = {};
            fread(head, 1, sizeof(head), f);
            fclose(f);
            static const unsigned char sig[8] = {0x89,'P','N','G','\r','\n',0x1A,'\n'};
            ok(std::memcmp(head, sig, 8) == 0, "PNG signature");
            ok(std::memcmp(head + 12, "IHDR", 4) == 0, "IHDR first chunk");
            const unsigned char bitDepth  = head[24];
            const unsigned char colorType = head[25];
            const unsigned char interlace = head[28];
            ok(bitDepth == 8, "bit depth 8");
            ok(colorType == 0 || colorType == 2 || colorType == 6,
               "color type 0/2/6 (wiki-media parser envelope)");
            ok(interlace == 0, "non-interlaced");
        }
    }

    DeleteFileW(pGdi.c_str());
    DeleteFileW(pWic.c_str());
    Gdiplus::GdiplusShutdown(token);
    if (coHr == S_OK || coHr == S_FALSE) CoUninitialize();

    printf(g_fail ? "FAILED (%d)\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
