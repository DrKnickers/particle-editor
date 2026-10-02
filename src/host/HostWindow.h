// HostWindow — the top-level Win32 window for the new UI. Owns:
//   - the parent HWND (the editor's main window)
//   - the D3D9 viewport child HWND (sibling-of-WebView2 composition)
//   - the WebView2 controller + view, navigating to the bundled React app
//   - the live Engine instance (constructed with parent as hFocus and
//     viewport-child as hDevice, matching legacy main.cpp's wiring)
//   - the BridgeDispatcher, LayoutBroker, and AcceleratorBridge
//
// The implementation lives in HostWindow.cpp and HostWindow_*.cpp. Most of
// the composition code is a port of src/host/viewport_poc.cpp (commit
// cf39762, polished 4b23425) — including the two visual-gate fixes:
//   1) ICoreWebView2Controller2::put_DefaultBackgroundColor({0,0,0,0})
//   2) InvalidateRect on the viewport child after creation, to seed
//      the first paint and suppress the white-flash on startup.
#ifndef HOST_HOST_WINDOW_H
#define HOST_HOST_WINDOW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "HostLaunchOptions.h"

class ITextureManager;
class IShaderManager;
class IFileManager;

namespace host {

struct HostWindowImpl;

// HostWindow is a thin facade over `HostWindowImpl`, which is declared in the
// private HostWindowImpl.h and implemented across HostWindow.cpp and the
// HostWindow_*.cpp files. One host window exists per process: the WndProc
// thunks reach the implementation through a single global pointer.
class HostWindow
{
public:
    HostWindow(HINSTANCE hInstance,
               ITextureManager& textureManager,
               IShaderManager&  shaderManager,
               IFileManager&    fileManager,
               const std::vector<std::wstring>& gameRoots,
               const HostLaunchOptions& options);
    ~HostWindow();

    HostWindow(const HostWindow&)            = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // Registers window classes, creates the parent + viewport-child HWNDs,
    // initialises D3D9 + WebView2 + Engine, runs the message loop. Returns
    // WM_QUIT's wParam (process exit code).
    int Run(int nCmdShow);

private:
    std::unique_ptr<HostWindowImpl> m_impl;  // see HostWindowImpl.h
};

} // namespace host

#endif // HOST_HOST_WINDOW_H
