// WindowCapture — composed-window PNG capture, factored out of HostWindow.cpp
// so both the --capture composite snap and the --snap-window CLI (and the
// debug/capture-window bridge) can share one implementation. Pure move, no
// behavior change.
//
// Mirror HostWindow.cpp's modern-Windows target before including windows.h:
// PrintWindow(PW_RENDERFULLCONTENT) + DirectComposition capture need a Win8.1+
// SDK target, and the rest of host/ standardizes on 0x0A00.
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "WindowCapture.h"

#include <objbase.h>   // IStream / IUnknown for GDI+ (matches HostWindow.cpp)
#include <gdiplus.h>
#include "GdiplusEncode.h"   // host::GdiplusEncoderClsid (DRY cpp-host-1)
#include <cstdio>      // fwprintf / stderr for SnapWindowOneShot diagnostics
#include <algorithm>   // std::clamp (ProbeWindowMaxLuma region)
#include <vector>      // ProbeWindowMaxLuma pixel buffer

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

namespace host {

// PNG encoder-CLSID lookup now shared via host/GdiplusEncode.h
// (DRY audit cpp-host-1) — this used to keep its own uncached copy.

bool CaptureWindowToPng(HWND hwnd, const std::wstring& path)
{
    RECT rc = {};
    if (!hwnd || !GetWindowRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC     screen = GetDC(nullptr);
    HDC     mem    = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bmp    = (screen && mem) ? CreateCompatibleBitmap(screen, w, h) : nullptr;
    if (!screen || !mem || !bmp)
    {
        // Under handle exhaustion any of these can be null; without this guard
        // PrintWindow/Gdiplus::Bitmap would operate on null and return a valid-but-
        // BLANK PNG as success.
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ oldb   = SelectObject(mem, bmp);
    const BOOL pw  = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    SelectObject(mem, oldb);

    bool saved = false;
    if (pw)
    {
        CLSID clsid = {};
        if (host::GdiplusEncoderClsid(L"image/png", clsid))
        {
            Gdiplus::Bitmap gb(bmp, nullptr);
            saved = (gb.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok);
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return saved && pw;
}

int ProbeWindowMaxLuma(HWND hwnd, double x0, double y0, double x1, double y1)
{
    RECT rc = {};
    if (!hwnd || !GetWindowRect(hwnd, &rc)) return -1;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return -1;

    HDC     screen = GetDC(nullptr);
    HDC     mem    = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bmp    = (screen && mem) ? CreateCompatibleBitmap(screen, w, h) : nullptr;
    int result = -1;
    if (screen && mem && bmp)
    {
        HGDIOBJ oldb  = SelectObject(mem, bmp);
        const BOOL pw = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
        SelectObject(mem, oldb);
        if (pw)
        {
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth       = w;
            bi.bmiHeader.biHeight      = -h;  // top-down rows
            bi.bmiHeader.biPlanes      = 1;
            bi.bmiHeader.biBitCount    = 32;  // BGRA
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 4);
            if (GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h)
            {
                const int rx0 = std::clamp(static_cast<int>(x0 * w), 0, w);
                const int rx1 = std::clamp(static_cast<int>(x1 * w), 0, w);
                const int ry0 = std::clamp(static_cast<int>(y0 * h), 0, h);
                const int ry1 = std::clamp(static_cast<int>(y1 * h), 0, h);
                if (rx1 > rx0 && ry1 > ry0)
                {
                    int best = 0;
                    for (int y = ry0; y < ry1; ++y)
                    {
                        const unsigned char* row = px.data() + (static_cast<size_t>(y) * w + rx0) * 4;
                        for (int x = rx0; x < rx1; ++x, row += 4)
                        {
                            const int s = row[0] + row[1] + row[2];  // B+G+R
                            if (s > best) best = s;
                        }
                    }
                    result = best;
                }
            }
        }
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    if (screen) ReleaseDC(nullptr, screen);
    return result;
}

int SnapWindowOneShot(const wchar_t* windowClass, const std::wstring& path)
{
    // Match the host's DPI context so PrintWindow captures at true device pixels on
    // scaled displays (the host is PER_MONITOR_AWARE_V2; this one-shot runs before
    // host::Run so it must set it itself). Ignore failure (older OS) and proceed.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HWND hwnd = FindWindowW(windowClass, nullptr);
    // Prefer the foreground window when several instances are open.
    HWND fg = GetForegroundWindow();
    if (fg)
    {
        wchar_t cls[64] = {};
        if (GetClassNameW(fg, cls, 64) > 0 && wcscmp(cls, windowClass) == 0) hwnd = fg;
    }
    if (!hwnd)
    {
        fwprintf(stderr, L"snap-window: no window of class '%ls' is running\n", windowClass);
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok)
    {
        fwprintf(stderr, L"snap-window: GDI+ startup failed\n");
        return 1;
    }
    const bool ok = CaptureWindowToPng(hwnd, path);
    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);

    if (!ok) { fwprintf(stderr, L"snap-window: capture failed -> %ls\n", path.c_str()); return 1; }
    fwprintf(stderr, L"snap-window: wrote %ls\n", path.c_str());
    return 0;
}

}  // namespace host
