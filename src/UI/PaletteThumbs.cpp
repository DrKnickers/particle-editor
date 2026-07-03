// PaletteThumbs.cpp — thumbnail decode → base64 PNG for the new-UI texture
// palette.
//
// Self-contained on purpose: it reuses the *technique* of the legacy popup's
// DecodeThumbnail (TexturePalette.cpp) — D3DXCreateTextureFromFileInMemoryEx
// into a scratch A8R8G8B8 surface, LockRect, copy out the BGRA pixels — but
// targets a PNG byte stream (GDI+) instead of an HBITMAP, and is parameterised
// with the FileManager + device rather than the popup's file-static services.
// (The legacy popup TU, TexturePalette.cpp, was removed with the old
// architecture.)
//
// The PNG-encoder-CLSID lookup and base64 encoder are shared via
// host/GdiplusEncode.h (host::GdiplusEncoderClsid / host::Base64Encode),
// consolidated from AlphaCompositor's former copies by a DRY audit.

#include "TexturePalette.h"
#include "../utils.h"     // WideToAnsi
#include "../managers.h"  // IFileManager
#include "../files.h"     // IFile
#include "../ResourceLimits.h"  // kMaxTextureAssetBytes
#include "../AssetPathSafety.h"

#include <d3d9.h>
#include <d3dx9.h>
#include <gdiplus.h>
#include "../host/GdiplusEncode.h"   // host::GdiplusEncoderClsid / host::Base64Encode
#include "../host/PerfTrace.h"
#include <objidl.h>       // IStream / CreateStreamOnHGlobal
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

using std::string;
using std::vector;
using std::wstring;

namespace {

using ThumbStatus = TexturePalette::ThumbStatus;

// Decode at a fixed square size. Larger than the legacy 32px popup thumb
// (the React grid renders ~120px cells from the "faithful" option) but
// bounded so the base64 payload stays small.
const int THUMB_PNG_PX = 128;

// filename -> decode result (uri + status). Failures are cached too: a
// known-missing / undecodable texture shouldn't be re-decoded on every
// popover open, and the missing/broken verdict is stable per mod.
std::unordered_map<wstring, TexturePalette::ThumbnailResult> g_bridgeThumbCache;

// PNG encoder-CLSID lookup + base64 encoder now shared via host/GdiplusEncode.h
// — these were copied verbatim from AlphaCompositor.cpp.
// Called qualified as host::GdiplusEncoderClsid / host::Base64Encode below
// (this TU is in an anonymous namespace, not namespace host).

// --- texture resolution: mirrors the legacy OpenTextureFile (TexturePalette.cpp) ---
// FileManager::getFile resolves loose files AND .meg-packed entries, so this
// thumbnails base-game packed textures too. Uppercase + .DDS-swap match the
// TextureManager resolution order.
IFile* OpenTextureFile(IFileManager* fm, const string& filename)
{
    // A .alo bakes the ABSOLUTE authoring path (e.g. C:\Art\Textures\Game\X.tga);
    // reduce it to its basename so it resolves through the MEG chain exactly like
    // the renderer does. Rejecting an absolute name outright is why the atlas
    // picker showed "missing" for effects using the shared master atlas.
    const string name = SanitizeAssetName(filename);
    if (name.empty() || !IsSafeRelativeAssetName(name)) return nullptr;

    string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return (char)::toupper(c); });

    static const string kBase = "Data\\Art\\Textures\\";
    if (IFile* f = fm->getFile(kBase + upper)) return f;

    const size_t dot = upper.rfind('.');
    if (dot != string::npos)
    {
        const string swapped = upper.substr(0, dot) + ".DDS";
        if (IFile* f = fm->getFile(kBase + swapped)) return f;
    }
    return nullptr;
}

