#include "TexturePalette.h"
#include "../crc32.h"
#include "../utils.h"
#include "../managers.h"
#include "../Resources/resource.en.h"
#include "../Resources/resource.h"

#include <d3d9.h>
#include <d3dx9.h>
#include <shlobj.h>      // SHGetFolderPathW
#include <algorithm>
#include <cwctype>
#include <cstdio>
#include <cstdarg>
#include <cstring>

#pragma comment(lib, "Shell32.lib")

using namespace TexturePalette;
using std::wstring;
using std::vector;
using std::unordered_map;

// ============================================================================
// Popup + content control
//
// Layout (px, popup client area):
//
//   ┌──────────────────────────────────────────────────────────┐
//   │  ○ Color   ○ Bump                                        │ filter row (20px)
//   │                                                          │
//   │  Pinned                                                  │ pin label
//   │  ┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐                       │ pin row (32px)
//   │  │  ││  ││  ││  ││  ││  ││  ││  │                       │
//   │  └──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘                       │
//   │                                                          │
//   │  Recent                                                  │ recent label
//   │  ┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐┌──┐                       │ recent row (32px)
//   │  │  ││  ││  ││  ││  ││  ││  ││  │                       │
//   │  └──┘└──┘└──┘└──┘└──┘└──┘└──┘└──┘                       │
//   │  status strip ...                                        │ status (14px)
//   └──────────────────────────────────────────────────────────┘

