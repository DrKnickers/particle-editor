// PaletteThumbs.cpp — thumbnail decode → base64 PNG for the new-UI texture
// palette (sub-feature B).
//
// Self-contained on purpose: it reuses the *technique* of the legacy popup's
// DecodeThumbnail (TexturePalette.cpp) — D3DXCreateTextureFromFileInMemoryEx
// into a scratch A8R8G8B8 surface, LockRect, copy out the BGRA pixels — but
// targets a PNG byte stream (GDI+) instead of an HBITMAP, and is parameterised
// with the FileManager + device rather than the popup's file-static services.
// (The legacy popup TU, TexturePalette.cpp, was removed with arch-A in.)
//
// The PNG-encoder-CLSID lookup and base64 encoder are copied verbatim (into a
// private anonymous namespace) from AlphaCompositor.cpp's proven
// implementations, so there is no cross-TU linkage coupling.

#include "TexturePalette.h"
#include "../utils.h"     // WideToAnsi
#include "../managers.h"  // IFileManager
#include "../files.h"     // IFile

#include <d3d9.h>
#include <d3dx9.h>
#include <gdiplus.h>
#include <objidl.h>       // IStream / CreateStreamOnHGlobal
#include <algorithm>
#include <cassert>
#include <cstring>
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
// (the React grid renders ~120px cells from sub-feature B's "faithful"
// option) but bounded so the base64 payload stays small.
const int THUMB_PNG_PX = 128;

// filename -> decode result (uri + status). Failures are cached too: a
// known-missing / undecodable texture shouldn't be re-decoded on every
// popover open, and the missing/broken verdict is stable per mod.
std::unordered_map<wstring, TexturePalette::ThumbnailResult> g_bridgeThumbCache;

// --- copied verbatim from AlphaCompositor.cpp (anonymous namespace there) ---

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
            cached   = info[i].Clsid;
            outClsid = cached;
            found    = true;
            return true;
        }
    }
    return false;
}

string Base64Encode(const uint8_t* data, size_t len)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    string out;
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

// --- texture resolution: mirrors the legacy OpenTextureFile (TexturePalette.cpp) ---
// FileManager::getFile resolves loose files AND .meg-packed entries, so this
// thumbnails base-game packed textures too. Uppercase + .DDS-swap match the
// TextureManager resolution order.
IFile* OpenTextureFile(IFileManager* fm, const string& filename)
{
    string upper = filename;
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
    IFile* file = OpenTextureFile(fm, WideToAnsi(filename));
    if (file == nullptr) return false;

    const unsigned long size = file->size();
    out.resize(size);
    if (size) file->read(out.data(), size);
    delete file;
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
    if (!GetPngEncoderClsid(pngClsid)) return false;

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

// Decode `filename` to PNG bytes (on Ok).: the return value reports
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
    auto it = g_bridgeThumbCache.find(filename);
    if (it != g_bridgeThumbCache.end()) return it->second;

    vector<uint8_t> png;
    const ThumbStatus status = DecodeToPngBytes(fileManager, device, filename, png);

    ThumbnailResult result;
    result.status = status;
    if (status == ThumbStatus::Ok && !png.empty())
        result.dataUri = "data:image/png;base64," + Base64Encode(png.data(), png.size());
    else if (status == ThumbStatus::Ok)
        // Defensive: an "Ok" decode that produced no bytes is, to the user, a
        // broken texture (no image to show).
        result.status = ThumbStatus::Broken;

    g_bridgeThumbCache[filename] = result;  // cache failures too (don't re-decode known-bad)
    return result;
}

void ClearBridgeThumbCache()
{
    g_bridgeThumbCache.clear();
}

PreviewResult GetTexturePreview(const std::wstring& filename,
                                IFileManager* fileManager,
                                IDirect3DDevice9* device,
                                int maxBound)
{
    PreviewResult out;
    if (device == nullptr || filename.empty() || fileManager == nullptr) { out.status = "missing"; return out; }

    vector<char> bytes;
    if (!ReadTextureBytes(fileManager, filename, bytes) || bytes.empty())
    {
        out.status = "missing";
        return out;
    }

    // Probe true source dimensions and reject non-2D resources (cube/volume):
    // the picker grid only makes sense over a flat sheet.
    D3DXIMAGE_INFO info = {};
    if (FAILED(D3DXGetImageInfoFromFileInMemory(bytes.data(), (UINT)bytes.size(), &info))
        || info.ResourceType != D3DRTYPE_TEXTURE)
    {
        out.status = "broken";
        return out;
    }
    out.srcW = (int)info.Width;
    out.srcH = (int)info.Height;

    // Downscale only when a dimension exceeds maxBound; preserve aspect ratio.
    int tw = out.srcW, th = out.srcH;
    if (tw > maxBound || th > maxBound)
    {
        // Parenthesize std::max to defeat the windows.h max() macro.
        if (tw >= th) { th = (std::max)(1, (int)((double)th * maxBound / tw)); tw = maxBound; }
        else          { tw = (std::max)(1, (int)((double)tw * maxBound / th)); th = maxBound; }
    }

    IDirect3DTexture9* tex = nullptr;
    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        device, bytes.data(), (UINT)bytes.size(),
        (UINT)tw, (UINT)th, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
        D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &tex);
    if (FAILED(hr) || tex == nullptr) { if (tex) tex->Release(); out.status = "broken"; return out; }

    vector<uint8_t> png;
    const bool ok = EncodeTextureToPngBytes(tex, tw, th, png);
    tex->Release();
    if (!ok) { out.status = "broken"; return out; }

    out.status  = "ok";
    out.dataUri = "data:image/png;base64," + Base64Encode(png.data(), png.size());
    return out;
}

} // namespace TexturePalette