// Read a texture's raw file bytes via the FileManager. Returns false if the
// file can't be opened; `out` is empty for a zero-byte file (caller treats that
// as broken). Mirrors the byte-read previously inlined in DecodeToPngBytes.
bool ReadTextureBytes(IFileManager* fm, const wstring& filename, vector<char>& out)
{
    assert(fm != nullptr && "ReadTextureBytes requires a non-null IFileManager");
    out.clear();
    // Absolute authoring paths -> basename (see OpenTextureFile), so a .alo's
    // C:\Art\Textures\... texture resolves instead of being rejected here.
    const string name = SanitizeAssetName(WideToAnsi(filename));
    if (name.empty() || !IsSafeRelativeAssetName(name)) return false;
    IFile* file = OpenTextureFile(fm, name);
    if (file == nullptr) return false;

    const unsigned long size = file->size();
    // A safe name can still resolve to a huge asset; cap before the allocation so
    // an adversarial mod can't force a giant resize off file->size() (#415).
    if (size > kMaxTextureAssetBytes) { file->Release(); return false; }
    out.resize(size);
    // A truncated read must fail, not silently hand a zero-padded
    // buffer to the decoder as if it were complete. Also Release() the
    // refcounted IFile (was `delete file`, which bypassed the IFile refcount).
    if (size && file->read(out.data(), size) != size)
    {
        out.clear();
        file->Release();
        return false;
    }
    file->Release();
    return true;
}

// Encode a decoded A8R8G8B8 scratch texture to PNG bytes (NOT base64). The
// surface is `w` x `h`; the output DIB is tightly packed (stride = w*4), so a
// non-square texture encodes correctly. Honors lr.Pitch on the source read.
// Returns false on any D3D/GDI+ failure. Extracted (and de-squared) from the
// PNG-encode block previously inlined in DecodeToPngBytes.
bool EncodeTextureToPngBytes(IDirect3DTexture9* tex, int w, int h, vector<uint8_t>& outPng)
{
    IDirect3DSurface9* surf = nullptr;
    if (FAILED(tex->GetSurfaceLevel(0, &surf))) return false;

    D3DLOCKED_RECT lr = {};
    if (FAILED(surf->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
        surf->Release();
        return false;
    }

    // Copy out into a tightly-packed BGRA buffer so the source surface can be
    // unlocked/released before GDI+ touches the pixels.
    const int stride = w * 4;
    vector<uint8_t> dib((size_t)stride * (size_t)h);
    for (int y = 0; y < h; ++y)
        memcpy(dib.data() + (size_t)y * stride,
               (const uint8_t*)lr.pBits + (size_t)y * lr.Pitch,
               (size_t)stride);
    surf->UnlockRect();
    surf->Release();

    CLSID pngClsid = {};
    if (!host::GdiplusEncoderClsid(L"image/png", pngClsid)) return false;

    // D3DFMT_A8R8G8B8 is BGRA in memory, matching GDI+ PixelFormat32bppARGB.
    Gdiplus::Bitmap bmp(w, h, stride, PixelFormat32bppARGB, dib.data());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return false;

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) || stream == nullptr)
        return false;
    if (bmp.Save(stream, &pngClsid, nullptr) != Gdiplus::Ok)
    {
        stream->Release();
        return false;
    }

    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    STATSTG stat = {};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) { stream->Release(); return false; }
    const size_t n = (size_t)stat.cbSize.QuadPart;
    outPng.resize(n);
    ULONG readBytes = 0;
    if (FAILED(stream->Read(outPng.data(), (ULONG)n, &readBytes)) || readBytes != n)
    {
        stream->Release();
        return false;
    }
    stream->Release();
    return true;
}

// Decode `filename` to PNG bytes (on Ok). The return value reports
// WHY there's no image. Missing = the file isn't reachable (no device/FM, or
// FileManager can't resolve it = a typo'd/absent path). Broken = the file IS
// present but unusable (empty, or D3DX/GDI+ can't turn it into pixels). This
// mirrors the legacy popup's GetMissingPlaceholder vs GetBrokenPlaceholder.
ThumbStatus DecodeToPngBytes(IFileManager* fm, IDirect3DDevice9* device,
                             const wstring& filename, vector<uint8_t>& outPng)
{
    if (fm == nullptr || device == nullptr) return ThumbStatus::Missing;

    vector<char> bytes;
    if (!ReadTextureBytes(fm, filename, bytes)) return ThumbStatus::Missing;
    if (bytes.empty()) return ThumbStatus::Broken;

    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        device, bytes.data(), (UINT)bytes.size(),
        THUMB_PNG_PX, THUMB_PNG_PX, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
        D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex);
    if (FAILED(hr) || tex == nullptr) { if (tex) tex->Release(); return ThumbStatus::Broken; }

    const bool ok = EncodeTextureToPngBytes(tex, THUMB_PNG_PX, THUMB_PNG_PX, outPng);
    tex->Release();
    return ok ? ThumbStatus::Ok : ThumbStatus::Broken;
}

} // namespace