namespace {

#ifndef NDEBUG
void DbgPrintf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsprintf_s(buf, fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
}
#else
void DbgPrintf(const char*, ...) {}
#endif

// Cell geometry. Each cell holds a thumbnail at the top + a single-
// line filename strip below, ellipsis-clipped for long names.
// Thumbnail fills the cell width — bigger preview, more useful at a
// glance. Cell height grows accordingly to keep the thumb square.
const int CELL_W         = 140;                              // ~28 chars of filename fit at 8pt before ellipsis
const int THUMB_PX       = CELL_W;                           // 140 — square thumb, no horizontal padding
const int NAME_H         = 16;
const int CELL_H         = THUMB_PX + 4 + NAME_H;            // 160 — thumb + 4 px gap + name strip
const int THUMB_GAP_PX   =  6;
const int THUMBS_PER_ROW =  4;                               // cells per visual sub-row
const int SECTION_ROWS   =  3;                               // sub-rows per logical section (pin / recent)
const int SUBROW_GAP     =  6;                               // gap between sub-rows within a section
const int MAX_PER_SECTION = THUMBS_PER_ROW * SECTION_ROWS;   // 8 — matches MAX_PINS/MAX_RECENTS in PaletteStore

// Popup layout (client area).
const int POPUP_MARGIN_X   = 16;
const int POPUP_MARGIN_Y   = 10;
const int FILTER_ROW_H     = 22;
const int LABEL_H          = 16;
const int ROW_GAP          = 10;
const int STATUS_H         = 16;
const int SECTION_CELLS_H  = SECTION_ROWS * CELL_H + (SECTION_ROWS - 1) * SUBROW_GAP;  // 206
const int CONTENT_W        = POPUP_MARGIN_X * 2 + THUMBS_PER_ROW * CELL_W
                            + (THUMBS_PER_ROW - 1) * THUMB_GAP_PX;        // ~610
const int CONTENT_H        = POPUP_MARGIN_Y
                            + FILTER_ROW_H + ROW_GAP
                            + LABEL_H + SECTION_CELLS_H + ROW_GAP
                            + LABEL_H + SECTION_CELLS_H + ROW_GAP
                            + STATUS_H
                            + POPUP_MARGIN_Y;                              // ~542

// Hover-pin badge. Loaded from IDB_PIN_BADGE — a 24×48 BMP strip
// with the empty/hover state on top and the filled/pinned state
// on bottom. Drawn at the thumb's top-right when the cell is
// hovered. Bigger than a tiny indicator because it's the click
// target for pinning/unpinning — needs to be unambiguously
// clickable.
const int STAR_PX    = 24;
const int STAR_INSET = 4;
HBITMAP g_pinBadgeBmp = NULL;   // 24×48 strip; lazy-loaded on first use

// Custom child-window IDs inside the popup.
const int IDC_PAL_CONTENT = 5001;
const int IDC_PAL_STATUS_TIMER = 5002;
const UINT_PTR STATUS_CLEAR_TIMER_ID = 1;

// Forwarded services. Set via SetServices().
IFileManager*     g_fileManager = nullptr;
IDirect3DDevice9* g_device      = nullptr;

// EmitterProps HWND for commit-message routing.
HWND g_commitTarget = nullptr;

// Visibility callback (button-toggle sync).
VisibilityCallback g_visibilityCb = nullptr;

// Popup state.
HWND g_popup        = nullptr;   // top-level popup window (lazy-created)
HWND g_popupContent = nullptr;   // owner-draw content child
HWND g_popupRadColor= nullptr;   // filter radios
HWND g_popupRadBump = nullptr;
HWND g_popupStatus  = nullptr;
HWND g_popupOwner   = nullptr;   // most recent ownerEditor (cached for refresh)

// Currently-selected entry index within the active filter's display
// list. -1 = none. Selection lives across show/hide.
int  g_selRow      = -1;    // 0 = pin row, 1 = recent row
int  g_selCol      = -1;
// Currently-hovered cell (-1 = none).
int  g_hoverRow    = -1;
int  g_hoverCol    = -1;

// In-memory thumbnail cache.
std::unordered_map<std::wstring, HBITMAP> g_thumbCache;

// Procedural placeholders, generated on first need.
HBITMAP g_phBroken  = nullptr;  // 32x32 magenta-with-X
HBITMAP g_phMissing = nullptr;  // 32x32 greyed-out

const wchar_t* CLASS_POPUP   = L"AloTexturePalettePopup";
const wchar_t* CLASS_CONTENT = L"AloPaletteContent";

LRESULT CALLBACK PopupWndProc  (HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK ContentWndProc(HWND, UINT, WPARAM, LPARAM);

// ----------------------------------------------------------------------------
// Thumbnail decoder

HBITMAP MakeSolidThumb(COLORREF tint, bool drawX)
{
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = THUMB_PX;
    bmi.bmiHeader.biHeight      = -THUMB_PX;        // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC hScreen = GetDC(NULL);
    void* pBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    ReleaseDC(NULL, hScreen);
    if (hbm == NULL || pBits == NULL) return NULL;

    const uint8_t r = GetRValue(tint);
    const uint8_t g = GetGValue(tint);
    const uint8_t b = GetBValue(tint);
    uint8_t* p = (uint8_t*)pBits;
    for (int y = 0; y < THUMB_PX; ++y)
    {
        for (int x = 0; x < THUMB_PX; ++x)
        {
            uint8_t* px = p + (y * THUMB_PX + x) * 4;
            // BGRA8: B=p[0], G=p[1], R=p[2], A=p[3]
            px[0] = b; px[1] = g; px[2] = r; px[3] = 0xFF;

            if (drawX)
            {
                // Diagonal X from corner to corner.
                if (x == y || x == (THUMB_PX - 1 - y))
                {
                    px[0] = 0; px[1] = 0; px[2] = 0; px[3] = 0xFF;
                }
            }
        }
    }
    return hbm;
}

HBITMAP GetBrokenPlaceholder()
{
    if (g_phBroken == NULL) g_phBroken = MakeSolidThumb(RGB(220, 60, 220), true);
    return g_phBroken;
}

HBITMAP GetMissingPlaceholder()
{
    if (g_phMissing == NULL) g_phMissing = MakeSolidThumb(RGB(160, 160, 160), true);
    return g_phMissing;
}

// Try to open a texture via FileManager. Matches the resolution
// order TextureManager::getTexture uses in main.cpp: prepend the
// engine's basePath ("Data\Art\Textures\"), then fall back to a
// .DDS extension swap (common engine convention — the .alo stores
// one extension but the on-disk file may be the other format).
IFile* OpenTextureFile(const std::string& filename)
{
    // The TextureManager uppercases names internally — match that
    // so case differences between INI-stored filenames and on-disk
    // names don't matter on mods that ship uppercase filenames.
    std::string upper = filename;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return (char)::toupper(c); });

    static const std::string kBase = "Data\\Art\\Textures\\";
    IFile* f = g_fileManager->getFile(kBase + upper);
    if (f != nullptr) return f;

    // .DDS swap (existing engine fallback at main.cpp:162).
    size_t dot = upper.rfind('.');
    if (dot != std::string::npos)
    {
        std::string swapped = upper.substr(0, dot) + ".DDS";
        f = g_fileManager->getFile(kBase + swapped);
        if (f != nullptr) return f;
    }
    return nullptr;
}

