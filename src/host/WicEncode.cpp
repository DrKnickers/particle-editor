// WicEncode.cpp — see WicEncode.h. [R3b] of
// tasks/2026-07-07-perf-followups-plan.md.

#include "WicEncode.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace host {

bool EncodeBgraToPngWic(const unsigned char* bgra, int w, int h,
                        const std::wstring& path)
{
    if (!bgra || w <= 0 || h <= 0) return false;
    // Overflow guard (review): the stride/size narrows to UINT below — at
    // extreme dimensions the unsigned multiply would wrap before WIC sees it.
    const unsigned long long strideLL = static_cast<unsigned long long>(w) * 4ull;
    const unsigned long long bytesLL  = strideLL * static_cast<unsigned long long>(h);
    if (bytesLL > 0xFFFFFFFFull) return false;

    ComPtr<IWICImagingFactory> fac;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;

    ComPtr<IWICStream> stream;
    if (FAILED(fac->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)))
        return false;

    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)))
        return false;
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2>         props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return false;

    // FilterOption=None is the speed lever (skip the per-scanline adaptive
    // filter search); InterlaceOption=false keeps the output inside what the
    // wiki-media raw PNG parser supports. Property failures are non-fatal —
    // the encode still produces a valid PNG, just slower/bigger.
    if (props)
    {
        PROPBAG2 optFilter = {};
        optFilter.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT vFilter;
        VariantInit(&vFilter);
        vFilter.vt   = VT_UI1;
        vFilter.bVal = WICPngFilterNone;
        props->Write(1, &optFilter, &vFilter);

        PROPBAG2 optInterlace = {};
        optInterlace.pstrName = const_cast<LPOLESTR>(L"InterlaceOption");
        VARIANT vInterlace;
        VariantInit(&vInterlace);
        vInterlace.vt      = VT_BOOL;
        vInterlace.boolVal = VARIANT_FALSE;
        props->Write(1, &optInterlace, &vInterlace);
    }
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h))))
        return false;

    // 32bppBGR: X8 alpha ignored — parity with the GDI+ path's
    // PixelFormat32bppRGB (the alpha byte is undefined after PrintWindow).
    // SetPixelFormat negotiates; WriteSource below converts the wrapped
    // buffer to whatever the encoder settled on (PNG typically lands on
    // 24bppBGR — color type 2, inside the wiki-media parser's support).
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;

    ComPtr<IWICBitmap> bmp;
    if (FAILED(fac->CreateBitmapFromMemory(
            static_cast<UINT>(w), static_cast<UINT>(h),
            GUID_WICPixelFormat32bppBGR, static_cast<UINT>(w) * 4,
            static_cast<UINT>(w) * 4 * static_cast<UINT>(h),
            const_cast<BYTE*>(bgra), &bmp)))
        return false;

    if (FAILED(frame->WriteSource(bmp.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(enc->Commit())) return false;
    return true;
}

}  // namespace host
