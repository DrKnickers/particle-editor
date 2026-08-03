#ifndef HOST_WINDOWCAPTURE_H
#define HOST_WINDOWCAPTURE_H

#include <windows.h>
#include <string>
#include <vector>

namespace host {

// [record-async-encode] Split of CaptureWindowToPng for the record loop's
// background PNG encoder (tasks/todo.md Branch B): the GRAB stays synchronous
// on the record thread (pixels captured at the same instant as the old inline
// path — determinism-neutral by construction); the encode+write moves to a
// worker. Grab = PrintWindow(PW_RENDERFULLCONTENT) + GetDIBits into top-down
// BGRA (stride w*4). Returns false on any capture/read failure.
bool GrabWindowPixels(HWND hwnd, std::vector<unsigned char>& bgra, int& w, int& h);

// Encode a GrabWindowPixels buffer to PNG. GDI+ must be initialized and the
// "image/png" encoder CLSID pre-warmed on the UI thread before calling this
// off-thread (GdiplusEncode.h's CLSID cache is not safe for a concurrent
// FIRST call — see its header note). Alpha is ignored (32bppRGB): the DDB
// alpha channel PrintWindow leaves behind is undefined.
bool EncodeBgraToPng(const unsigned char* bgra, int w, int h, const std::wstring& path);

// Capture the fully composed window (DWM/DirectComposition flatten of the
// WebView2 UI visual + the D3D9 engine visual) to a PNG via
// PrintWindow(PW_RENDERFULLCONTENT). GDI+ must already be initialized
// (GdiplusStartup) before calling. Returns true on success.
bool CaptureWindowToPng(HWND hwnd, const std::wstring& path);

// One-shot helper for the --snap-window CLI: find the top-level window of the
// given class, init GDI+, capture it, shut GDI+ down, and return a process exit
// code (0 = ok, 1 = window not found OR capture failed). No engine/WebView2.
int SnapWindowOneShot(const wchar_t* windowClass, const std::wstring& path);

// --drive assert-viewport-nonblack probe: PrintWindow the composed window and
// return the max R+G+B (0..765) inside the fractional region [x0,y0)x[x1,y1)
// of the window rect, or -1 on any capture/read failure. Fresh capture per
// call (no caching) — pure GDI (GetDIBits), no GDI+ needed.
int ProbeWindowMaxLuma(HWND hwnd, double x0, double y0, double x1, double y1);

}  // namespace host

#endif  // HOST_WINDOWCAPTURE_H