// Decode a texture to a 32x32 HBITMAP via D3DX9 → DIB section.
// Returns either a real thumbnail, the broken placeholder (decode
// failed), or the missing placeholder (FileManager couldn't find
// the file).
HBITMAP DecodeThumbnail(const std::wstring& filename)
{
    if (g_fileManager == nullptr || g_device == nullptr)
        return GetMissingPlaceholder();

    const std::string ansiName = WideToAnsi(filename);
    IFile* file = OpenTextureFile(ansiName);
    if (file == nullptr)
    {
        DbgPrintf("[Palette] missing file '%s'\n", ansiName.c_str());
        return GetMissingPlaceholder();
    }

    unsigned long size = file->size();
    if (size == 0) { delete file; return GetBrokenPlaceholder(); }

    std::vector<char> bytes(size);
    file->read(bytes.data(), size);
    delete file;

    IDirect3DTexture9* pTex = nullptr;
    HRESULT hr = D3DXCreateTextureFromFileInMemoryEx(
        g_device, bytes.data(), (UINT)size,
        THUMB_PX, THUMB_PX, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH,
        D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL, &pTex);

    if (FAILED(hr) || pTex == nullptr)
    {
        DbgPrintf("[Palette] thumbnail decode failed path='%s' fallback=placeholder\n",
                  ansiName.c_str());
        if (pTex != nullptr) pTex->Release();
        return GetBrokenPlaceholder();
    }

    // Copy the surface to a CreateDIBSection HBITMAP.
    IDirect3DSurface9* pSurf = nullptr;
    if (FAILED(pTex->GetSurfaceLevel(0, &pSurf)))
    {
        pTex->Release();
        return GetBrokenPlaceholder();
    }
    D3DLOCKED_RECT lr;
    if (FAILED(pSurf->LockRect(&lr, NULL, D3DLOCK_READONLY)))
    {
        pSurf->Release();
        pTex->Release();
        return GetBrokenPlaceholder();
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = THUMB_PX;
    bmi.bmiHeader.biHeight      = -THUMB_PX;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC hScreen = GetDC(NULL);
    void* pBits = nullptr;
    HBITMAP hbm = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    ReleaseDC(NULL, hScreen);

    if (hbm != NULL && pBits != NULL)
    {
        const uint8_t* src = (const uint8_t*)lr.pBits;
        uint8_t*       dst = (uint8_t*)pBits;
        const int rowBytes = THUMB_PX * 4;
        for (int y = 0; y < THUMB_PX; ++y)
            memcpy(dst + y * rowBytes, src + y * lr.Pitch, rowBytes);
    }
    pSurf->UnlockRect();
    pSurf->Release();
    pTex->Release();

    if (hbm == NULL) return GetBrokenPlaceholder();
    return hbm;
}

HBITMAP GetCachedThumbnail(const std::wstring& filename)
{
    auto it = g_thumbCache.find(filename);
    if (it != g_thumbCache.end()) return it->second;
    HBITMAP hbm = DecodeThumbnail(filename);
    g_thumbCache[filename] = hbm;
    return hbm;
}

// ----------------------------------------------------------------------------
// Geometry helpers

// Full cell rect (thumb area + filename strip).
//   section: 0 = Pinned, 1 = Recent
//   idx:     0..MAX_PER_SECTION-1, laid out left-to-right then
//            top-to-bottom across SECTION_ROWS sub-rows of
//            THUMBS_PER_ROW cells each.
void GetCellRect(int section /*0=pin,1=rec*/, int idx, RECT* out)
{
    const int subRow = idx / THUMBS_PER_ROW;
    const int col    = idx % THUMBS_PER_ROW;
    const int sectionTop = POPUP_MARGIN_Y + FILTER_ROW_H + ROW_GAP + LABEL_H
                         + section * (SECTION_CELLS_H + ROW_GAP + LABEL_H);
    const int yBase = sectionTop + subRow * (CELL_H + SUBROW_GAP);
    const int xBase = POPUP_MARGIN_X + col * (CELL_W + THUMB_GAP_PX);
    out->left   = xBase;
    out->top    = yBase;
    out->right  = xBase + CELL_W;
    out->bottom = yBase + CELL_H;
}

// Just the thumb area within the cell (thumb is centered horizontally,
// top-aligned vertically). Star icon is hit-tested against this rect.
void GetThumbRect(int section, int idx, RECT* out)
{
    RECT cell; GetCellRect(section, idx, &cell);
    const int xBase = cell.left + (CELL_W - THUMB_PX) / 2;
    out->left   = xBase;
    out->top    = cell.top;
    out->right  = xBase + THUMB_PX;
    out->bottom = cell.top + THUMB_PX;
}

bool HitTestCells(int x, int y, int* outSection, int* outIdx)
{
    for (int s = 0; s < 2; ++s)
    {
        for (int i = 0; i < MAX_PER_SECTION; ++i)
        {
            RECT rc; GetCellRect(s, i, &rc);
            if (x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom)
            {
                *outSection = s; *outIdx = i;
                return true;
            }
        }
    }
    return false;
}

bool HitTestStar(int x, int y, int* outSection, int* outIdx)
{
    int s, i;
    if (!HitTestCells(x, y, &s, &i)) return false;
    RECT t; GetThumbRect(s, i, &t);
    const int sx = t.right - STAR_INSET - STAR_PX;
    const int sy = t.top   + STAR_INSET;
    if (x >= sx && x < sx + STAR_PX && y >= sy && y < sy + STAR_PX)
    {
        *outSection = s; *outIdx = i;
        return true;
    }
    return false;
}

// ----------------------------------------------------------------------------
// Content control — owner-draw paint

void DrawStar(HDC hdc, int x, int y, bool filled)
{
    // Blit the appropriate half of IDB_PIN_BADGE (24×48 strip) at the
    // requested position. Top half = empty/hover; bottom = pinned.
    // Lazy-load the bitmap on first call.
    if (g_pinBadgeBmp == NULL)
    {
        HMODULE hMod = GetModuleHandleW(NULL);
        g_pinBadgeBmp = (HBITMAP)LoadImageW(hMod, MAKEINTRESOURCEW(IDB_PIN_BADGE),
                                            IMAGE_BITMAP, 0, 0, LR_DEFAULTCOLOR);
        if (g_pinBadgeBmp == NULL)
        {
            DbgPrintf("[Palette] LoadImage(IDB_PIN_BADGE) failed err=%lu\n", GetLastError());
            return;
        }
    }
    HDC hMem = CreateCompatibleDC(hdc);
    HGDIOBJ old = SelectObject(hMem, g_pinBadgeBmp);
    // src y offset = 0 for empty (top half), 24 for filled (bottom half).
    const int srcY = filled ? STAR_PX : 0;
    BitBlt(hdc, x, y, STAR_PX, STAR_PX, hMem, 0, srcY, SRCCOPY);
    SelectObject(hMem, old);
    DeleteDC(hMem);
}

void DrawCell(HDC hdc, const RECT& cell, const RECT& thumb,
              const Entry& e, bool selected, bool hovered)
{
    // Cell background. Hover tints the whole cell saturated light
    // blue so it stands out unambiguously against any thumbnail
    // palette. The prior pale blue (215, 232, 250) blended too
    // quietly with grey; this is several shades more saturated.
    const COLORREF bgColor = hovered ? RGB(160, 200, 250) : RGB(240, 240, 240);
    HBRUSH bg = CreateSolidBrush(bgColor);
    FillRect(hdc, &cell, bg);
    DeleteObject(bg);

    // Thumbnail blit into the centered thumb rect.
    HBITMAP hbm = GetCachedThumbnail(e.filename);
    if (hbm != NULL)
    {
        HDC hMem = CreateCompatibleDC(hdc);
        HGDIOBJ old = SelectObject(hMem, hbm);
        BitBlt(hdc, thumb.left, thumb.top, THUMB_PX, THUMB_PX, hMem, 0, 0, SRCCOPY);
        SelectObject(hMem, old);
        DeleteDC(hMem);
    }

    // Thumb frame.
    //   selected: 2 px saturated blue (commit target)
    //   hovered  : 2 px lighter blue   (mouse-over indicator)
    //   default  : 1 px grey
    if (selected)
    {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(40, 100, 220));
        HGDIOBJ old = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, thumb.left + 1, thumb.top + 1,
                       thumb.right - 1, thumb.bottom - 1);
        SelectObject(hdc, old); SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }
    else if (hovered)
    {
        // Hover frame: 3 px bright blue — same tone family as the
        // hover background tint, lighter than the saturated-blue
        // selection frame so the two can still be distinguished
        // when a cell goes from hover to selected.
        HPEN pen = CreatePen(PS_SOLID, 3, RGB(70, 150, 240));
        HGDIOBJ old = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, thumb.left + 1, thumb.top + 1,
                       thumb.right - 1, thumb.bottom - 1);
        SelectObject(hdc, old); SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }
    else
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(150, 150, 150));
        HGDIOBJ old = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, thumb.left, thumb.top, thumb.right, thumb.bottom);
        SelectObject(hdc, old); SelectObject(hdc, oldBrush);
        DeleteObject(pen);
    }

    // Pin badge on hover (top-right of thumb, drawn AFTER the frame so
    // it sits visibly on top).
    if (hovered)
    {
        DrawStar(hdc, thumb.right - STAR_INSET - STAR_PX,
                      thumb.top   + STAR_INSET, e.isPinned);
    }

    // Filename strip below the thumb. Ellipsis-clipped if too wide.
    RECT nameRect;
    nameRect.left   = cell.left + 1;
    nameRect.right  = cell.right - 1;
    nameRect.top    = thumb.bottom + 2;
    nameRect.bottom = cell.bottom;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(40, 40, 40));
    DrawTextW(hdc, e.filename.c_str(), (int)e.filename.size(), &nameRect,
              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void DrawLabel(HDC hdc, int x, int y, const wchar_t* text)
{
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(60, 60, 60));
    TextOutW(hdc, x, y, text, (int)wcslen(text));
}

