// HostWindow — the top-level Win32 window for the new UI. Owns:
//   - the parent HWND (the editor's main window when --new-ui is active)
//   - the D3D9 viewport child HWND (sibling-of-WebView2 composition)
//   - the WebView2 controller + view, navigating to the bundled React app
//   - the live Engine instance (constructed with parent as hFocus and
//     viewport-child as hDevice, matching legacy main.cpp's wiring)
//   - the BridgeDispatcher, LayoutBroker, and AcceleratorBridge
//
// The implementation lives in HostWindow.cpp. Most of the composition
// code is a port of src/host/viewport_poc.cpp (commit cf39762, polished
// 4b23425) — including the two visual-gate fixes:
//   1) ICoreWebView2Controller2::put_DefaultBackgroundColor({0,0,0,0})
//   2) InvalidateRect on the viewport child after creation, to seed
//      the first paint and suppress the white-flash on startup.
#ifndef HOST_HOST_WINDOW_H
#define HOST_HOST_WINDOW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

class ITextureManager;
class IShaderManager;
class IFileManager;

namespace host {

// HostWindow is a thin facade. The whole implementation lives as
// `HostWindowImpl` in the .cpp (file-local because the WndProc thunks
// need a fixed global pointer and the lifetime of one host window per
// process is enforced by Task 1.3's scope). This header only exists so
// other TUs can spell the type if Task 2.x decides to expose it.
class HostWindow
{
public:
    HostWindow(HINSTANCE hInstance,
               ITextureManager& textureManager,
               IShaderManager&  shaderManager,
               IFileManager&    fileManager,
               const std::vector<std::wstring>& gameRoots,
               bool useDevUi   = false,
               bool useTestHost = false,
               const std::wstring& captureAlo = L"",
               const std::wstring& capturePng = L"",
               int captureFrames = 60,
               int captureSkydome = 0);
    ~HostWindow();

    HostWindow(const HostWindow&)            = delete;
    HostWindow& operator=(const HostWindow&) = delete;

    // Registers window classes, creates the parent + viewport-child HWNDs,
    // initialises D3D9 + WebView2 + Engine, runs the message loop. Returns
    // WM_QUIT's wParam (process exit code).
    int Run(int nCmdShow);

private:
    void* m_impl;  // opaque HostWindowImpl* (see HostWindow.cpp)
};

} // namespace host

#endif // HOST_HOST_WINDOW_H
