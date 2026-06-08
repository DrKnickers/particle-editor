// Standard Win32 + COM + WebView2 + D3D9 PoC for the hybrid app
// composition strategy. NOT shipped — diagnostic-only binary used during
// the Phase 1 acceptance gate.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <wrl.h>
#include <d3d9.h>
#include "WebView2.h"
#include <string>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cwchar>

using namespace Microsoft::WRL;

namespace {
constexpr wchar_t kWindowClassName[]   = L"LT4ViewportPocMain";
constexpr wchar_t kViewportClassName[] = L"LT4ViewportPocChild";
constexpr int     kInitialWidth        = 1280;
constexpr int     kInitialHeight       = 800;

HWND                              g_mainWnd        = nullptr;
HWND                              g_viewportWnd    = nullptr;
ComPtr<ICoreWebView2Controller>   g_webviewController;
ComPtr<ICoreWebView2>             g_webview;
IDirect3D9*                       g_d3d            = nullptr;
IDirect3DDevice9*                 g_device         = nullptr;
FILE*                             g_logFile        = nullptr;

void LogDbg(const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fflush(g_logFile);
    }
}

void OpenLogFile() {
    wchar_t tempDir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tempDir) == 0) return;
    std::wstring logPath = std::wstring(tempDir) + L"viewport_poc.log";
    _wfopen_s(&g_logFile, logPath.c_str(), L"w");
    if (g_logFile) {
        LogDbg("[PoC] === viewport_poc started ===\n");
    }
}

void CloseLogFile() {
    if (g_logFile) {
        LogDbg("[PoC] === viewport_poc exiting ===\n");
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

// Compute the path to the React app's built dist directory.
// Walk up from x64/<Config>/viewport_poc.exe to the repo root, then descend
// into web/apps/viewport-poc/dist.
std::wstring GetWebAppPath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    auto root = p.parent_path().parent_path().parent_path();
    return (root / L"web" / L"apps" / L"viewport-poc" / L"dist").wstring();
}

LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
void HandleWebMessage(const std::wstring& json);
void ResizeWebViewToClient();

// -------- D3D9 device init / clear / present --------

bool InitD3D9(HWND hwnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) {
        LogDbg("[PoC] Direct3DCreate9 failed\n");
        return false;
    }

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed              = TRUE;
    pp.SwapEffect            = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat      = D3DFMT_UNKNOWN;
    pp.hDeviceWindow         = hwnd;
    pp.PresentationInterval  = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp.EnableAutoDepthStencil = FALSE;

    HRESULT hr = g_d3d->CreateDevice(
        D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
        D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device);
    if (FAILED(hr)) {
        LogDbg("[PoC] CreateDevice HWVP failed 0x%08lx, trying MIXED\n", hr);
        hr = g_d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_MIXED_VERTEXPROCESSING, &pp, &g_device);
    }
    if (FAILED(hr)) {
        LogDbg("[PoC] CreateDevice MIXED failed 0x%08lx, trying SOFTWARE\n", hr);
        hr = g_d3d->CreateDevice(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_device);
    }
    if (FAILED(hr)) {
        LogDbg("[PoC] CreateDevice failed all paths 0x%08lx\n", hr);
        g_d3d->Release();
        g_d3d = nullptr;
        return false;
    }
    LogDbg("[PoC] D3D9 device created OK\n");
    return true;
}

void RenderD3D9() {
    if (!g_device) return;
    g_device->Clear(0, nullptr, D3DCLEAR_TARGET,
                    D3DCOLOR_XRGB(20, 100, 200), 1.0f, 0);
    g_device->BeginScene();
    g_device->EndScene();
    g_device->Present(nullptr, nullptr, nullptr, nullptr);
}

LRESULT CALLBACK ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        RenderD3D9();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        // Suppress GDI erase — D3D9 owns the surface.
        return 1;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

// -------- WebView2 lifecycle --------

void HandleWebMessage(const std::wstring& json) {
    LogDbg("[PoC] WebMsg: %ls\n", json.c_str());
    // Minimal JSON parse — look for "layout/viewport-rect" with x/y/w/h ints.
    auto kindStart = json.find(L"\"kind\":\"");
    if (kindStart == std::wstring::npos) return;
    kindStart += 8;
    auto kindEnd = json.find(L'"', kindStart);
    if (kindEnd == std::wstring::npos) return;
    std::wstring kind = json.substr(kindStart, kindEnd - kindStart);
    if (kind != L"layout/viewport-rect") return;

    auto findInt = [&](const wchar_t* key) -> long {
        std::wstring needle = L"\"";
        needle += key;
        needle += L"\":";
        auto p = json.find(needle);
        if (p == std::wstring::npos) return 0;
        p += needle.length();
        return wcstol(json.c_str() + p, nullptr, 10);
    };
    long x = findInt(L"x");
    long y = findInt(L"y");
    long w = findInt(L"w");
    long h = findInt(L"h");
    LogDbg("[PoC] viewport rect: %ld,%ld %ldx%ld\n", x, y, w, h);

    // With PMv2 awareness, child-window coords are in physical pixels.
    // React sends device pixels; pass through to SetWindowPos.
    if (g_viewportWnd) {
        SetWindowPos(g_viewportWnd, HWND_TOP,
                     static_cast<int>(x), static_cast<int>(y),
                     static_cast<int>(w), static_cast<int>(h),
                     SWP_NOACTIVATE);
        InvalidateRect(g_viewportWnd, nullptr, FALSE);
    }
}