namespace TexturePalette {

ThumbnailResult GetThumbnail(const std::wstring& filename,
                             IFileManager* fileManager,
                             IDirect3DDevice9* device)
{
    const bool perfOn = host::perf::Enabled();
    std::unique_ptr<host::perf::Span> span;
    if (perfOn) {
        span = std::make_unique<host::perf::Span>("texture.palette.thumbnail", nlohmann::json{
            {"filename", WideToAnsi(filename)}
        });
    }
    auto it = g_bridgeThumbCache.find(filename);
    if (it != g_bridgeThumbCache.end()) {
        if (span) span->End("cache_hit");
        return it->second;
    }

    vector<uint8_t> png;
    const ThumbStatus status = DecodeToPngBytes(fileManager, device, filename, png);

    ThumbnailResult result;
    result.status = status;
    if (status == ThumbStatus::Ok && !png.empty())
        result.dataUri = "data:image/png;base64," + host::Base64Encode(png.data(), png.size());
    else if (status == ThumbStatus::Ok)
        // Defensive: an "Ok" decode that produced no bytes is, to the user, a
        // broken texture (no image to show).
        result.status = ThumbStatus::Broken;

    g_bridgeThumbCache[filename] = result;  // cache failures too (don't re-decode known-bad)
    if (span) {
        span->End(result.status == ThumbStatus::Ok ? "ok" :
                  result.status == ThumbStatus::Missing ? "missing" : "broken");
    }
    return result;
}

void ClearBridgeThumbCache()
{
    g_bridgeThumbCache.clear();
}

PreviewResult GetTexturePreview(const std::wstring& filename,
                                IFileManager* fileManager,
                                IDirect3DDevice9* device,
                                int maxBound,
                                bool flattenAlpha)
{
    const bool perfOn = host::perf::Enabled();
    std::unique_ptr<host::perf::Span> totalSpan;
    if (perfOn) {
        totalSpan = std::make_unique<host::perf::Span>("texture.preview", nlohmann::json{
            {"filename", WideToAnsi(filename)},
            {"maxBound", maxBound},
            {"flattenAlpha", flattenAlpha}
        });
    }
    PreviewResult out;
    if (device == nullptr || filename.empty() || fileManager == nullptr) {
        out.status = "missing";
        if (totalSpan) totalSpan->End("missing");
        return out;
    }

    vector<char> bytes;
    {
        std::unique_ptr<host::perf::Span> readSpan;
        if (perfOn) {
            readSpan = std::make_unique<host::perf::Span>("texture.preview.read_bytes", nlohmann::json{
                {"filename", WideToAnsi(filename)}
            });
        }
        if (!ReadTextureBytes(fileManager, filename, bytes) || bytes.empty())
        {
            if (readSpan) readSpan->End("missing");
            if (totalSpan) totalSpan->End("missing");
            out.status = "missing";
            return out;
        }
        if (readSpan) readSpan->End("ok");
    }

    {
        std::unique_ptr<host::perf::Span> infoSpan;
        if (perfOn) {
            infoSpan = std::make_unique<host::perf::Span>("texture.preview.image_info", nlohmann::json{
                {"filename", WideToAnsi(filename)},
                {"byteCount", bytes.size()}
            });
        }
        D3DXIMAGE_INFO info = {};
        if (FAILED(D3DXGetImageInfoFromFileInMemory(bytes.data(), (UINT)bytes.size(), &info))
            || info.ResourceType != D3DRTYPE_TEXTURE)
        {
            if (infoSpan) infoSpan->End("broken");
            if (totalSpan) totalSpan->End("broken");
            out.status = "broken";
            return out;
        }
        out.srcW = (int)info.Width;
        out.srcH = (int)info.Height;
        if (infoSpan) infoSpan->End("ok");
    }

    // Downscale only when a dimension exceeds maxBound; preserve aspect ratio.
    int tw = out.srcW, th = out.srcH;
    if (tw > maxBound || th > maxBound)
    {
        // Parenthesize std::max to defeat the windows.h max() macro.
        if (tw >= th) { th = (std::max)(1, (int)((double)th * maxBound / tw)); tw = maxBound; }
        else          { tw = (std::max)(1, (int)((double)tw * maxBound / th)); th = maxBound; }
    }

    if (perfOn) {
        host::perf::Emit({
            {"eventName", "texture.preview.resize_plan"},
            {"eventType", "instant"},
            {"filename", WideToAnsi(filename)},
            {"srcW", out.srcW},
            {"srcH", out.srcH},
            {"targetW", tw},
            {"targetH", th}
        });
    }

    IDirect3DTexture9* tex = nullptr;
    {
        std::unique_ptr<host::perf::Span> decodeSpan;
        if (perfOn) {
            decodeSpan = std::make_unique<host::perf::Span>("texture.preview.d3dx_decode", nlohmann::json{
                {"filename", WideToAnsi(filename)},
                {"targetW", tw},
                {"targetH", th}
            });
        }
        HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
            device, bytes.data(), (UINT)bytes.size(),
            (UINT)tw, (UINT)th, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
            D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex);
        if (FAILED(hr) || tex == nullptr) {
            if (tex) tex->Release();
            if (decodeSpan) decodeSpan->End("broken");
            if (totalSpan) totalSpan->End("broken");
            out.status = "broken";
            return out;
        }
        if (decodeSpan) decodeSpan->End("ok");
    }

