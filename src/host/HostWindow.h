// HostWindow — the editor's top-level Win32 window. Owns:
//   - the parent HWND (the editor's main window)
//   - the D3D9 viewport HWND: a hidden WS_POPUP owned by the parent that
//     only carries the D3D9 device; the engine frame is presented through
//     the DirectComposition tree (Compositor), behind the WebView2 visual
//   - the WebView2 composition controller + view, navigating to the
//     bundled React app
//   - the live Engine instance (constructed with parent as hFocus and
//     the viewport popup as hDevice)
//   - the BridgeDispatcher, LayoutBroker, and AcceleratorBridge
//
// The implementation lives in HostWindow.cpp and HostWindow_*.cpp. It grew
// out of an early composition proof of concept, and keeps that PoC's
// ICoreWebView2Controller2::put_DefaultBackgroundColor({0,0,0,0}) fix
// (a transparent WebView2 background).
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

    // Registers window classes, creates the parent + viewport popup HWNDs,
    // initialises D3D9 + WebView2 + Engine, runs the message loop. Returns
    // WM_QUIT's wParam (process exit code).
    int Run(int nCmdShow);

private:
    std::unique_ptr<HostWindowImpl> m_impl;  // see HostWindowImpl.h
};

} // namespace host

#endif // HOST_HOST_WINDOW_H