HRESULT InitWebView2(HWND parent) {
    std::wstring userDataFolder;
    {
        wchar_t buf[MAX_PATH];
        GetTempPathW(MAX_PATH, buf);
        userDataFolder = buf;
        userDataFolder += L"LT4ViewportPocWebView2Data";
    }

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [parent](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(envHr) || !env) {
                    LogDbg("[PoC] WebView2 env failed 0x%08lx\n", envHr);
                    return E_FAIL;
                }
                env->CreateCoreWebView2Controller(
                    parent,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT ctlHr, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(ctlHr) || !controller) {
                                LogDbg("[PoC] Controller failed 0x%08lx\n", ctlHr);
                                return E_FAIL;
                            }
                            g_webviewController = controller;
                            controller->get_CoreWebView2(&g_webview);

                            // Set WebView2 background to fully transparent so the
                            // D3D9 sibling child HWND is visible through the slot.
                            ComPtr<ICoreWebView2Controller2> ctrl2;
                            if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&ctrl2)))) {
                                COREWEBVIEW2_COLOR transparent = {};
                                transparent.A = 0;
                                transparent.R = 0;
                                transparent.G = 0;
                                transparent.B = 0;
                                ctrl2->put_DefaultBackgroundColor(transparent);
                                LogDbg("[PoC] WebView2 default bg set to transparent\n");
                            }

                            // Fit to client.
                            RECT bounds;
                            GetClientRect(g_mainWnd, &bounds);
                            controller->put_Bounds(bounds);

                            // Map virtual host to the dist directory.
                            ComPtr<ICoreWebView2_3> wv3;
                            g_webview.As(&wv3);
                            if (wv3) {
                                std::wstring distPath = GetWebAppPath();
                                LogDbg("[PoC] dist path: %ls\n", distPath.c_str());
                                wv3->SetVirtualHostNameToFolderMapping(
                                    L"app.local", distPath.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
                            }

                            // Receive JSON messages from React.
                            EventRegistrationToken tok;
                            g_webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR raw = nullptr;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            HandleWebMessage(raw);
                                            CoTaskMemFree(raw);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            // Navigate.
                            g_webview->Navigate(L"https://app.local/index.html");
                            LogDbg("[PoC] Navigate dispatched\n");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void ResizeWebViewToClient() {
    if (!g_webviewController) return;
    RECT r;
    GetClientRect(g_mainWnd, &r);
    g_webviewController->put_Bounds(r);
}

// -------- Main window --------

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Create the D3D9 viewport child as a sibling. Initial position
        // is a small visible rect so visual confirmation works even before
        // the React side sends its first layout message.
        g_viewportWnd = CreateWindowExW(
            0, kViewportClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            16, 16, 320, 240, hwnd, nullptr,
            GetModuleHandleW(nullptr), nullptr);
        if (!g_viewportWnd) {
            LogDbg("[PoC] CreateWindowEx viewport child failed (gle=%lu)\n", GetLastError());
            return -1;
        }
        if (!InitD3D9(g_viewportWnd)) {
            MessageBoxW(hwnd, L"D3D9 init failed.", L"PoC", MB_ICONERROR);
            return -1;
        }
        // Ensure viewport child sits on TOP in z-order so it punches through WebView2.
        SetWindowPos(g_viewportWnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        ResizeWebViewToClient();
        return 0;
    case WM_DESTROY:
        if (g_webviewController) {
            g_webviewController->Close();
            g_webviewController.Reset();
        }
        g_webview.Reset();
        if (g_device) {
            g_device->Release();
            g_device = nullptr;
        }
        if (g_d3d) {
            g_d3d->Release();
            g_d3d = nullptr;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}
} // namespace

// -------- WinMain --------

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    OpenLogFile();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LogDbg("[PoC] CoInitializeEx hr=0x%08lx\n", coHr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClassName;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{};
    vc.cbSize        = sizeof(vc);
    vc.lpfnWndProc   = ViewportWndProc;
    vc.hInstance     = hInst;
    vc.lpszClassName = kViewportClassName;
    vc.hbrBackground = nullptr;
    RegisterClassExW(&vc);

    g_mainWnd = CreateWindowExW(
        0, kWindowClassName, L"Viewport Composition PoC",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, kInitialWidth, kInitialHeight,
        nullptr, nullptr, hInst, nullptr);
    if (!g_mainWnd) {
        LogDbg("[PoC] CreateWindow failed (gle=%lu)\n", GetLastError());
        CloseLogFile();
        return 1;
    }

    HRESULT hr = InitWebView2(g_mainWnd);
    if (FAILED(hr)) {
        wchar_t msg[256];
        swprintf(msg, 256,
                 L"WebView2 init failed (0x%08lx).\nIs the Evergreen runtime installed?",
                 hr);
        MessageBoxW(g_mainWnd, msg, L"PoC", MB_ICONERROR);
        CloseLogFile();
        return 1;
    }

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);

    MSG mmsg;
    while (GetMessage(&mmsg, nullptr, 0, 0)) {
        TranslateMessage(&mmsg);
        DispatchMessage(&mmsg);
    }

    CoUninitialize();
    CloseLogFile();
    return static_cast<int>(mmsg.wParam);
}