    // [atlas-picker] "Color channel" preview mode. Particle atlases are commonly
    // ADDITIVE (a frame's content lives in RGB with alpha 0), so honoring alpha would
    // render those frames transparent ("missing"). When flattenAlpha is set, force
    // every pixel fully OPAQUE so the raw RGB colour channel shows on its own black —
    // every frame is visible. Done HERE (before PNG encode) because a browser canvas
    // premultiplies and would lose alpha-0 RGB. flattenAlpha=false leaves the texture's
    // real alpha so the picker can show the alpha cut-outs (and the legitimately empty
    // frames of textures that carry no alpha).
    if (flattenAlpha)
    {
        std::unique_ptr<host::perf::Span> flattenSpan;
        if (perfOn) {
            flattenSpan = std::make_unique<host::perf::Span>("texture.preview.flatten_alpha", nlohmann::json{
                {"filename", WideToAnsi(filename)}
            });
        }
        D3DSURFACE_DESC sd = {};
        D3DLOCKED_RECT  lr = {};
        if (SUCCEEDED(tex->GetLevelDesc(0, &sd)) &&
            SUCCEEDED(tex->LockRect(0, &lr, NULL, 0)))
        {
            for (UINT y = 0; y < sd.Height; ++y)
            {
                uint8_t* prow = (uint8_t*)lr.pBits + (size_t)y * lr.Pitch;
                for (UINT x = 0; x < sd.Width; ++x)
                    prow[(size_t)x * 4 + 3] = 255;   // A8R8G8B8 in memory = B,G,R,A; force opaque
            }
            tex->UnlockRect(0);
            if (flattenSpan) flattenSpan->End("ok");
        }
        else
        {
            if (flattenSpan) flattenSpan->End("skipped");
        }
    }

    vector<uint8_t> png;
    bool ok = false;
    {
        std::unique_ptr<host::perf::Span> encodeSpan;
        if (perfOn) {
            encodeSpan = std::make_unique<host::perf::Span>("texture.preview.png_encode", nlohmann::json{
                {"filename", WideToAnsi(filename)},
                {"targetW", tw},
                {"targetH", th}
            });
        }
        ok = EncodeTextureToPngBytes(tex, tw, th, png);
        if (encodeSpan) encodeSpan->End(ok ? "ok" : "broken");
    }
    tex->Release();
    if (!ok) {
        if (totalSpan) totalSpan->End("broken");
        out.status = "broken";
        return out;
    }

    out.status  = "ok";
    {
        std::unique_ptr<host::perf::Span> base64Span;
        if (perfOn) {
            base64Span = std::make_unique<host::perf::Span>("texture.preview.base64", nlohmann::json{
                {"filename", WideToAnsi(filename)},
                {"pngBytes", png.size()}
            });
        }
        out.dataUri = "data:image/png;base64," + host::Base64Encode(png.data(), png.size());
        if (base64Span) base64Span->End("ok");
    }
    if (totalSpan) totalSpan->End("ok");
    return out;
}

} // namespace TexturePalette