void ShowStatus(HWND hPopup, UINT stringId);
void HidePopupAndReset(HWND hPopup, const char* reason);

void RebuildLayout(HWND hContent)
{
    // Update filter-radio "checked" state from the store.
    const SlotMask f = Store::Instance().ActiveFilter();
    SendMessage(g_popupRadColor, BM_SETCHECK, f == SLOT_COLOR ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessage(g_popupRadBump,  BM_SETCHECK, f == SLOT_BUMP  ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(hContent, NULL, TRUE);
}

void OnContentPaint(HWND hWnd)
{
    PAINTSTRUCT ps;
    HDC hdcScreen = BeginPaint(hWnd, &ps);

    // Double-buffer: paint everything into an off-screen DC, then
    // BitBlt the result to the screen in one operation. Without this,
    // each mouse-move-driven repaint shows the intermediate states
    // (grey FillRect → thumbnail blit → frame → text), producing
    // visible flicker on the hovered cell.
    RECT rcClient;
    GetClientRect(hWnd, &rcClient);
    HDC     hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmMem = CreateCompatibleBitmap(hdcScreen,
                                            rcClient.right, rcClient.bottom);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hbmMem);
    HDC     hdc    = hdcMem;   // all subsequent draws go through hdc

    // BeginPaint's DC starts with the system bitmap font (ugly on
    // modern Windows). Select the dialog font so labels + filename
    // strings render in the same 8pt MS Shell Dlg as the rest of
    // the editor.
    HFONT hFont = (HFONT)SendMessage(hWnd, WM_GETFONT, 0, 0);
    if (hFont == NULL) hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

    HBRUSH bg = (HBRUSH)(COLOR_BTNFACE + 1);
    FillRect(hdc, &ps.rcPaint, bg);

    // Section labels.
    DrawLabel(hdc, POPUP_MARGIN_X, POPUP_MARGIN_Y + FILTER_ROW_H + ROW_GAP - 2,
              L"Pinned");
    DrawLabel(hdc, POPUP_MARGIN_X,
              POPUP_MARGIN_Y + FILTER_ROW_H + ROW_GAP + LABEL_H + SECTION_CELLS_H + ROW_GAP - 2,
              L"Recent");

    const SlotMask filter = Store::Instance().ActiveFilter();
    const std::vector<Entry> pins    = Store::Instance().Pins(filter);
    const std::vector<Entry> recents = Store::Instance().Recents(filter);

    auto drawSection = [&](int section, const std::vector<Entry>& entries)
    {
        for (int i = 0; i < MAX_PER_SECTION; ++i)
        {
            RECT cell;  GetCellRect (section, i, &cell);
            RECT thumb; GetThumbRect(section, i, &thumb);
            if (i < (int)entries.size())
            {
                const bool sel = (g_selRow   == section && g_selCol   == i);
                const bool hov = (g_hoverRow == section && g_hoverCol == i);
                DrawCell(hdc, cell, thumb, entries[i], sel, hov);
            }
            else
            {
                // Empty cell: dim background + dim frame around the
                // thumb area only (no name strip).
                HBRUSH e = CreateSolidBrush(RGB(228, 228, 228));
                FillRect(hdc, &cell, e);
                DeleteObject(e);
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
                HGDIOBJ old = SelectObject(hdc, pen);
                HGDIOBJ oldB = SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, thumb.left, thumb.top, thumb.right, thumb.bottom);
                SelectObject(hdc, old); SelectObject(hdc, oldB);
                DeleteObject(pen);
            }
        }
    };
    drawSection(0, pins);
    drawSection(1, recents);

    SelectObject(hdc, hOldFont);

    // Flush the off-screen buffer to the screen — only the dirty
    // rect, so partial invalidations stay cheap.
    const int w = ps.rcPaint.right  - ps.rcPaint.left;
    const int h = ps.rcPaint.bottom - ps.rcPaint.top;
    BitBlt(hdcScreen, ps.rcPaint.left, ps.rcPaint.top, w, h,
           hdcMem,    ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hbmMem);
    DeleteDC(hdcMem);
    EndPaint(hWnd, &ps);
}

