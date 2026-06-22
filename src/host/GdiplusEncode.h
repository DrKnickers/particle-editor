#pragma once
// GdiplusEncode.h — the one GDI+ image-encoder CLSID lookup + base64 encoder
// for the host/UI image-export paths (DRY audit 2026-06-22, cpp-host-1).
// Consolidates four copy-pasted encoder-CLSID lookups (AlphaCompositor's PNG +
// JPEG, WindowCapture's PNG, PaletteThumbs' PNG — differing only by MIME) and
// two byte-identical Base64Encode copies (AlphaCompositor + PaletteThumbs).
// `inline` so every translation unit shares one definition with no ODR conflict;
// the function-local statics likewise resolve to a single instance program-wide.
//
// Threading: the per-MIME CLSID cache is populated on first use without a lock —
// matching the originals. Every call site runs on the host UI thread; this is
// NOT safe for a concurrent first call from multiple threads.

// <gdiplus.h> needs <windows.h> (wingdi.h types + the min/max macros) ahead of
// it, so pull it in here to keep this header self-contained. Guarded so we don't
// fight a TU that already configured the windows.h include.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>   // IStream / COM types gdiplus.h references (lean windows.h omits them)
#include <gdiplus.h>
#include <cstdint>
#include <cstring>   // wcscmp
#include <string>
#include <vector>

namespace host {

// Find the GDI+ encoder CLSID for a MIME type ("image/png", "image/jpeg", …).
// The encoder list is enumerated once per distinct MIME and cached; returns
// false (leaving `out` untouched) if GDI+ has no encoder for `mime` — should
// never happen with GDI+ initialized, but we fail soft.
inline bool GdiplusEncoderClsid(const wchar_t* mime, CLSID& out)
{
    struct Entry { std::wstring mime; CLSID clsid; };
    static std::vector<Entry> cache;   // one shared instance (inline fn static)
    for (const Entry& e : cache)
        if (e.mime == mime) { out = e.clsid; return true; }

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
        if (wcscmp(info[i].MimeType, mime) == 0)
        {
            out = info[i].Clsid;
            cache.push_back({ mime, info[i].Clsid });
            return true;
        }
    }
    return false;
}

// Standard base64 encoder (no deps). Encodes the bytes the GDI+ encoder produced
// into the ASCII payload the React side reads via `data:image/...;base64,<…>`.
inline std::string Base64Encode(const uint8_t* data, size_t len)
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

} // namespace host
