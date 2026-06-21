#ifndef HOST_WINDOWCAPTURE_H
#define HOST_WINDOWCAPTURE_H

#include <windows.h>
#include <string>

namespace host {

// Capture the fully composed window (DWM/DirectComposition flatten of the
// WebView2 UI visual + the D3D9 engine visual) to a PNG via
// PrintWindow(PW_RENDERFULLCONTENT). GDI+ must already be initialized
// (GdiplusStartup) before calling. Returns true on success.
bool CaptureWindowToPng(HWND hwnd, const std::wstring& path);

// One-shot helper for the --snap-window CLI: find the top-level window of the
// given class, init GDI+, capture it, shut GDI+ down, and return a process exit
// code (0 = ok, 1 = window not found OR capture failed). No engine/WebView2.
int SnapWindowOneShot(const wchar_t* windowClass, const std::wstring& path);

}  // namespace host

#endif  // HOST_WINDOWCAPTURE_H