// Return the (filename, slot) of the cell at (row, col) under the
// current filter, or empty filename if out of range.
void ResolveCell(int row, int col, std::wstring* outFilename, SlotMask* outSlot)
{
    outFilename->clear();
    *outSlot = Store::Instance().ActiveFilter();
    const SlotMask filter = *outSlot;
    if (row == 0)
    {
        std::vector<Entry> pins = Store::Instance().Pins(filter);
        if (col >= 0 && col < (int)pins.size()) *outFilename = pins[col].filename;
    }
    else if (row == 1)
    {
        std::vector<Entry> recents = Store::Instance().Recents(filter);
        if (col >= 0 && col < (int)recents.size()) *outFilename = recents[col].filename;
    }
}

LRESULT CALLBACK ContentWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_PAINT:
        OnContentPaint(hWnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
    {
        const int x = (int)(short)LOWORD(lParam);
        const int y = (int)(short)HIWORD(lParam);
        int r = -1, c = -1;
        HitTestCells(x, y, &r, &c);
        if (r != g_hoverRow || c != g_hoverCol)
        {
            g_hoverRow = r; g_hoverCol = c;
            InvalidateRect(hWnd, NULL, FALSE);
            DbgPrintf("[Palette] hover section=%d idx=%d (xy=%d,%d)\n", r, c, x, y);

            // TrackMouseEvent so we know when the cursor leaves the
            // control and can clear the hover state.
            TRACKMOUSEEVENT tme = {};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hWnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (g_hoverRow != -1 || g_hoverCol != -1)
        {
            g_hoverRow = -1; g_hoverCol = -1;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        // Forward Esc to the popup so the user can dismiss the popup
        // even when focus is on the content control (clicking a thumb
        // sets focus here). PopupWndProc handles Esc → toggle/hide.
        if (wParam == VK_ESCAPE)
        {
            HWND hPopup = GetParent(hWnd);
            if (hPopup != NULL) SendMessage(hPopup, WM_KEYDOWN, wParam, lParam);
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    {
        SetFocus(hWnd);
        const int x = (int)(short)LOWORD(lParam);
        const int y = (int)(short)HIWORD(lParam);

        // Star hit-test first (it overlaps a thumb cell, but star
        // wins when both apply).
        int r = -1, c = -1;
        if (HitTestStar(x, y, &r, &c))
        {
            std::wstring fn; SlotMask sl;
            ResolveCell(r, c, &fn, &sl);
            if (!fn.empty())
            {
                if (!Store::Instance().TogglePin(fn))
                {
                    ShowStatus(g_popup, IDS_PALETTE_PINS_FULL);
                }
                // Selection may have pointed at the moved entry;
                // clear it to avoid stale state.
                g_selRow = -1; g_selCol = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }

        // Thumb cell — selection only on single-click.
        if (HitTestCells(x, y, &r, &c))
        {
            std::wstring fn; SlotMask sl;
            ResolveCell(r, c, &fn, &sl);
            if (!fn.empty())
            {
                g_selRow = r; g_selCol = c;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK:
    {
        const int x = (int)(short)LOWORD(lParam);
        const int y = (int)(short)HIWORD(lParam);
        int r, c;
        if (HitTestCells(x, y, &r, &c))
        {
            std::wstring fn; SlotMask sl;
            ResolveCell(r, c, &fn, &sl);
            if (!fn.empty() && g_commitTarget != nullptr)
            {
                // Synchronous send — filename pointer is valid until
                // the handler returns.
                SendMessageW(g_commitTarget, WM_PALETTE_COMMIT,
                             (WPARAM)sl, (LPARAM)fn.c_str());
                // Close the popup after a successful commit. Most
                // users want to apply a texture and then continue
                // tweaking the emitter — keeping the palette open
                // would just obscure the viewport. They can reopen
                // it via the palette button if they want to swap again.
                HWND hPopup = GetParent(hWnd);
                if (hPopup != NULL) HidePopupAndReset(hPopup, "dblclk commit");
            }
        }
        return 0;
    }

    case WM_COMMAND:
    {
        if (HIWORD(wParam) == BN_CLICKED)
        {
            const UINT id = LOWORD(wParam);
            if (id == IDC_RADIO_PALETTE_COLOR || id == IDC_RADIO_PALETTE_BUMP)
            {
                SlotMask f = (id == IDC_RADIO_PALETTE_COLOR) ? SLOT_COLOR : SLOT_BUMP;
                Store::Instance().SetActiveFilter(f);
                // Clear hover + selection — both pointed at indices
                // valid for the previous filter's display list.
                g_selRow = -1; g_selCol = -1;
                g_hoverRow = -1; g_hoverCol = -1;
                InvalidateRect(hWnd, NULL, FALSE);
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// Popup window

void ShowStatus(HWND hPopup, UINT stringId)
{
    if (g_popupStatus == NULL) return;
    std::wstring text = LoadString(stringId);
    SetWindowTextW(g_popupStatus, text.c_str());
    // Clear after 3 s. Repeated calls extend the timer (SetTimer with
    // same ID resets it).
    SetTimer(hPopup, STATUS_CLEAR_TIMER_ID, 3000, NULL);
}

void ClearStatus(HWND hPopup)
{
    if (g_popupStatus != NULL) SetWindowTextW(g_popupStatus, L"");
    KillTimer(hPopup, STATUS_CLEAR_TIMER_ID);
}

// Centralize the hide path so transient state (hover, selection,
// status text, status timer) is reset consistently regardless of
// which trigger fired (button toggle, X, Esc).
void HidePopupAndReset(HWND hPopup, const char* reason)
{
    RECT r; GetWindowRect(hPopup, &r);
    Store::Instance().SetPopupPos(POINT{ r.left, r.top });
    ShowWindow(hPopup, SW_HIDE);
    ClearStatus(hPopup);
    g_hoverRow = -1; g_hoverCol = -1;
    g_selRow   = -1; g_selCol   = -1;
    DbgPrintf("[Palette] popup hide pos=(%d,%d) via %s\n", r.left, r.top, reason);
    if (g_visibilityCb) g_visibilityCb(false);
}

bool ValidateOnScreen(POINT p)
{
    // Validate against attached monitors. Nudge in a few pixels so
    // a position right on the boundary still counts.
    POINT probe { p.x + 4, p.y + 4 };
    return MonitorFromPoint(probe, MONITOR_DEFAULTTONULL) != NULL;
}

POINT ButtonAnchoredDefault(const RECT& buttonRectScreen)
{
    POINT p { buttonRectScreen.left, buttonRectScreen.bottom + 4 };
    return p;
}

LRESULT CALLBACK PopupWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        HINSTANCE hInst = (HINSTANCE)(LONG_PTR)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Content control fills the whole popup client area minus the
        // status strip at the bottom. Filter radios live INSIDE it as
        // children so sibling-overlap painting (which would otherwise
        // hide them behind content's WM_PAINT FillRect) isn't an issue.
        g_popupContent = CreateWindowExW(0, CLASS_CONTENT, NULL,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, CONTENT_W, CONTENT_H - STATUS_H - POPUP_MARGIN_Y,
            hWnd, (HMENU)(LONG_PTR)IDC_PAL_CONTENT, hInst, NULL);
        SendMessage(g_popupContent, WM_SETFONT, (WPARAM)hFont, FALSE);

        // Filter radios — children of the content control so they
        // sit ABOVE the FillRect background paint (sibling-of-popup
        // ordering would put them below).
        g_popupRadColor = CreateWindowExW(0, L"BUTTON", L"Color",
            WS_CHILD | WS_VISIBLE | WS_GROUP | BS_AUTORADIOBUTTON,
            POPUP_MARGIN_X, POPUP_MARGIN_Y + 2, 60, 16,
            g_popupContent, (HMENU)(LONG_PTR)IDC_RADIO_PALETTE_COLOR, hInst, NULL);
        g_popupRadBump = CreateWindowExW(0, L"BUTTON", L"Bump",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            POPUP_MARGIN_X + 70, POPUP_MARGIN_Y + 2, 60, 16,
            g_popupContent, (HMENU)(LONG_PTR)IDC_RADIO_PALETTE_BUMP, hInst, NULL);
        SendMessage(g_popupRadColor, WM_SETFONT, (WPARAM)hFont, FALSE);
        SendMessage(g_popupRadBump,  WM_SETFONT, (WPARAM)hFont, FALSE);

        // Status strip — child of the popup (sits below content).
        g_popupStatus = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            POPUP_MARGIN_X,
            CONTENT_H - STATUS_H - POPUP_MARGIN_Y + 2,
            CONTENT_W - 2 * POPUP_MARGIN_X, STATUS_H,
            hWnd, (HMENU)(LONG_PTR)IDC_PALETTE_STATUS, hInst, NULL);
        SendMessage(g_popupStatus, WM_SETFONT, (WPARAM)hFont, FALSE);

        RebuildLayout(g_popupContent);
        return 0;
    }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE)
        {
            HidePopupAndReset(hWnd, "Esc");
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_CLOSE)
        {
            HidePopupAndReset(hWnd, "X");
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == STATUS_CLEAR_TIMER_ID)
        {
            ClearStatus(hWnd);
            return 0;
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, STATUS_CLEAR_TIMER_ID);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ----------------------------------------------------------------------------
// Lazy popup creation

void EnsurePopupCreated(HWND ownerEditor)
{
    if (g_popup != NULL) return;

    HINSTANCE hInst = (HINSTANCE)(LONG_PTR)GetWindowLongPtrW(ownerEditor, GWLP_HINSTANCE);

    // Compute the window size from the desired client size + caption.
    RECT rc { 0, 0, CONTENT_W, CONTENT_H };
    AdjustWindowRectEx(&rc, WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_TOOLWINDOW);
    const int w = rc.right  - rc.left;
    const int h = rc.bottom - rc.top;

    g_popup = CreateWindowExW(
        WS_EX_TOOLWINDOW, CLASS_POPUP, L"Texture palette",
        WS_POPUPWINDOW | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h,
        ownerEditor, NULL, hInst, NULL);

    g_popupOwner = ownerEditor;
}

} // anonymous namespace

namespace TexturePalette {

bool Initialize(HINSTANCE hInst)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = ContentWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CLASS_CONTENT;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    WNDCLASSEXW wc2 = {};
    wc2.cbSize        = sizeof(wc2);
    wc2.style         = CS_DBLCLKS;
    wc2.lpfnWndProc   = PopupWndProc;
    wc2.hInstance     = hInst;
    wc2.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc2.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc2.lpszClassName = CLASS_POPUP;
    if (RegisterClassExW(&wc2) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;

    return true;
}

void SetServices(IFileManager* fileManager, IDirect3DDevice9* device)
{
    g_fileManager = fileManager;
    g_device      = device;
    // Device change invalidates the cache (HBITMAPs themselves don't
    // depend on D3D, but the source textures might no longer be loadable).
    for (auto& kv : g_thumbCache) if (kv.second) DeleteObject(kv.second);
    g_thumbCache.clear();
    if (g_phBroken)  { DeleteObject(g_phBroken);  g_phBroken  = NULL; }
    if (g_phMissing) { DeleteObject(g_phMissing); g_phMissing = NULL; }
}

void SetCommitTarget(HWND emitterPropsWnd)
{
    g_commitTarget = emitterPropsWnd;
}

void SetVisibilityCallback(VisibilityCallback cb)
{
    g_visibilityCb = cb;
}

void TogglePopup(HWND ownerEditor, const RECT& buttonRectScreen)
{
    EnsurePopupCreated(ownerEditor);
    if (g_popup == NULL) return;

    if (IsWindowVisible(g_popup))
    {
        HidePopupAndReset(g_popup, "button");
    }
    else
    {
        POINT fallback = ButtonAnchoredDefault(buttonRectScreen);
        POINT pos      = Store::Instance().GetPopupPos(fallback);
        if (!ValidateOnScreen(pos))
        {
            DbgPrintf("[Palette] popup position invalid (off-screen) snapping to default\n");
            pos = fallback;
        }
        SetWindowPos(g_popup, NULL, pos.x, pos.y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
        RebuildLayout(g_popupContent);
        // Give content keyboard focus so Esc immediately dismisses
        // the popup (without this, focus stays on the palette button
        // and Esc goes to DefWindowProc — i.e., nothing).
        SetFocus(g_popupContent);
        DbgPrintf("[Palette] popup show pos=(%d,%d)\n", pos.x, pos.y);
        if (g_visibilityCb) g_visibilityCb(true);
    }
}

bool IsPopupVisible()
{
    return g_popup != NULL && IsWindowVisible(g_popup) != 0;
}

void RefreshPopup()
{
    if (g_popup == NULL) return;
    // Hover/selection indices may now point at non-existent entries
    // (mod switch invalidates per-mod entry lists; commit re-orders
    // the recents list). Reset them defensively.
    g_hoverRow = -1; g_hoverCol = -1;
    g_selRow   = -1; g_selCol   = -1;
    if (g_popupContent != NULL) RebuildLayout(g_popupContent);
}

void ClearThumbnailCache()
{
    for (auto& kv : g_thumbCache) if (kv.second) DeleteObject(kv.second);
    g_thumbCache.clear();
    DbgPrintf("[Palette] thumbnail cache cleared\n");
}

} // namespace TexturePalette
