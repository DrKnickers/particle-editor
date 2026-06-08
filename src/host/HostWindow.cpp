// HostWindow — see HostWindow.h for the design overview.
//
// Most of this file is a port of src/host/viewport_poc.cpp, split into
// instance methods on a singleton-style HostWindow + Impl pair. The PoC
// proved the composition pattern (WebView2 surface set transparent, D3D9
// sibling child HWND layered on top, layout/viewport-rect drives
// SetWindowPos). We carry those decisions forward verbatim.
//
// IMPORTANT: this TU upgrades _WIN32_WINNT to 0x0A00 (Windows 10) before
// including windows.h. The rest of the project targets XP-era APIs;
// WebView2 + DPI awareness need a modern target.
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // Phase 3 Stage 3c: GET_X_LPARAM / GET_Y_LPARAM for mouse forwarding
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <wrl/implements.h>
#include <d3d9.h>
#include <winhttp.h>
#include <shlwapi.h>  // Phase 0: SHCreateMemStream for WebResourceRequested response
#include <dwmapi.h>   // title-bar dark-mode (DWMWA_USE_IMMERSIVE_DARK_MODE)
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")

// See BridgeDispatcher.cpp for the runtime (theme-toggle) title-bar sync;
// this is the startup default. Guarded for older SDKs (value 20 on modern
// Windows, which the editor targets via WebView2 + DComp).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>     // _wgetenv / _wtoi for ALO_HOSTING_MODE etc.
#include <cstring>
#include <share.h>     // Stage 4f: _SH_DENYNO for _wfsopen sharing
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "HostWindow.h"
#include "Run.h"

#include "AcceleratorBridge.h"
#include "AlphaCompositor.h"
#include "Compositor.h"
#include "FramePublisher.h"
#include "InputDispatcher.h"

#include <objbase.h>
#include <gdiplus.h>
#include "BridgeDispatcher.h"
#include "HostBridgeProxy.h"
#include "LayoutBroker.h"

#include "../engine.h"
#include "../managers.h"
#include "../ModManager.h"
#include "../MouseCursor.h"
#include "../ParticleSystem.h"
#include "../ParticleSystemIO.h"
#include "../ParticleSystemInstance.h"
#include "../SpawnerDriver.h"
#include "../UndoStack.h"
#include "../Autosave.h"  //: two-tier autosave timers + clean-exit cleanup

using namespace Microsoft::WRL;

namespace host {

namespace {

constexpr wchar_t kHostWindowClassName[]     = L"AloHostMain";
constexpr wchar_t kHostViewportClassName[]   = L"AloHostViewport";
constexpr int     kInitialWidth              = 1280;
constexpr int     kInitialHeight             = 800;
constexpr wchar_t kVirtualHostName[]         = L"app.local";
constexpr INTERNET_PORT kDevServerPort       = 5174;
constexpr UINT_PTR    kStatsTimerId          = 0x100;  // 4 Hz stats broadcast

// G11: WebView2 origin allow-list. The host must trust only the
// page it deliberately loads, not "whatever is currently navigated". Three
// origins are legitimate:
//   - https://app.local/     prod: the SetVirtualHostNameToFolderMapping
//                            origin (kVirtualHostName) serving the bundled
//                            web/apps/editor/dist.
//   - http://localhost:5174/ dev: the Vite HMR server (kDevServerPort), only
//                            when --dev-ui is active.
//   - about:                 WebView2's own about:blank initial navigation.
// The trailing '/' on the two host prefixes is load-bearing: it stops a
// lookalike like https://app.local.evil.test/ from slipping through. Scheme
// and host compare case-insensitively per RFC 3986, hence _wcsnicmp. Used by
// add_NavigationStarting (cancel off-origin nav) and the WebMessageReceived
// handler (drop messages from an untrusted document source).
bool IsApprovedWebViewOrigin(PCWSTR uri, bool devUi)
{
    if (!uri) return false;
    const auto hasPrefix = [uri](PCWSTR prefix) -> bool
    {
        return _wcsnicmp(uri, prefix, wcslen(prefix)) == 0;
    };
    if (hasPrefix(L"https://app.local/")) return true;
    if (hasPrefix(L"about:"))             return true;
    if (devUi && hasPrefix(L"http://localhost:5174/")) return true;
    return false;
}

// FPSMeasurer — ring-buffer of the last 32 frame timestamps. Originally
// ported from the legacy src/main.cpp `FPSMeasurer` (lines 56-99), but
// swapped GetTickCount() for QueryPerformanceCounter so the math
// stays meaningful in's uncapped (no-vsync) UpdateLayeredWindow
// rendering regime. GetTickCount's ~15.6 ms resolution is too coarse
// when the renderer pegs at hundreds of FPS: 32 frames can fit inside
// 0–2 ticks, producing fps readings that snap between 0 (zero-diff
// guard) and 1024 (32 frames / 0.03 s). QPC has sub-microsecond
// resolution and is free.
class FPSMeasurer
{
    static const int MAX_FRAMES = 32;
    LONGLONG m_frames[MAX_FRAMES];   // QPC tick values
    LONGLONG m_qpcFrequency;          // ticks per second
    size_t   m_iFrame;
    size_t   m_nFrames;
    size_t   m_lastFrame;
    size_t   m_firstFrame;
public:
    float getFPS()
    {
        if (m_nFrames > 0 && m_qpcFrequency > 0)
        {
            const LONGLONG diff = m_frames[m_lastFrame] - m_frames[m_firstFrame];
            if (diff > 0)
                return static_cast<float>(m_nFrames) * static_cast<float>(m_qpcFrequency) / static_cast<float>(diff);
        }
        return 0.0f;
    }
    void measure()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        m_lastFrame        = m_iFrame;
        m_frames[m_iFrame] = t.QuadPart;
        m_nFrames          = m_nFrames < MAX_FRAMES ? m_nFrames + 1 : MAX_FRAMES;
        m_iFrame           = (m_iFrame + 1) % MAX_FRAMES;
        if (m_iFrame == m_firstFrame)
            m_firstFrame = (m_firstFrame + 1) % MAX_FRAMES;
    }
    FPSMeasurer() : m_qpcFrequency(0), m_iFrame(0), m_nFrames(0), m_lastFrame(0), m_firstFrame(0)
    {
        memset(m_frames, 0, sizeof(m_frames));
        LARGE_INTEGER freq;
        if (QueryPerformanceFrequency(&freq)) m_qpcFrequency = freq.QuadPart;
    }
};

// [PERF] per-stage frame timing. QPC microsecond helpers + a tiny
// per-stage accumulator. Always-on (QPC is ~20 ns/call, ~6 calls/frame),
// emitted to host.log at 1 Hz under the [PERF] prefix to localise which
// composition-path stage's cost scales with window area. See
// tasks/todo.md (measurement round). The QPC frequency is fixed for the
// process lifetime, so cache it once.
static LONGLONG PerfQpcFreq()
{
    static LONGLONG freq = 0;
    if (freq == 0) { LARGE_INTEGER f; if (QueryPerformanceFrequency(&f)) freq = f.QuadPart; }
    return freq;
}
static LONGLONG PerfQpcNow()
{
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}
static double PerfUsSince(LONGLONG start)
{
    const LONGLONG f = PerfQpcFreq();
    if (f <= 0) return 0.0;
    return static_cast<double>(PerfQpcNow() - start) * 1.0e6 / static_cast<double>(f);
}
struct PerfStage
{
    double   sumUs = 0.0;
    double   maxUs = 0.0;
    unsigned n     = 0;
    void   add(double us) { sumUs += us; if (us > maxUs) maxUs = us; ++n; }
    double avg() const    { return n ? sumUs / n : 0.0; }
    void   reset()        { sumUs = 0.0; maxUs = 0.0; n = 0; }
};

// Probe the installed WebView2 Evergreen runtime. Returns true if
// GetAvailableCoreWebView2BrowserVersionString succeeds and returns a
// non-empty version string. Call AFTER CoInitializeEx so that
// ShellExecuteW works cleanly in the error branch, but BEFORE any
// window creation so the dialog is the only visible artifact when the
// runtime is absent.
static bool WebView2RuntimeInstalled()
{
    LPWSTR versionInfo = nullptr;
    HRESULT hr = GetAvailableCoreWebView2BrowserVersionString(nullptr, &versionInfo);
    bool installed = SUCCEEDED(hr) && versionInfo != nullptr && versionInfo[0] != L'\0';
    if (versionInfo) CoTaskMemFree(versionInfo);
    return installed;
}

// Probe the Vite dev server at http://localhost:5174/. Used when
// --dev-ui is active to verify the server is listening before
// navigating. Returns true only if a 2xx response is received.
// Short timeouts (≤2 s total) so startup never hangs.
bool ProbeDevServer()
{
    HINTERNET hSession = WinHttpOpen(L"AloParticleEditor-DevProbe",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    // resolve: 1 s, connect: 1 s, send: 1.5 s, receive: 1.5 s
    WinHttpSetTimeouts(hSession, 1000, 1000, 1500, 1500);

    HINTERNET hConnect = WinHttpConnect(hSession, L"localhost", kDevServerPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = false;
    BOOL sent = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (sent && WinHttpReceiveResponse(hRequest, nullptr))
    {
        DWORD statusCode = 0, len = sizeof(statusCode);
        if (WinHttpQueryHeaders(hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &len,
                WINHTTP_NO_HEADER_INDEX))
        {
            ok = (statusCode >= 200 && statusCode < 300);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

// Walk up from x64/<Config>/ParticleEditor.exe to the repo root, then
// descend to web/apps/editor/dist (Vite's build output). Same pattern as
// viewport_poc, just a different sub-path.
std::wstring ComputeEditorDistPath()
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path p(exePath);
    auto root = p.parent_path().parent_path().parent_path();
    return (root / L"web" / L"apps" / L"editor" / L"dist").wstring();
}

// WebView2 user-data folder under %LOCALAPPDATA%. We use a stable,
// production-quality location (not %TEMP%) so the runtime can persist
// IndexedDB / cache across launches.
std::wstring ComputeUserDataFolder()
{
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))
        && localAppData)
    {
        std::wstring folder = localAppData;
        CoTaskMemFree(localAppData);
        folder += L"\\AloParticleEditor\\WebView2";
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr); // best-effort
        return folder;
    }
    // Fallback to temp.
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"AloParticleEditor_WebView2";
}

// Log file under %LOCALAPPDATA%\AloParticleEditor\host.log — handy for
// diagnostics when there's no debugger attached.
std::wstring ComputeHostLogPath()
{
    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))
        && localAppData)
    {
        std::wstring path = localAppData;
        CoTaskMemFree(localAppData);
        path += L"\\AloParticleEditor";
        SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        return path + L"\\host.log";
    }
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"AloParticleEditor_host.log";
}

// Convert UTF-16 → UTF-8 via WideCharToMultiByte. Used to hand WebView2
// strings to BridgeDispatcher, and to hand the Win32 EXEPATH to
// ComputeEditorDistPath's spdlog-style debug printf.
std::string Utf16ToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToUtf16(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                  nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

LRESULT CALLBACK HostMainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HostViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Custom message dispatched from OnCompositionControllerReady when async
// composition setup fails. wParam carries the failure HRESULT. Handled
// in MainWndProc to tear down partial composition state and re-dispatch
// to HWND mode via the stashed webEnv. The pre-dispatch sync failures
// (Compositor::Init, QI Environment3) already fall back inline; this
// message closes the analogous hole for failures that happen AFTER
// CreateCoreWebView2CompositionController has been dispatched. (Post-
// audit F8.)
static const UINT WM_APP_COMPOSITION_FALLBACK = WM_APP + 1;

} // namespace

// -----------------------------------------------------------------------------
// Impl
// -----------------------------------------------------------------------------

// File-scope pointer chased by the WndProc thunks below. Set by
// HostWindowImpl::Run before any window is created, cleared after the
// message loop returns. Single-instance is fine because Task 1.3
// only ever runs one host window per process.
struct HostWindowImpl;
HostWindowImpl* g_self = nullptr;

struct HostWindowImpl
{
    HINSTANCE        hInstance;
    HWND             hMain         = nullptr;
    HWND             hViewport     = nullptr;

    // render loop: the host no longer maintains its own placeholder
    // D3D9 device. The Engine constructs the live device internally (via
    // its `(hFocus, hDevice)` ctor) and we render through `engine->Render()`.
    // Running two D3D9 devices targeting the same HWND was a structural
    // hazard; dropping the placeholder is the cleanest option since Engine
    // is constructed unconditionally in WM_CREATE.

    ComPtr<ICoreWebView2Controller> webController;
    ComPtr<ICoreWebView2>           webView;
    // Phase 0: needed by the WebResourceRequested handler to
    // construct the response stream via env->CreateWebResourceResponse.
    ComPtr<ICoreWebView2Environment> webEnv;
    EventRegistrationToken          accelKeyTok = {};
    // G5: stash the WebMessageReceived registration token so
    // WM_DESTROY can explicitly remove the handler before tearing down
    // webView. Pre-fix the token was a local in InitWebView2 and the
    // handler stayed subscribed (the lambda captures `this`) — masked
    // today by webView.Reset(), but the explicit-unsubscribe pattern
    // mirrors accelKeyTok above and is materially safer.
    EventRegistrationToken          webMessageTok = {};
    // G11: navigation / new-window / permission policy tokens.
    // Registered alongside webMessageTok in InitWebView2 and removed in
    // WM_DESTROY (mirroring the G5 webMessageTok lifecycle). The handlers
    // enforce the IsApprovedWebViewOrigin allow-list (cancel off-origin
    // top-level navigation), deny all popups, and deny every permission
    // request — defence-in-depth against a redirected/compromised renderer.
    EventRegistrationToken          navStartingTok = {};
    EventRegistrationToken          newWindowTok   = {};
    EventRegistrationToken          permissionTok  = {};
    // F10: TME_LEAVE arming state. WebView2 needs a
    // COREWEBVIEW2_MOUSE_EVENT_KIND_MOUSE_LEAVE input when the pointer
    // exits the host HWND so CSS :hover / cursor state clears. Re-arm
    // on each WM_MOUSEMOVE after the leave fires.
    bool                            m_mouseTracked = false;
    // G8: owned class background brush. Created in Run(),
    // released in WM_DESTROY.
    HBRUSH                          m_classBrush = nullptr;

    ITextureManager& textureManager;
    IShaderManager&  shaderManager;
    IFileManager&    fileManager;
    std::unique_ptr<Engine> engine;

    //: layered-window alpha compositor. Constructed after the
    // Engine (needs its D3D9 device), torn down before the Engine in
    // WM_DESTROY so Engine never dereferences a freed compositor.
    std::unique_ptr<host::AlphaCompositor> alphaCompositor;

    // host-state plumbing — the new-UI host owns the live
    // ParticleSystem (replaced on file/new and file/open) and a single
    // SpawnerDriver (config mutated via SetConfig). The BridgeDispatcher
    // gets pointer-to-pointer access via BindHostState so its handlers
    // can read/write through the host's owned slots.
    //
    // Render loop wiring: RenderD3D9 drives SpawnerDriver::Tick and
    // engine->Update / engine->Render per frame; file/new and file/open
    // call engine->Clear + engine->OnParticleSystemChanged(-1) after
    // swapping the unique_ptr so the engine drops cached per-instance
    // state for the old system.
    std::unique_ptr<ParticleSystem> particleSystem;
    std::unique_ptr<SpawnerDriver>  spawnerDriver;

    // Undo / redo stack. Task 2.4: constructed here so BridgeDispatcher
    // can service `undo/perform` requests. Captures are not yet wired
    // through the new-UI bridge surface (Phase 3 emitter work), so the
    // stack stays empty for now and `undo/perform` resolves with
    // `applied: false`. The plumbing exists so Phase 3 wraps the
    // engine setter handlers in Capture() without re-touching this file.
    UndoStack                          undoStack;

    LayoutBroker                       layout;
    AcceleratorBridge                  accelerator;
    std::unique_ptr<BridgeDispatcher>  dispatcher;
    FPSMeasurer                        fpsMeasurer;

    // [PERF] per-stage frame-timing accumulators. Reset each 1 Hz
    // emit in RenderD3D9. Always-on; see tasks/todo.md (measurement round).
    PerfStage          perfUpdate, perfRender, perfWait, perfComposite, perfFrame;
    // [PERF2] round-2 — engine Render() per-pass sub-timing (us).
    PerfStage          perfRScene, perfRBloom, perfRDistort, perfRCompose, perfRPresent;
    unsigned long long perfWaitSpinsSum = 0;
    unsigned           perfWaitSpinsMax = 0;
    DWORD              perfLastEmitTick = 0;

    // D6: mod state shared with React. ModManager constructed in
    // the impl ctor (DiscoverMods + RestoreLastSelectedMod run before
    // any UI shows); SetEngine called in WM_CREATE once the Engine
    // exists. Passed to BridgeDispatcher via SetModManager.
    std::unique_ptr<::ModManager>      modManager;

    // render loop bookkeeping. m_lastRenderTime drives dt for the
    // per-frame SpawnerDriver::Tick — matches the legacy
    // `g_spawnerLastFrameTime` flow in src/main.cpp:1572. First frame
    // sees dt == 0 (sentinel value 0.0f means "not yet rendered"), same
    // as the legacy first-frame initialisation.
    //
    // m_lastEmittedActiveCount debounces the spawner/active-count event:
    // we only emit when Engine::GetNumInstances() actually changes,
    // since the source is polled every render frame and we don't want
    // to flood WebMessage. -1 forces an initial emit on first non-zero
    // change.
    float                              m_lastRenderTime        = 0.0f;
    int                                m_lastEmittedActiveCount = -1;

    // viewport interaction (camera controls). Mirror of legacy
    // src/main.cpp:2920-3060 drag-state. On WM_LBUTTONDOWN /
    // WM_RBUTTONDOWN we snapshot the camera + cursor XY, then
    // WM_MOUSEMOVE deltas are applied relative to the snapshot
    // (matches legacy "drag relative to start" feel — releasing and
    // re-pressing resets the reference frame). NONE means no drag in
    // progress; the wheel handler only fires when dragMode == NONE.
    // OBJECT_Z: cursor-bound preview is being dragged for placement.
    // Only Z (height) tracks the drag delta; X/Y stay frozen at the
    // click position. WM_LBUTTONUP detaches the preview (place it).
    // Matches legacy src/main.cpp:2891-2898 + 2877-2883 + 2936-2948.
    enum class DragMode { NONE, MOVE, ROTATE, ZOOM, OBJECT_Z };
    DragMode        m_dragMode      = DragMode::NONE;
    Engine::Camera  m_dragStartCam  = {};
    int             m_dragStartX    = 0;
    int             m_dragStartY    = 0;

    // shift-click-to-spawn. Mirror of legacy
    // `info->mouseCursor` + `info->attachedParticleSystem` at
    // src/main.cpp:369-399 / 2945-2966.
    //
    // m_mouseCursor: Object3D whose position is set from screen-space
    // mouse moves (WM_MOUSEMOVE → GetCursorPos3D unproject) and whose
    // velocity is derived from QueryPerformanceCounter deltas in
    // UpdateVelocity() (called once per RenderD3D9).
    //
    // m_attachedParticleSystem: non-null between Shift-press (spawn) and
    // Shift-release (kill). Engine returns a pointer we keep until we
    // KillParticleSystem it.
    //
    // m_lastCursorX/Y: cache of the most recent (x,y) seen by
    // WM_MOUSEMOVE. Used as the spawn coords on WM_KEYDOWN VK_SHIFT
    // because WM_KEYDOWN's lParam is NOT cursor coords (legacy bug at
    // src/main.cpp:2960 passes garbage). Fallback if the cache is
    // stale: GetCursorPos + ScreenToClient.
    MouseCursor             m_mouseCursor;
    ParticleSystemInstance* m_attachedParticleSystem = nullptr;
    int                     m_lastCursorX = 0;
    int                     m_lastCursorY = 0;
    // (Group A): last GetTickCount() at which we pushed a
    // `cursor/position-3d` event. Throttled to ~30 Hz so the
    // WebView2 message channel isn't saturated by WM_MOUSEMOVE
    // (which fires per-pixel). The legacy status bar updates per
    // WM_MOUSEMOVE since SendMessage is free in-process; over the
    // bridge a 33 ms minimum interval is a good compromise.
    DWORD                   m_lastCursorEmitTick = 0;

    // Phase 1: canvas-in-DOM transport state.
    //
    // m_archCMode is the env-var-gated kill switch; when true and the
    // AlphaCompositor is up, m_framePublisher is constructed alongside
    // it in WM_CREATE. Camera input still flows through the visible
    // popup HWND during Phase 1 (Phase 2 will route input through the
    // canvas + a new viewport/input bridge surface).
    bool                                m_archCMode    = false;
    int                                 m_archCQuality = 70;  // JPEG quality 1..100
    std::unique_ptr<host::FramePublisher> m_framePublisher;

    // Phase 2: viewport/input bridge surface owner. Constructed
    // alongside FramePublisher when m_archCMode is true; holds a raw
    // HWND for the popup it PostMessages to. BridgeDispatcher gets a
    // borrow via SetInputDispatcher.
    std::unique_ptr<host::InputDispatcher> m_inputDispatcher;

    // Phase 3 Stage 3: WebView2 composition hosting.
    //
    // m_compositionMode is the env-var-gated mode flag (set in the
    // ctor from ALO_HOSTING_MODE; defaults to true under, opt
    // out via ALO_HOSTING_MODE=legacy). When true, InitWebView2 takes
    // the CreateCoreWebView2CompositionController path instead of
    // CreateCoreWebView2Controller, and a host::Compositor owns the
    // DirectComposition visual tree that WebView2 plugs into via
    // put_RootVisualTarget.
    //
    // m_compositionController is the composition-mode controller
    // returned by CreateCoreWebView2CompositionController. We also QI
    // it to ICoreWebView2Controller and store in `webController` so
    // every existing wire-up (put_Bounds, AcceleratorKeyPressed, etc.)
    // works unchanged. Kept here so WM_DESTROY can release the
    // composition-specific reference before releasing the base
    // controller (the teardown ordering matters per the spike's
    // Shutdown sequence in dxgi_spike.cpp:783).
    bool                                       m_compositionMode = false;
    std::unique_ptr<host::Compositor>          m_compositor;
    ComPtr<ICoreWebView2CompositionController> m_compositionController;

    // Phase 3 Stage 3d: cursor sync. Under HWND hosting,
    // WebView2's child HWND owns the cursor via its own WM_SETCURSOR
    // handler. Under composition hosting the host HWND receives
    // WM_SETCURSOR and must consult the composition controller for
    // the desired cursor (pointer for links, I-beam for inputs, etc).
    // The composition controller fires add_CursorChanged whenever
    // its desired cursor changes; we cache the HCURSOR here and
    // return it on the next WM_SETCURSOR.
    //
    // The cursor HCURSOR is owned by WebView2 — we MUST NOT call
    // DestroyCursor on it. Treat as a borrowed handle valid until
    // the next add_CursorChanged event.
    HCURSOR                                    m_webViewCursor       = nullptr;
    EventRegistrationToken                     m_cursorChangedTok    = {};

    bool        useDevUi   = false;  // --dev-ui: navigate to Vite HMR server
    bool        useTestHost = false; // --test-host: CDP :9222 + DevTools
    // rendering-fidelity] --capture mode: load m_captureAlo,
    // render m_captureFrames frames, write engine RT to m_capturePng,
    // then quit. Both paths empty = normal interactive run.
    std::wstring m_captureAlo;
    std::wstring m_capturePng;
    int          m_captureFrames = 60;
    // --skydome <slot>: apply this skydome slot in --capture mode before
    // rendering (0 = Off / solid colour, the default).
    int          m_captureSkydomeSlot = 0;
    FILE*       logFile = nullptr;
    std::mutex  logMutex;

    HostWindowImpl(HINSTANCE inst,
                   ITextureManager& tex,
                   IShaderManager&  shd,
                   IFileManager&    fil,
                   const std::vector<std::wstring>& gameRoots_,
                   bool devUi    = false,
                   bool testHost = false,
                   const std::wstring& captureAlo = L"",
                   const std::wstring& capturePng = L"",
                   int captureFrames = 60,
                   int captureSkydome = 0)
        : hInstance(inst)
        , textureManager(tex)
        , shaderManager(shd)
        , fileManager(fil)
        , useDevUi(devUi)
        , useTestHost(testHost)
        , m_captureAlo(captureAlo)
        , m_capturePng(capturePng)
        , m_captureFrames(captureFrames)
        , m_captureSkydomeSlot(captureSkydome)
        , layout(nullptr)
        , accelerator()
        , modManager(std::make_unique<ModManager>(&fil, gameRoots_))
    {
        // D6: discover installed mods and restore the
        // previously-active one from the registry before any UI shows.
        // Both calls are quick; they don't touch GPU / WebView2 state.
        // Engine pointer is bound later via SetEngine() in WM_CREATE.
        modManager->DiscoverMods();
        modManager->RestoreLastSelectedMod();

        // Default rendering path is architecture C (DXGI
        // composition + DComp engine visual + WebView2 composition
        // hosting). Opt out via ALO_HOSTING_MODE=legacy → architecture
        // A (AlphaCompositor popup + HWND-hosted WebView2, the
        // pre-default). Unknown values warn and fall through to
        // default. See ROADMAP §5.x (architecture-C wire-up)
        // and (default flip + dual-env-var retirement).
        m_archCMode = true;
        m_compositionMode = true;
        if (const wchar_t* v = _wgetenv(L"ALO_HOSTING_MODE"))
        {
            if (wcscmp(v, L"legacy") == 0)
            {
                m_archCMode = false;
                m_compositionMode = false;
            }
            else if (v[0] != L'\0' && wcscmp(v, L"composition") != 0)
            {
                fprintf(stderr,
                    "[host] WARNING: ALO_HOSTING_MODE=\"%ls\" unrecognized; "
                    "valid values: \"composition\" (default) or \"legacy\". "
                    "Falling through to default (composition).\n", v);
                fflush(stderr);
            }
        }

        // Boot-mode log line — unconditional (release builds
        // too) so issue reports include the active mode in their
        // first log line. Cost: one printf per process launch.
        fprintf(stderr, "[host] hosting mode: %s\n",
                m_compositionMode ? "composition (architecture C, default)"
                                  : "legacy (architecture A, opt-out)");
        fflush(stderr);

        // Deprecated env-var detection (R7 mitigation). The
        // four-var toggle (ALO_WEBVIEW2_HOSTING + ALO_VIEWPORT_TRANSPORT
        // and their VITE_* twins) was retired by; warn loudly
        // if any is still set so users update muscle memory and shell
        // scripts. Remove in the future-style cleanup that
        // deletes architecture A entirely.
        for (const wchar_t* deprecated : { L"ALO_WEBVIEW2_HOSTING",
                                           L"ALO_VIEWPORT_TRANSPORT" })
        {
            if (const wchar_t* v = _wgetenv(deprecated))
            {
                if (v[0] != L'\0')
                {
                    fprintf(stderr,
                        "[host] WARNING: %ls=\"%ls\" is set, but this env "
                        "var was retired by. Use ALO_HOSTING_MODE "
                        "(values: \"composition\" default or \"legacy\") "
                        "instead. Ignoring %ls.\n",
                        deprecated, v, deprecated);
                    fflush(stderr);
                }
            }
        }

        if (const wchar_t* q = _wgetenv(L"ALO_VIEWPORT_JPEG_Q"))
        {
            int n = _wtoi(q);
            if (n >= 1 && n <= 100) m_archCQuality = n;
        }
    }

    void Log(const char* fmt, ...);
    void OpenLog();
    void CloseLog();

    //: InitD3D9 dropped; the Engine owns the live D3D9 device. The
    // viewport HWND is handed to Engine's ctor in WM_CREATE.
    void RenderD3D9();

    HRESULT InitWebView2();
    // Wires every per-controller setup step that's common to both HWND
    // and composition hosting (transparent bg, DevTools, host-object
    // proxy, AcceleratorKeyPressed, put_Bounds, navigation, etc.).
    // Called from the controller-ready completion callback in both
    // modes — the composition controller QI's down to
    // ICoreWebView2Controller so the same wire-up works for both.
    HRESULT FinishWebView2ControllerSetup(ICoreWebView2Controller* controller);
    // Composition-mode completion callback. Stores the composition
    // controller, QI's down to the base controller for the shared
    // setup, then drives Compositor::AttachWebView2 to commit the
    // DComp tree with WebView2's RootVisualTarget plugged in.
    HRESULT OnCompositionControllerReady(HRESULT chr, ICoreWebView2CompositionController* ctl);
    // Phase 3 Stage 3c: forward a Win32 mouse message arriving
    // at hMain into the WebView2 composition surface via
    // ICoreWebView2CompositionController::SendMouseInput. Under HWND
    // hosting, WebView2's child HWND received WM_MOUSE* directly from
    // the OS — under composition hosting the host HWND owns input and
    // must forward. Also handles SetCapture/ReleaseCapture for
    // drag-past-window-edge continuity. No-op when m_compositionMode
    // is false. The caller (MainWndProc) returns 0 after this so
    // DefWindowProc doesn't double-process the message.
    void    ForwardMouseToCompositionWebView2(UINT msg, WPARAM wp, LPARAM lp);
    void    ResizeWebViewToClient();

    void OnWebMessage(const std::wstring& json);

    // Extracted HWND-mode controller dispatch. Originally inline in the
    // env-creation callback; pulled out so the F8 async-fallback handler
    // (WM_APP_COMPOSITION_FALLBACK) can reuse it after tearing down a
    // failed composition setup. Returns the synchronous HRESULT of
    // CreateCoreWebView2Controller; the actual controller is delivered
    // via the inner async callback.
    HRESULT DispatchHwndModeController(ICoreWebView2Environment* env);

    LRESULT MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    int Run(int nCmdShow);
};

// ---------- logging ----------

void HostWindowImpl::OpenLog()
{
    std::wstring path = ComputeHostLogPath();
    // Phase 3 Stage 4f hardening — _wfopen_s opens with
    // exclusive default share-mode (_SH_DENYRW) so concurrent readers
    // get EBUSY. Surfaced when the dxgi-transport.spec.ts tried to
    // read host.log via Node fs.readFileSync to assert [COMP-engine-*]
    // log lines. Switch to _wfsopen with _SH_DENYNO so readers (tests,
    // Get-Content -Wait, etc.) can open the file while the host is
    // writing to it. The host is the only writer so deny-no is safe.
    logFile = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (logFile) Log("[host] === --new-ui session started ===\n");
}

void HostWindowImpl::CloseLog()
{
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile)
    {
        fputs("[host] === --new-ui session ending ===\n", logFile);
        fclose(logFile);
        logFile = nullptr;
    }
}

void HostWindowImpl::Log(const char* fmt, ...)
{
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile)
    {
        fputs(buf, logFile);
        fflush(logFile);
    }
}

// ---------- D3D9 ----------

// render loop + per-frame spawner tick. Replaces the prior
// placeholder clear-to-background path. The per-frame sequence here
// mirrors the legacy `Render` in src/main.cpp:1882 verbatim:
//
//   - Compute dt from the previous frame's timestamp (GetTimeF).
//   - Tick the SpawnerDriver — emits any due burst instances into the
//     Engine.
//   - Engine::Update() advances per-instance state.
//   - Engine::Render() does the actual D3D9 draw + Present.
//   - fpsMeasurer.measure() ticks the FPS ring buffer.
//
// After rendering, compare Engine::GetNumInstances() against the
// last-emitted active-count and broadcast spawner/active-count when it
// changes. The SpawnerPanel badge subscribes to that event unchanged.
void HostWindowImpl::RenderD3D9()
{
    if (!engine) return;

    float now = GetTimeF();
    float dt  = (m_lastRenderTime > 0.0f) ? (now - m_lastRenderTime) : 0.0f;
    m_lastRenderTime = now;

    // [PERF] start of the timed region (covers Tick + Update + Render +
    // the composition sync/copy). Per-stage deltas are taken below.
    const LONGLONG perfFrameStart = PerfQpcNow();

    if (spawnerDriver && particleSystem)
        spawnerDriver->Tick(dt, particleSystem.get(), engine.get());

    // shift-click-to-spawn: refresh cursor velocity from
    // QueryPerformanceCounter deltas before the engine sees it. The
    // attached ParticleSystemInstance reads MouseCursor::GetVelocity
    // through its Object3D parent chain during Update. Mirrors legacy
    // src/main.cpp:1904 — the legacy render loop calls UpdateVelocity
    // unconditionally each frame whether or not a system is attached.
    m_mouseCursor.UpdateVelocity();

    const LONGLONG perfT0 = PerfQpcNow();
    engine->Update();
    const double perfUpdateUs = PerfUsSince(perfT0);

    const LONGLONG perfT1 = PerfQpcNow();
    engine->Render();
    const double perfRenderUs = PerfUsSince(perfT1);

    // [PERF2] fold the engine's per-pass sub-timing of this Render() call.
    const Engine::RenderPassTimingsUs perfPasses = engine->GetLastRenderTimings();
    perfRScene.add(perfPasses.scene);
    perfRBloom.add(perfPasses.bloom);
    perfRDistort.add(perfPasses.distort);
    perfRCompose.add(perfPasses.composite);
    perfRPresent.add(perfPasses.present);

    fpsMeasurer.measure();

    // Phase 3 Stage 4c — composition-mode per-frame composite.
    // engine->Render() above issued D3D9 draws into the AlphaCompositor's
    // shared texture. IssueEndFrameQuery markers the D3D9 command stream
    // after those draws; WaitEndFrameQuery spins until the GPU has
    // finished them — cross-device sync per sub-plan §3.3 path (b).
    // Then CompositeEngineFrame CopyResources from the D3D11 alias into
    // the engine's DXGI swapchain back buffer and Present1's it. DComp
    // picks up the new content on its next composition cycle.
    //
    // Gated on composition-mode + Compositor::IsReady (Stage 3
    // attachment committed) + engineVisualAttached (Stage 4b attach
    // succeeded). When AttachEngineVisual failed (LUID mismatch,
    // D3D11 device, etc.), CompositeEngineFrame returns S_FALSE and
    // this block is a per-frame no-op; composition mode stays intact
    // with viewport area empty per sub-plan §3.8.
    if (m_compositionMode && m_compositor && m_compositor->IsReady())
    {
        engine->IssueEndFrameQuery();
        // [PERF] WaitEndFrameQuery is the suspected hot stage — time the
        // busy-spin and capture the spin count it now returns.
        const LONGLONG perfT2     = PerfQpcNow();
        const int      perfSpins  = engine->WaitEndFrameQuery();
        const double   perfWaitUs = PerfUsSince(perfT2);
        // Phase 3 Stage 4d — pass the engine's current shared
        // handle so Compositor can lazy-detect AlphaCompositor::Resize
        // invalidation and re-open the D3D11 alias. Without this, a
        // window resize freezes the viewport (engine keeps rendering
        // into a new D3D9 texture but our cached alias still points
        // at the released old one). Single pointer compare per frame
        // in the steady state; full re-open + swapchain ResizeBuffers
        // only on actual handle change.
        const LONGLONG perfT3 = PerfQpcNow();
        m_compositor->CompositeEngineFrame(engine->GetSharedTextureHandle());
        const double perfCompositeUs = PerfUsSince(perfT3);

        perfWait.add(perfWaitUs);
        perfComposite.add(perfCompositeUs);
        perfWaitSpinsSum += static_cast<unsigned long long>(perfSpins < 0 ? 0 : perfSpins);
        if (static_cast<unsigned>(perfSpins) > perfWaitSpinsMax)
            perfWaitSpinsMax = static_cast<unsigned>(perfSpins);
    }

    // [PERF] accumulate this frame's stage costs and emit a 1 Hz summary
    // to host.log (mirrors the [COMP-engine-frame] GetTickCount throttle).
    // Times are microseconds. The fps field is derived from frame.avg for
    // sanity only — under an agent-driven launch it is unrepresentative of
    // the user's healthy run (); read per-stage ratios + spin counts.
    perfUpdate.add(perfUpdateUs);
    perfRender.add(perfRenderUs);
    perfFrame.add(PerfUsSince(perfFrameStart));

    const DWORD perfNow = GetTickCount();
    if (perfLastEmitTick == 0 || (perfNow - perfLastEmitTick) >= 1000)
    {
        perfLastEmitTick = perfNow;
        RECT pr = {};
        GetClientRect(hMain, &pr);
        const double favg    = perfFrame.avg();
        const double fps     = favg > 0.0 ? 1.0e6 / favg : 0.0;
        const double spinAvg = perfWait.n
            ? static_cast<double>(perfWaitSpinsSum) / static_cast<double>(perfWait.n) : 0.0;
        Log("[PERF] win=%ldx%ld fps=%.0f frame=%.0f/%.0f update=%.0f/%.0f "
            "render=%.0f/%.0f wait=%.0f/%.0f spins=%.0f/%u composite=%.0f/%.0f (us avg/max)\n",
            pr.right - pr.left, pr.bottom - pr.top, fps,
            perfFrame.avg(), perfFrame.maxUs,
            perfUpdate.avg(), perfUpdate.maxUs,
            perfRender.avg(), perfRender.maxUs,
            perfWait.avg(), perfWait.maxUs,
            spinAvg, perfWaitSpinsMax,
            perfComposite.avg(), perfComposite.maxUs);
        Log("[PERF2] win=%ldx%ld render-passes: scene=%.0f/%.0f bloom=%.0f/%.0f "
            "distort=%.0f/%.0f compose=%.0f/%.0f present=%.0f/%.0f (us avg/max)\n",
            pr.right - pr.left, pr.bottom - pr.top,
            perfRScene.avg(), perfRScene.maxUs,
            perfRBloom.avg(), perfRBloom.maxUs,
            perfRDistort.avg(), perfRDistort.maxUs,
            perfRCompose.avg(), perfRCompose.maxUs,
            perfRPresent.avg(), perfRPresent.maxUs);
        perfUpdate.reset(); perfRender.reset(); perfWait.reset();
        perfComposite.reset(); perfFrame.reset();
        perfRScene.reset(); perfRBloom.reset(); perfRDistort.reset();
        perfRCompose.reset(); perfRPresent.reset();
        perfWaitSpinsSum = 0; perfWaitSpinsMax = 0;
    }

    // Phase 1: hand the just-composited frame to FramePublisher
    // for encode + base64 + emit. Lifecycle is gated on m_framePublisher
    // being non-null. See for why the transport is
    // inline-in-payload rather than WebResourceRequested.
    //
    // follow-up — items 13+15] Skip under composition mode: the
    // React-side <img> consumer of viewport/frame-ready early-returns in
    // composition (ViewportSlot.tsx isLegacyMode() check) because DXGI
    // is the actual engine-pixel source, so the per-frame JPEG encode is
    // pure wasted work. Previously left running ("harmless until
    // architecture-A deletion"), but the encode cost scales with frame
    // area — at 3440x1440 maximized the ~5 MP/frame encode visibly
    // dropped FPS vs legacy mode. Construction stays coupled to
    // m_archCMode for now (less surgery); the per-frame call is the
    // hot path that mattered. Full FramePublisher removal still belongs
    // in the architecture-A deletion dispatch.
    if (m_framePublisher && !m_compositionMode) m_framePublisher->OnFrameComposited();

    // spawner/active-count: emit when GetNumInstances() differs from the
    // last emitted value. Polled per-frame, debounced to avoid WebMessage
    // spam. The SpawnerPanel badge subscription doesn't change — only
    // the source flips from MockBridge timer to real engine state.
    if (dispatcher)
    {
        int instances = engine->GetNumInstances();
        if (instances != m_lastEmittedActiveCount)
        {
            m_lastEmittedActiveCount = instances;
            dispatcher->EmitSpawnerActiveCount(instances);
        }
    }
}

// ---------- WebView2 ----------

void HostWindowImpl::ResizeWebViewToClient()
{
    if (!webController) return;
    RECT r;
    GetClientRect(hMain, &r);
    webController->put_Bounds(r);
    //: when main resizes, the viewport popup's screen position
    // may need to change too (the main HWND's client origin shifted
    // in screen space). React will re-send a layout/viewport-rect
    // once its ResizeObserver fires, which is the authoritative
    // source. Just nudge the screen position from the cached client
    // rect in the meantime so the viewport doesn't lag visually.
    layout.RefreshScreenPosition();
}

void HostWindowImpl::OnWebMessage(const std::wstring& json)
{
    Log("[host] WebMsg (%zu chars)\n", json.size());
    if (dispatcher)
        dispatcher->Dispatch(Utf16ToUtf8(json));
}

HRESULT HostWindowImpl::InitWebView2()
{
    std::wstring userDataFolder = ComputeUserDataFolder();
    Log("[host] WebView2 user-data folder: %ls\n", userDataFolder.c_str());

    // Task 2.2: when --test-host is set, pass --remote-debugging-port=9222
    // to the underlying Chromium runtime so Playwright (and any CDP client)
    // can attach. Opt-in only: production launches use nullptr options.
    // CoreWebView2EnvironmentOptions is the SDK's ready-made implementation
    // (WebView2EnvironmentOptions.h) — it correctly defaults the
    // TargetCompatibleBrowserVersion to the SDK's compiled version, which
    // a hand-rolled class would have to know explicitly.
    ComPtr<ICoreWebView2EnvironmentOptions> envOptions;
    if (useTestHost)
    {
        Log("[host] test-host: enabling CDP on :9222 via AdditionalBrowserArguments\n");
        auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (opts)
        {
            // T9] --force-renderer-accessibility enables Blink's
            // accessibility subsystem at startup so the UIA tree is
            // immediately available to out-of-process clients
            // (uia_inspector). Without it, Blink's a11y is lazily
            // initialized only when a UIA client fires a cross-process
            // structure-change event — which uia_inspector.exe does
            // not do, leaving the RenderWidgetHostView node with empty
            // children. Gated by the outer `if (useTestHost)` block —
            // release builds are untouched.
            opts->put_AdditionalBrowserArguments(
                L"--remote-debugging-port=9222 --force-renderer-accessibility");
            opts.As(&envOptions);
        }
    }

    HRESULT envCreateHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), envOptions.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT
            {
                if (FAILED(envHr) || !env)
                {
                    Log("[host] WebView2 env failed 0x%08lx\n", envHr);
                    return E_FAIL;
                }
                // Phase 0: stash for WebResourceRequested.
                webEnv = env;

                // Phase 3 Stage 3b: composition hosting branch.
                // Gate is m_compositionMode (default true under,
                // false only when ALO_HOSTING_MODE=legacy). Stand up the
                // host::Compositor (DComp V1 device only, no tree yet —
                // tree assembly is deferred until inside the composition-
                // controller completion callback, per v3 lesson) and
                // create a CompositionController instead of the legacy
                // HWND-mode controller. Falls back to HWND mode on any
                // failure so the rest of the path still works.
                if (m_compositionMode)
                {
                    m_compositor = std::make_unique<host::Compositor>(
                        hMain,
                        [this](const std::string& s) { Log("%s\n", s.c_str()); });
                    HRESULT chr = m_compositor->Init();
                    if (FAILED(chr))
                    {
                        Log("[host] composition: Compositor::Init failed hr=0x%08lx — falling back to HWND mode\n", chr);
                        m_compositor.reset();
                        m_compositionMode = false;
                    }
                }

                if (m_compositionMode && m_compositor)
                {
                    // QI for Environment3 — exposes
                    // CreateCoreWebView2CompositionController. Confirmed
                    // available in SDK 1.0.3967.48 (WebView2.h:42610).
                    ComPtr<ICoreWebView2Environment3> env3;
                    HRESULT qihr = env->QueryInterface(IID_PPV_ARGS(&env3));
                    if (FAILED(qihr) || !env3)
                    {
                        Log("[host] composition: QI Environment3 failed hr=0x%08lx — falling back to HWND mode\n", qihr);
                        m_compositor.reset();
                        m_compositionMode = false;
                    }
                    else
                    {
                        Log("[host] composition: CreateCoreWebView2CompositionController dispatching\n");
                        return env3->CreateCoreWebView2CompositionController(
                            hMain,
                            Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                                [this](HRESULT cHr, ICoreWebView2CompositionController* ctl) -> HRESULT
                                {
                                    return OnCompositionControllerReady(cHr, ctl);
                                }).Get());
                    }
                }

                // Default HWND-mode path. Extracted to
                // DispatchHwndModeController so the F8 async-fallback
                // handler can reuse it after tearing down a failed
                // composition setup.
                DispatchHwndModeController(env);
                return S_OK;
            }).Get());
    Log("[host] CreateCoreWebView2EnvironmentWithOptions returned 0x%08lx (testHost=%d composition=%d)\n",
        envCreateHr, useTestHost ? 1 : 0, m_compositionMode ? 1 : 0);
    return envCreateHr;
}

// ---------------------------------------------------------------------
// Phase 3 Stage 3b: shared per-controller setup. Runs after
// either CreateCoreWebView2Controller (HWND mode) or
// CreateCoreWebView2CompositionController (+ QI to ICoreWebView2Controller)
// completes. Every WebView2 wire-up (transparent bg, DevTools, host-object
// proxy, AcceleratorKeyPressed, put_Bounds, app.local mapping,
// add_WebMessageReceived, Navigate) is on the base ICoreWebView2Controller
// or ICoreWebView2 interfaces both modes inherit — so this method runs
// unchanged in both.
// ---------------------------------------------------------------------
HRESULT HostWindowImpl::FinishWebView2ControllerSetup(ICoreWebView2Controller* controller)
{
    if (!controller) return E_POINTER;
    webController = controller;
    controller->get_CoreWebView2(&webView);

    // PROVEN FIX (PoC visual gate, polish 4b23425):
    // Force the WebView2 surface to fully transparent so
    // the sibling D3D9 child HWND is visible through the
    // viewport slot's transparent <div>.
    ComPtr<ICoreWebView2Controller2> ctrl2;
    if (SUCCEEDED(controller->QueryInterface(IID_PPV_ARGS(&ctrl2))))
    {
        COREWEBVIEW2_COLOR transparent = {};
        transparent.A = 0;
        transparent.R = 0;
        transparent.G = 0;
        transparent.B = 0;
        ctrl2->put_DefaultBackgroundColor(transparent);
        Log("[host] WebView2 bg => transparent\n");
    }

    // WebView2 settings. Two things:
    //  1. ALWAYS disable the native right-click context menu. This is a
    //     desktop app, not a browser — the WebView2 default menu (Reload /
    //     Save As / Inspect) otherwise pops on top of and MASKS the app's
    //     own Radix context menus (emitter tree, curve editor), so e.g.
    //     "Dissolve Link Group" is unreachable. The jsdom test lane can't
    //     catch this (Radix opens fine there); only a faithful WebView2
    //     launch surfaces it ().
    //  2. test-host mode enables DevTools (F12) for Playwright/CDP — no
    //     effect in normal launches (gated on useTestHost).
    if (webView)
    {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webView->get_Settings(&settings)) && settings)
        {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            Log("[host] WebView2 default context menu disabled\n");
            if (useTestHost)
            {
                settings->put_AreDevToolsEnabled(TRUE);
                Log("[host] test-host: DevTools enabled (F12)\n");
            }
        }
    }

    // Task 2.2.1: expose hostBridge via AddHostObjectToScript
    // (--test-host only). WebView2 drops postMessage under
    // CDP attachment (lessons.md); the host-object
    // channel is on a separate marshalling path and works,
    // so Playwright drives request/response via this object
    // instead. Never exposed in production — gated on
    // useTestHost.
    if (useTestHost && webView)
    {
        ComPtr<HostBridgeProxy> proxy;
        HRESULT phr = Microsoft::WRL::MakeAndInitialize<HostBridgeProxy>(
            &proxy,
            [this](const std::string& req) -> std::string {
                if (!dispatcher) {
                    return R"({"type":"res","ok":false,"error":"dispatcher not ready"})";
                }
                return dispatcher->DispatchSync(req);
            });
        if (SUCCEEDED(phr) && proxy)
        {
            VARIANT proxyVar;
            VariantInit(&proxyVar);
            proxyVar.vt = VT_DISPATCH;
            proxyVar.pdispVal = proxy.Get();
            proxyVar.pdispVal->AddRef();

            HRESULT ahr = webView->AddHostObjectToScript(
                L"hostBridge", &proxyVar);
            Log("[host] test-host: AddHostObjectToScript(hostBridge) hr=0x%08lx\n",
                ahr);

            // VariantClear releases the AddRef above; the
            // host-object map inside WebView2 keeps its
            // own reference, so the proxy stays alive for
            // the lifetime of the page.
            VariantClear(&proxyVar);
        }
        else
        {
            Log("[host] test-host: HostBridgeProxy init failed hr=0x%08lx\n", phr);
        }
    }

    // Task 1.6: intercept registered accelerator keys before
    // WebView2 routes them to the page. ICoreWebView2Controller
    // exposes add_AcceleratorKeyPressed for exactly this purpose;
    // we only set Handled=TRUE when the combo matches the
    // dictionary registered by React via `register-accelerators`.
    controller->add_AcceleratorKeyPressed(
        Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
            [this](ICoreWebView2Controller* /*sender*/,
                   ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT
            {
                COREWEBVIEW2_KEY_EVENT_KIND kind = {};
                args->get_KeyEventKind(&kind);
                // Only react on key-down events; KEY_UP events are
                // intentionally ignored (no repeat firing).
                if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
                    kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
                {
                    return S_OK;
                }
                UINT vk = 0;
                args->get_VirtualKey(&vk);

                // GetKeyState is synchronous and reliable in an
                // event handler context — reads the current physical
                // key state at the moment of the event.
                bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
                bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;

                bool matched = accelerator.TryDispatch(vk, ctrl, shift, alt,
                    [this](const std::string& combo)
                    {
                        Log("[Accel] combo=%s\n", combo.c_str());
                        if (dispatcher)
                            dispatcher->EmitAcceleratorPressed(combo);
                    });

                if (matched)
                    args->put_Handled(TRUE);

                return S_OK;
            }).Get(),
        &accelKeyTok);
    Log("[host] AcceleratorKeyPressed handler registered\n");

    // Fit to client.
    RECT bounds;
    GetClientRect(hMain, &bounds);
    controller->put_Bounds(bounds);

    //: viewport is now a top-level WS_POPUP
    // owned by hMain (created in WM_CREATE). DWM
    // composites top-level popups as their own
    // layer in screen space, above any child HWND's
    // DComp surface (including WebView2). No
    // SetWindowRgn cut-out is required, no z-order
    // promotion is needed — owned popups naturally
    // stay above their owner.

    // Production mode: map app.local → web/apps/editor/dist
    // so the React app loads from a stable virtual origin.
    // Dev mode (--dev-ui): skip the mapping; Vite's own
    // dev server serves everything from localhost:5174.
    if (!useDevUi)
    {
        ComPtr<ICoreWebView2_3> wv3;
        webView.As(&wv3);
        if (wv3)
        {
            std::wstring distPath = ComputeEditorDistPath();
            Log("[host] editor dist: %ls\n", distPath.c_str());
            wv3->SetVirtualHostNameToFolderMapping(
                kVirtualHostName, distPath.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }
    }

    // Subscribe to JS → host messages.
    // G5: stash the registration token in the member
    // webMessageTok so WM_DESTROY can explicitly unsubscribe.
    webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                // G11: reject messages whose originating document
                // isn't an approved origin. Belt-and-suspenders with the
                // NavigationStarting cancel — if a frame ever loaded an
                // off-origin document, its postMessage must not reach the
                // native bridge.
                LPWSTR src = nullptr;
                if (SUCCEEDED(args->get_Source(&src)) && src)
                {
                    const bool approved = IsApprovedWebViewOrigin(src, useDevUi);
                    if (!approved)
                    {
                        Log("[host] G11: dropped WebMessage from untrusted "
                            "source %ls\n", src);
                        CoTaskMemFree(src);
                        return S_OK;
                    }
                    CoTaskMemFree(src);
                }
                LPWSTR raw = nullptr;
                HRESULT hr1 = args->TryGetWebMessageAsString(&raw);
                if (SUCCEEDED(hr1) && raw)
                {
                    OnWebMessage(raw);
                    CoTaskMemFree(raw);
                }
                else
                {
                    // Fall back: maybe the page posted a JSON value
                    // (chrome.webview.postMessage(obj) rather than
                    // postMessage(JSON.stringify(obj))). Surface a
                    // dedicated log so we can tell the difference
                    // between "no event" and "event but parse failed".
                    LPWSTR json = nullptr;
                    HRESULT hr2 = args->get_WebMessageAsJson(&json);
                    if (SUCCEEDED(hr2) && json)
                    {
                        Log("[host] WMR JSON-only (%zu chars), hr1=0x%08lx\n",
                            wcslen(json), hr1);
                        OnWebMessage(json);
                        CoTaskMemFree(json);
                    }
                    else
                    {
                        Log("[host] WMR empty: hr1=0x%08lx hr2=0x%08lx\n",
                            hr1, hr2);
                    }
                }
                return S_OK;
            }).Get(), &webMessageTok);

    // G11: navigation / new-window / permission policy. Registered
    // BEFORE the Navigate() call below so the very first (legitimate) load is
    // already subject to the allow-list. The app's own target —
    // https://app.local/index.html (prod) or http://localhost:5174/ (dev) —
    // is approved by IsApprovedWebViewOrigin, so its initial navigation is
    // NOT cancelled; only off-origin navigations are.
    webView->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT
            {
                LPWSTR uri = nullptr;
                if (SUCCEEDED(args->get_Uri(&uri)) && uri)
                {
                    if (!IsApprovedWebViewOrigin(uri, useDevUi))
                    {
                        Log("[host] G11: cancelled navigation to %ls\n", uri);
                        args->put_Cancel(TRUE);
                    }
                    CoTaskMemFree(uri);
                }
                return S_OK;
            }).Get(), &navStartingTok);

    // Deny all popups: the editor is a single-window app, so any window.open /
    // target=_blank is unwanted. put_Handled(TRUE) tells WebView2 we took
    // ownership; by creating no window the request is effectively dropped.
    webView->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT
            {
                Log("[host] G11: denied new-window request\n");
                args->put_Handled(TRUE);
                return S_OK;
            }).Get(), &newWindowTok);

    // Deny every permission request (geolocation, camera, mic, clipboard,
    // notifications, …): the editor needs none of them.
    webView->add_PermissionRequested(
        Callback<ICoreWebView2PermissionRequestedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT
            {
                Log("[host] G11: denied permission request\n");
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            }).Get(), &permissionTok);

    //: The WebResourceRequested route was
    // tried mid-spike and abandoned because
    // SetVirtualHostNameToFolderMapping short-circuits
    // user handlers for the mapped host. The transport
    // now ships the JPEG bytes inline in the
    // viewport/frame-ready postMessage payload — see
    // FramePublisher.cpp + in tasks/lessons.md.

    // Navigate to the React app.
    if (useDevUi)
    {
        Log("[host] dev-ui: Navigate to Vite dev server\n");
        webView->Navigate(L"http://localhost:5174/");
    }
    else
    {
        webView->Navigate(L"https://app.local/index.html");
    }
    Log("[host] Navigate dispatched\n");
    return S_OK;
}

// ---------------------------------------------------------------------
// [F8] Extracted HWND-mode dispatch. Originally inline in
// the env-creation callback's else branch; pulled out so the async-
// fallback handler (WM_APP_COMPOSITION_FALLBACK) can reuse it after
// tearing down a failed composition setup. The inner callback body is
// byte-identical to the original.
// ---------------------------------------------------------------------
HRESULT HostWindowImpl::DispatchHwndModeController(ICoreWebView2Environment* env)
{
    if (!env) return E_POINTER;
    return env->CreateCoreWebView2Controller(
        hMain,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [this](HRESULT ctlHr, ICoreWebView2Controller* controller) -> HRESULT
            {
                if (FAILED(ctlHr) || !controller)
                {
                    Log("[host] WebView2 controller failed 0x%08lx\n", ctlHr);
                    return E_FAIL;
                }
                return FinishWebView2ControllerSetup(controller);
            }).Get());
}

// ---------------------------------------------------------------------
// Phase 3 Stage 3b: composition controller completion callback.
// Mirrors dxgi_spike.cpp:OnCompositionControllerReady. Order:
//   1. Stash the composition controller (kept alive for WM_DESTROY).
//   2. QI down to ICoreWebView2Controller and run the shared
//      FinishWebView2ControllerSetup. All wire-up post-step is identical
//      to HWND mode (transparent bg, AcceleratorKeyPressed, put_Bounds,
//      Navigate, ...).
//   3. Build the DComp tree NOW (deferred per v3 — must happen AFTER
//      the controller exists). Compositor::AttachWebView2 plugs the
//      controller's RootVisualTarget into the webview visual + Commits.
// If step 3 fails: it's the failure mode. Log and return; the
// editor still has the controller wired so the rest of the host stays
// alive, but the visual tree won't show anything. Per sub-stage 3b
// acceptance, this is the load-bearing observation.
// ---------------------------------------------------------------------
HRESULT HostWindowImpl::OnCompositionControllerReady(
    HRESULT chr, ICoreWebView2CompositionController* ctl)
{
    if (FAILED(chr) || !ctl)
    {
        Log("[host] composition: controller completion FAILED hr=0x%08lx ctl=%p\n",
            chr, static_cast<void*>(ctl));
        // [F8] Schedule HWND-mode fallback on next message-
        // loop iteration. PostMessage so this callback can unwind first.
        HRESULT failHr = (chr == S_OK) ? E_FAIL : chr;
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(failHr), 0);
        return failHr;
    }
    m_compositionController = ctl;
    Log("[host] composition: controller ready, QI to base for shared setup\n");

    // QI down to ICoreWebView2Controller. The composition controller
    // does NOT inherit from ICoreWebView2Controller in the IDL — they
    // are sibling interfaces returned from different creation paths,
    // both backed by the same underlying object. QueryInterface is the
    // documented way to get the base controller interface from a
    // composition controller.
    ComPtr<ICoreWebView2Controller> baseController;
    HRESULT qihr = ctl->QueryInterface(IID_PPV_ARGS(&baseController));
    if (FAILED(qihr) || !baseController)
    {
        Log("[host] composition: QI to ICoreWebView2Controller failed hr=0x%08lx\n", qihr);
        // [F8] Schedule HWND-mode fallback.
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(qihr), 0);
        return qihr;
    }

    HRESULT setupHr = FinishWebView2ControllerSetup(baseController.Get());
    if (FAILED(setupHr))
    {
        Log("[host] composition: shared controller setup failed hr=0x%08lx\n", setupHr);
        // [F8] Schedule HWND-mode fallback.
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(setupHr), 0);
        return setupHr;
    }

    // Phase 3 Stage 3e: DPI. Composition hosting doesn't
    // auto-track DPI like HWND mode does — the host must call
    // put_RasterizationScale to tell WebView2 the device-pixel
    // scaling factor. Without this, chrome rasterizes at 1.0
    // regardless of monitor DPI and looks blurry on high-DPI
    // displays. WM_DPICHANGED below updates the scale when the
    // window moves between monitors at different DPI.
    //
    // ICoreWebView2Controller (the base interface, which composition
    // controller QI's down to via baseController above) exposes
    // put_RasterizationScale starting at the
    // ICoreWebView2Controller3 interface generation. QI down to it
    // if available; skip silently otherwise (best-effort).
    {
        ComPtr<ICoreWebView2Controller3> ctrl3;
        if (SUCCEEDED(baseController.As(&ctrl3)) && ctrl3)
        {
            UINT dpi = GetDpiForWindow(hMain);
            if (dpi == 0) dpi = 96;
            double scale = static_cast<double>(dpi) / 96.0;
            HRESULT shr = ctrl3->put_RasterizationScale(scale);
            if (FAILED(shr))
            {
                Log("[host] composition: put_RasterizationScale(%.2f) hr=0x%08lx (non-fatal)\n",
                    scale, shr);
            }
        }
    }

    // Phase 3 Stage 3d: cursor sync. The composition
    // controller exposes the desired cursor via get_Cursor and
    // fires add_CursorChanged whenever it changes (e.g. pointer
    // over a link, I-beam over a text input). Cache the HCURSOR
    // and return it from WM_SETCURSOR in MainWndProc. Without
    // this the cursor stays as the Win32 default arrow regardless
    // of what WebView2 wants — link affordance lost.
    HRESULT chrCur = ctl->add_CursorChanged(
        Callback<ICoreWebView2CursorChangedEventHandler>(
            [this](ICoreWebView2CompositionController* sender, IUnknown*) -> HRESULT
            {
                HCURSOR hc = nullptr;
                if (sender && SUCCEEDED(sender->get_Cursor(&hc)))
                {
                    m_webViewCursor = hc;
                }
                return S_OK;
            }).Get(),
        &m_cursorChangedTok);
    if (FAILED(chrCur))
    {
        Log("[host] composition: add_CursorChanged hr=0x%08lx (non-fatal)\n", chrCur);
    }
    // Prime m_webViewCursor with whatever the controller currently
    // wants — without this the first WM_SETCURSOR before any cursor
    // change leaves m_webViewCursor null and we fall through to
    // DefWindowProc (which paints the class arrow). Cheap +
    // documented as the right pattern in the WebView2 samples.
    {
        HCURSOR hc = nullptr;
        if (SUCCEEDED(ctl->get_Cursor(&hc)) && hc)
        {
            m_webViewCursor = hc;
        }
    }

    // Build the visual tree. This is the load-bearing call — if it
    // returns S_OK but the editor renders opaque white, we are in the
    // documented failure mode. Per sub-stage 3b acceptance gate:
    // STOP, capture binary + log + screenshot, surface to user. Do
    // not iterate beyond the 24h cap.
    if (m_compositor)
    {
        HRESULT bhr = m_compositor->AttachWebView2(ctl);
        if (FAILED(bhr))
        {
            Log("[host] composition: Compositor::AttachWebView2 FAILED hr=0x%08lx —-class failure?\n", bhr);
            return bhr;
        }
        // Seed the tree to the current client size so the first paint
        // is sized correctly. SetSize commits internally.
        RECT r;
        GetClientRect(hMain, &r);
        const int clientW = r.right  - r.left;
        const int clientH = r.bottom - r.top;
        m_compositor->SetSize(clientW, clientH);

        // Phase 3 Stage 4b — attach engine visual BEHIND the
        // WebView2 visual (per sub-plan §3.4 / D3). On failure, log
        // and continue with composition mode intact: chrome works,
        // viewport area stays empty (per §3.8 / D7 — explicit
        // no-chain-into-F8). Stage 4c will wire the per-frame
        // CompositeEngineFrame call site; until then this attach
        // is functionally "load the engine visual into the tree
        // but don't Present it" — 4b's smoke matches Stage 3b's
        // chrome-only output.
        if (engine && engine->GetSharedTextureHandle())
        {
            HANDLE sharedTex = engine->GetSharedTextureHandle();
            LUID   engineLuid = engine->GetAdapterLuid();
            HRESULT ehr = m_compositor->AttachEngineVisual(sharedTex, clientW, clientH, engineLuid);
            if (FAILED(ehr))
            {
                Log("[host] composition: AttachEngineVisual hr=0x%08lx — composition mode continues with engine visual NOT attached (viewport area will be empty; sub-plan §3.8)\n", ehr);
                // Do NOT PostMessage(WM_APP_COMPOSITION_FALLBACK) — that
                // path is for chrome-itself-broken failures; engine-
                // attach failures keep the chrome usable in composition
                // mode.
            }
        }
        else
        {
            Log("[host] composition: skipping AttachEngineVisual (engine=%p sharedHandle=%p) — composition mode continues without engine pixels\n",
                engine.get(),
                engine ? engine->GetSharedTextureHandle() : nullptr);
        }

        // Phase 3 Stage 5 — inject the DComp Compositor into the
        // LayoutBroker so React-side layout/scene-rect dispatches start
        // routing into Compositor::SetEngineVisualTransform + Engine::
        // SetSceneViewport. The setter also replays the cached scene-
        // rect onto the newly-attached compositor via ReemitOcclusions
        // (sub-plan §3.5), so if React HAS already dispatched a scene-
        // rect by this point, the engine visual + engine viewport
        // immediately match it. (In practice React's first dispatch
        // typically arrives AFTER this site because React is still
        // booting inside the WebView2 visual; the explicit full-client
        // seed below covers the in-between frames.)
        if (m_compositor)
        {
            layout.SetCompositor(m_compositor.get());

            // If LayoutBroker has no cached scene-rect yet (React hasn't
            // dispatched layout/scene-rect yet — the common case at
            // composition-controller-ready time), explicitly seed the
            // engine visual + engine viewport to full client so the
            // first frame is sized correctly. Without this seed, the
            // engine visual's offset/clip stays at the DComp default
            // (0,0,inf,inf) — visually OK but inconsistent with the
            // post-Stage-5 invariant "engine visual ALWAYS has an
            // explicit transform under composition mode."
            //
            // The seed also makes the boot-time
            // [COMP-engine-transform] / [engine] SetSceneViewport log
            // lines appear before React's first dispatch — useful as
            // a positive control + asserted by T7's dxgi-scene-rect
            // Playwright spec.
            int sx, sy, sw, sh;
            if (!layout.GetSceneRect(sx, sy, sw, sh))
            {
                sx = 0;
                sy = 0;
                sw = clientW;
                sh = clientH;

                // immediate=true — apply the seed straight through
                // rather than queueing it for CompositeEngineFrame.
                // At attach time the engine hasn't rendered yet under
                // the new transform, so there's nothing to coordinate
                // with; queueing would just delay the visible clip
                // until the first composite.
                HRESULT thr = m_compositor->SetEngineVisualTransform(sx, sy, sw, sh, /*immediate=*/true);
                if (FAILED(thr) && thr != S_FALSE)
                {
                    Log("[host] composition: initial seed SetEngineVisualTransform hr=0x%08lx (non-fatal)\n", thr);
                }
                // Phase 3 Stage 5 T6 follow-up (rev 2) —
                // restore engine viewport seed under B-γ with per-
                // pixel-FoV-vs-current-RT reference. At seed time
                // sceneH equals BackBufferHeight (full client), so
                // SetSceneViewport's per-pixel-FoV computes
                // fovY = 45° × clientH/RT_H = 45° — matches pre-
                // Stage-5 projection exactly. No FoV explosion at
                // attach.
                if (engine)
                {
                    engine->SetSceneViewport(sx, sy, sw, sh);
                }
            }
        }

        Log("[host] composition hosting ready (DComp tree committed)\n");
    }

    // Phase 3 Stage 3f (path b+): give WebView2 logical
    // keyboard focus. Under HWND hosting, WebView2's own child HWND
    // received WM_KEY*/WM_IME_* via the OS focus chain — under
    // composition, the host HWND owns Win32 focus and WebView2
    // is just a DComp visual with no HWND of its own. WebView2's
    // input thread won't see keys unless we MoveFocus explicitly.
    // Without this: clicks still reach React (mouse forwarding 3c
    // works), but Escape/typing/IME silently vanish because
    // AcceleratorKeyPressed and the DOM keydown chain only fire
    // when WebView2 has focus. WM_SETFOCUS in MainWndProc keeps it
    // restored after Alt-Tab cycles.
    //
    // PROGRAMMATIC reason = "the host asked, don't traverse to a
    // particular child first." Equivalent to focusing the WebView's
    // root document body.
    HRESULT fhr = baseController->MoveFocus(
        COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    if (FAILED(fhr))
    {
        Log("[host] composition: initial MoveFocus hr=0x%08lx (non-fatal)\n", fhr);
    }
    return S_OK;
}

// ---------------------------------------------------------------------
// Phase 3 Stage 3c: mouse forwarding under composition hosting.
// Translates the Win32 WM_MOUSE* message family into
// ICoreWebView2CompositionController::SendMouseInput calls. The
// COREWEBVIEW2_MOUSE_EVENT_KIND enum values are numerically identical
// to the WM_* constants (verified at compile time against WebView2.h
// 1.0.3967.48 — WM_MOUSEMOVE=512, WM_LBUTTONDOWN=513, ...), so a
// direct cast is safe. Same for COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS
// matching MK_* bits.
//
// Wheel messages (WM_MOUSEWHEEL, WM_MOUSEHWHEEL) arrive in SCREEN
// coordinates while all other WM_MOUSE* arrive in CLIENT coords;
// translate the wheel cases via ScreenToClient. Wheel delta goes in
// the mouseData parameter (signed short in the HIWORD of wParam).
//
// Capture handling: SetCapture(hMain) on any button-down so drags
// extending past the window edge keep flowing as WM_MOUSEMOVE to the
// host. ReleaseCapture() when the up-event leaves wParam's MK_*
// button bits at zero (no button still held). This avoids the
// alternate "track which button captured" book-keeping and
// matches the simple model React's pointer-id state expects.
// ---------------------------------------------------------------------
void HostWindowImpl::ForwardMouseToCompositionWebView2(UINT msg, WPARAM wp, LPARAM lp)
{
    if (!m_compositionController) return;

    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    UINT32 mouseData = 0;
    if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
    {
        ScreenToClient(hMain, &pt);
        // GET_WHEEL_DELTA_WPARAM returns a signed short. Cast through
        // INT16 first to sign-extend correctly into the 32-bit slot
        // SendMouseInput expects.
        mouseData = static_cast<UINT32>(static_cast<INT16>(GET_WHEEL_DELTA_WPARAM(wp)));
    }

    // MK_* bits in wParam's low word map 1:1 to
    // COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS:
    //   MK_LBUTTON=0x01  → LEFT_BUTTON
    //   MK_RBUTTON=0x02  → RIGHT_BUTTON
    //   MK_SHIFT  =0x04  → SHIFT
    //   MK_CONTROL=0x08  → CONTROL
    //   MK_MBUTTON=0x10  → MIDDLE_BUTTON
    // (MK_XBUTTON1/2 don't have COREWEBVIEW2 equivalents in 1.0.3967.48;
    //  Stage 3c doesn't forward them. The 99-test suite doesn't
    //  exercise XButton.)
    auto virtualKeys = static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(
        LOWORD(wp) & (MK_LBUTTON | MK_RBUTTON | MK_SHIFT |
                      MK_CONTROL | MK_MBUTTON));

    m_compositionController->SendMouseInput(
        static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg),
        virtualKeys,
        mouseData,
        pt);

    // Capture: any button-down captures, any button-up that leaves
    // wParam with no buttons held releases.
    switch (msg)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
        SetCapture(hMain);
        break;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP:
        if ((wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)) == 0)
        {
            ReleaseCapture();
        }
        break;
    default:
        break;
    }
}

// ---------- WndProc dispatch ----------

LRESULT HostWindowImpl::MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        //: viewport is a top-level WS_POPUP window OWNED by main
        // (not a WS_CHILD). DWM composites top-level popups as their
        // own layer in screen space, above any child HWND's DComp
        // surface — including WebView2's. WS_EX_NOACTIVATE prevents
        // the popup from stealing focus on click (camera drag still
        // works because mouse capture is explicit in ViewportWndProc).
        // WS_EX_TOOLWINDOW keeps the popup out of the taskbar.
        //
        // Ownership semantics: an owned popup follows the owner's
        // minimize/restore state, gets destroyed when the owner is
        // destroyed, and stays z-ordered above the owner. Position
        // is in SCREEN coords; LayoutBroker translates from main-
        // client coords via ClientToScreen.
        //: WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA) replaces
        ///'s SetWindowRgn cut-out. The AlphaCompositor pushes a
        // pre-multiplied ARGB bitmap each tick, the OS composites the
        // popup onto the WebView2 underneath, and software alpha stamps
        // (T4) carve soft-edged holes for chrome occlusion rects.
        hViewport = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kHostViewportClassName, L"",
            WS_POPUP | WS_VISIBLE,
            16, 16, 320, 240, hwnd /* owner */, nullptr,
            hInstance, nullptr);
        if (!hViewport)
        {
            Log("[host] CreateWindowExW viewport failed (gle=%lu)\n", GetLastError());
            return -1;
        }
        layout.SetViewport(hViewport);

        //: no host-owned D3D9 device. The Engine constructs the
        // live device internally below, targeting this viewport HWND.

        // Construct the Engine now that both HWNDs exist. hFocus = parent,
        // hDevice = viewport child — same wiring as legacy main.cpp.
        try
        {
            engine = std::make_unique<Engine>(
                hwnd, hViewport, textureManager, shaderManager, fileManager);
            if (dispatcher) dispatcher->SetEngine(engine.get());
            layout.SetEngine(engine.get());
            // D6: bind engine to ModManager so subsequent
            // SelectMod() calls can hot-swap shaders + textures.
            if (modManager) modManager->SetEngine(engine.get());

            // [bloom-restore, session 10] Restore bloom config from the
            // registry (HKCU\Software\AloParticleEditor), mirroring legacy
            // main.cpp's startup restore (SetBloom* from ReadBloom*). The
            // new-UI host previously skipped this, so the engine kept its
            // strength=0 constructor default and toggling "Enable bloom"
            // produced NO visible glow even when the user has saved bloom
            // settings from the legacy editor. Same value names/types legacy
            // reads/writes, so settings round-trip between the two UIs.
            //
            // Skipped under --test-host: the a11y goldens capture the bloom
            // dialog's strength value, so the harness must see the
            // constructor defaults (0.00) deterministically, not whatever the
            // dev machine has saved in the registry.
            if (!useTestHost)
            {
                HKEY hKey = nullptr;
                if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\AloParticleEditor",
                                  0, KEY_READ, &hKey) == ERROR_SUCCESS)
                {
                    DWORD en = 0, type = 0, size = sizeof(en);
                    if (RegQueryValueExW(hKey, L"BloomEnabled", nullptr, &type,
                                         reinterpret_cast<LPBYTE>(&en), &size) == ERROR_SUCCESS
                        && type == REG_DWORD)
                        engine->SetBloom(en != 0);

                    // REG_BINARY float; reject NaN/Inf so a corrupt blob can't
                    // drive bloom into a silly state (matches legacy's check).
                    auto readF = [&](const wchar_t* name, float fallback) -> float {
                        float v = 0.0f; DWORD t = 0, s = sizeof(v);
                        if (RegQueryValueExW(hKey, name, nullptr, &t,
                                             reinterpret_cast<LPBYTE>(&v), &s) == ERROR_SUCCESS
                            && t == REG_BINARY && s == sizeof(v)
                            && v == v && (v - v) == 0.0f)
                            return v;
                        return fallback;
                    };
                    engine->SetBloomStrength(readF(L"BloomStrength", engine->GetBloomStrength()));
                    engine->SetBloomCutoff  (readF(L"BloomCutoff",   engine->GetBloomCutoff()));
                    engine->SetBloomSize    (readF(L"BloomSize",     engine->GetBloomSize()));

                    // [view-settings-restore, session 11] Mirror legacy
                    // main.cpp's startup restore (main.cpp:7614-7692) so the
                    // new-UI viewport opens with the user's persisted
                    // background / ground / skydome instead of engine ctor
                    // defaults. Same value names/types legacy reads, so
                    // settings round-trip between the two UIs. Same
                    // !useTestHost gate as bloom: the a11y goldens (e.g.
                    // dialog-lighting's "Show ground" toggle) must see
                    // deterministic ctor defaults. GroundZ is intentionally
                    // NOT restored — legacy resets it to 0 each launch by
                    // design (main.cpp:7626).
                    auto readDword = [&](const wchar_t* name, DWORD& out) -> bool {
                        DWORD t = 0, s = sizeof(out);
                        return RegQueryValueExW(hKey, name, nullptr, &t,
                                                reinterpret_cast<LPBYTE>(&out), &s) == ERROR_SUCCESS
                               && t == REG_DWORD && s == sizeof(out);
                    };
                    // REG_SZ two-pass sized read (mirrors ReadGroundSlotPath:
                    // the stored value may omit the trailing NUL).
                    auto readSz = [&](const wchar_t* name) -> std::wstring {
                        DWORD t = 0, cb = 0;
                        if (RegQueryValueExW(hKey, name, nullptr, &t, nullptr, &cb) != ERROR_SUCCESS
                            || t != REG_SZ || cb < sizeof(wchar_t))
                            return std::wstring();
                        std::vector<wchar_t> buf(cb / sizeof(wchar_t) + 1, 0);
                        if (RegQueryValueExW(hKey, name, nullptr, &t,
                                             reinterpret_cast<LPBYTE>(buf.data()), &cb) != ERROR_SUCCESS)
                            return std::wstring();
                        buf.back() = 0;
                        return std::wstring(buf.data());
                    };

                    DWORD dw = 0;
                    if (readDword(L"BackgroundColor", dw))
                        engine->SetBackground(static_cast<COLORREF>(dw));
                    if (readDword(L"ShowGround", dw))
                        engine->SetGround(dw != 0);
                    engine->SetGroundZ(0.0f);

                    // Ground texture: per-slot custom paths BEFORE the
                    // selected index, so SetGroundTexture can find the right
                    // source for a custom slot (ordering is load-bearing).
                    for (int slot = 0; slot < Engine::kGroundTextureCount; ++slot)
                    {
                        wchar_t name[32];
                        swprintf_s(name, L"GroundTextureSlot%d", slot);
                        std::wstring path = readSz(name);
                        if (!path.empty())
                            engine->SetGroundSlotCustomPath(slot, path);
                    }
                    if (readDword(L"GroundSolidColor", dw))
                        engine->SetGroundSolidColor(static_cast<COLORREF>(dw));
                    if (readDword(L"GroundTexture", dw)
                        && dw < static_cast<DWORD>(Engine::kGroundTextureCount))
                        engine->SetGroundTexture(static_cast<int>(dw));

                    // Skydome: custom paths first so SetSkydomeSlot can reload
                    // a previously-active custom slot.
                    for (int s = Engine::kSkydomeFirstCustomSlot;
                         s < Engine::kSkydomeSlotCount; ++s)
                    {
                        wchar_t name[64];
                        swprintf_s(name, L"SkydomeCustomSlot%d", s);
                        engine->SetSkydomeCustomPath(s, readSz(name));
                    }
                    if (readDword(L"SkydomeIndex", dw)
                        && static_cast<int>(dw) < Engine::kSkydomeSlotCount)
                        engine->SetSkydomeSlot(static_cast<int>(dw));

                    // [lighting-restore, session 12] Restore the persisted
                    // lighting (sun / fill1 / fill2 angles + colours +
                    // intensities, ambient, shadow) so the new-UI viewport
                    // opens with the user's saved lights instead of engine
                    // ctor defaults. Mirrors legacy PushLightingToEngine
                    // (src/main.cpp:6376-6410) field-for-field, including the
                    // Force-Align fill-angle computation: when the
                    // LightingForceFillAlignment flag is ON the fill azimuths
                    // are derived from the sun (sun.z + 120° / + 210°, both at
                    // -10° tilt); when OFF the persisted free-edit angles feed
                    // the engine directly. Floats are REG_BINARY (readF),
                    // colours + the flag are REG_DWORD. Same !useTestHost gate
                    // as the rest of this block (the engine snapshot the
                    // dialog-lighting a11y golden seeds from must show ctor
                    // defaults under --test-host). Intensity is folded into the
                    // diffuse/specular channels exactly as legacy MakeLight
                    // (src/main.cpp:6222); fills pass specular=black.
                    auto readColor = [&](const wchar_t* name, COLORREF def) -> COLORREF {
                        DWORD v = 0;
                        return readDword(name, v) ? static_cast<COLORREF>(v) : def;
                    };
                    auto makeLight = [](float zDeg, float tiltDeg, COLORREF diffuse,
                                        COLORREF specular, float intensity) -> Engine::Light {
                        Engine::Light L = {};
                        const float zr = D3DXToRadian(zDeg);
                        const float tr = D3DXToRadian(tiltDeg);
                        const float c  = cosf(tr);
                        L.Position  = D3DXVECTOR4(c * cosf(zr), c * sinf(zr), sinf(tr), 0.0f);
                        L.Direction = D3DXVECTOR4(0, 0, 0, 0);
                        L.Diffuse   = D3DXVECTOR4(GetRValue(diffuse)  / 255.0f * intensity,
                                                  GetGValue(diffuse)  / 255.0f * intensity,
                                                  GetBValue(diffuse)  / 255.0f * intensity, 1.0f);
                        L.Specular  = D3DXVECTOR4(GetRValue(specular) / 255.0f * intensity,
                                                  GetGValue(specular) / 255.0f * intensity,
                                                  GetBValue(specular) / 255.0f * intensity, 1.0f);
                        return L;
                    };
                    auto colorToVec4 = [](COLORREF c) -> D3DXVECTOR4 {
                        return D3DXVECTOR4(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                           GetBValue(c) / 255.0f, 0.0f);
                    };

                    const float    sunIntensity = readF(L"LightSunIntensity", 0.50f);
                    const float    sunZ         = readF(L"LightSunZAngle",    0.0f);
                    const float    sunTilt      = readF(L"LightSunTilt",      45.0f);
                    const COLORREF sunAmbient   = readColor(L"LightSunAmbientColor",  RGB(40, 40, 50));
                    const COLORREF sunSpecular  = readColor(L"LightSunSpecularColor", RGB(190, 190, 200));
                    const COLORREF sunDiffuse   = readColor(L"LightSunDiffuseColor",  RGB(180, 180, 190));
                    const COLORREF sunShadow    = readColor(L"LightSunShadowColor",   RGB(100, 100, 110));
                    DWORD faDw = 0;
                    const bool forceAlign = readDword(L"LightingForceFillAlignment", faDw)
                                                ? (faDw != 0) : true;  // kLightForceAlignDefault
                    const float    fill1Intensity = readF(L"LightFill1Intensity", 0.50f);
                    const float    fill1Zp        = readF(L"LightFill1ZAngle",    120.0f);
                    const float    fill1Tiltp     = readF(L"LightFill1Tilt",      -10.0f);
                    const COLORREF fill1Diffuse   = readColor(L"LightFill1DiffuseColor", RGB(60, 80, 160));
                    const float    fill2Intensity = readF(L"LightFill2Intensity", 0.50f);
                    const float    fill2Zp        = readF(L"LightFill2ZAngle",    210.0f);
                    const float    fill2Tiltp     = readF(L"LightFill2Tilt",      -10.0f);
                    const COLORREF fill2Diffuse   = readColor(L"LightFill2DiffuseColor", RGB(60, 80, 160));

                    // Force-align fill angles (verbatim src/main.cpp:6400-6403).
                    const float fill1Z    = forceAlign ? (sunZ + 120.0f) : fill1Zp;
                    const float fill1Tilt = forceAlign ? -10.0f          : fill1Tiltp;
                    const float fill2Z    = forceAlign ? (sunZ + 210.0f) : fill2Zp;
                    const float fill2Tilt = forceAlign ? -10.0f          : fill2Tiltp;

                    engine->SetLight(Engine::LT_SUN,
                        makeLight(sunZ, sunTilt, sunDiffuse, sunSpecular, sunIntensity));
                    engine->SetLight(Engine::LT_FILL1,
                        makeLight(fill1Z, fill1Tilt, fill1Diffuse, RGB(0, 0, 0), fill1Intensity));
                    engine->SetLight(Engine::LT_FILL2,
                        makeLight(fill2Z, fill2Tilt, fill2Diffuse, RGB(0, 0, 0), fill2Intensity));
                    engine->SetAmbient(colorToVec4(sunAmbient));
                    engine->SetShadow (colorToVec4(sunShadow));

                    // The standing no-user verification channel for the
                    // lighting restore () — distinct from [view-restore]
                    // above. Prints the inputs that drove the engine writes.
                    Log("[lighting-restore] sunZ=%.1f sunTilt=%.1f forceAlign=%d "
                        "fill1Z=%.1f fill2Z=%.1f sunDiffuse=0x%06X\n",
                        sunZ, sunTilt, forceAlign ? 1 : 0, fill1Z, fill2Z,
                        static_cast<unsigned>(sunDiffuse));

                    // Dump restored view-settings to host.log. This is the
                    // ONLY no-user verification channel for this restore: the
                    // --test-host CDP bridge can't observe it (the whole block
                    // is gated off under --test-host), so a faithful
                    // non-test-host launch + this log line is how parity is
                    // confirmed (host.log is trusted under; agent
                    // screenshots are not — see tasks/lessons.md).
                    Log("[view-restore] bg=0x%06X showGround=%d groundTex=%d "
                        "groundSolid=0x%06X skydome=%d\n",
                        static_cast<unsigned>(engine->GetBackground()),
                        engine->GetGround() ? 1 : 0,
                        engine->GetGroundTexture(),
                        static_cast<unsigned>(engine->GetGroundSolidColor()),
                        engine->GetSkydomeSlot());
                    RegCloseKey(hKey);
                }
            }
            Log("[host] Engine constructed OK\n");
        }
        catch (const std::exception& e)
        {
            Log("[host] Engine construction threw: %s\n", e.what());
            MessageBoxA(hwnd, e.what(), "Engine init failed", MB_ICONERROR);
            // Continue — viewport will still clear, just without engine state.
        }
        catch (...)
        {
            Log("[host] Engine construction threw unknown exception\n");
            // Continue without engine; snapshot will return ok:false.
        }

        //: stand up the alpha compositor against the Engine's D3D9
        // device. The Engine's Reset() resizes the off-screen RT on
        // layout changes; we still bootstrap a non-degenerate size now
        // so the very first Render finds a valid RT to target.
        if (engine && engine->GetDevice())
        {
            try
            {
                alphaCompositor = std::make_unique<host::AlphaCompositor>(engine->GetDevice());
                RECT vrc{};
                GetClientRect(hViewport, &vrc);
                alphaCompositor->Resize(vrc.right - vrc.left, vrc.bottom - vrc.top);
                engine->SetAlphaCompositor(alphaCompositor.get());
                // [PERF] In composition mode the DComp shared-texture path is
                // the transport; tell the engine to skip the redundant
                // per-frame layered Composite() readback (round-3 fix).
                engine->SetCompositionMode(m_compositionMode);
                layout.SetAlphaCompositor(alphaCompositor.get());
                Log("[host] AlphaCompositor up (%ldx%ld)\n",
                    vrc.right - vrc.left, vrc.bottom - vrc.top);

                // Phase 1: when mode is enabled, stand up
                // the FramePublisher on top of the compositor. Emit
                // callback wraps PostWebMessageAsJson with the UTF-16
                // conversion already established by the bridge dispatcher;
                // logger fans through Log() at 1 Hz. Constructed AFTER the
                // compositor so EncodeFrameJpeg has somewhere to read.
                if (m_archCMode && alphaCompositor)
                {
                    auto emit = [this](const std::string& json) {
                        if (!webView) return;
                        std::wstring w = Utf8ToUtf16(json);
                        webView->PostWebMessageAsJson(w.c_str());
                    };
                    m_framePublisher = std::make_unique<host::FramePublisher>(
                        alphaCompositor.get(), emit, m_archCQuality);
                    m_framePublisher->SetLogger([this](const std::string& line) {
                        Log("%s\n", line.c_str());
                    });
                    Log("[ArchC] FramePublisher up (mode=canvas-jpeg, q=%d)\n", m_archCQuality);

                    // Phase 2: stand up the InputDispatcher on the
                    // hidden viewport popup. Bound to BridgeDispatcher
                    // below in Run() once `dispatcher` exists.
                    m_inputDispatcher = std::make_unique<host::InputDispatcher>(hViewport);
                    m_inputDispatcher->SetLogger([this](const std::string& line) {
                        Log("%s\n", line.c_str());
                    });
                    Log("[ArchC] InputDispatcher up (popup=%p)\n",
                        static_cast<void*>(hViewport));
                }
            }
            catch (const std::exception& e)
            {
                Log("[host] AlphaCompositor init failed: %s — falling back to legacy Present\n", e.what());
                alphaCompositor.reset();
                m_framePublisher.reset();
            }
        }

        // Seed the first paint (suppresses white-flash on startup; see
        // PoC visual gate notes in the task brief).
        InvalidateRect(hViewport, nullptr, FALSE);

        // Start the 4 Hz stats timer. Fires every 250 ms and emits a
        // stats/tick event to React so the status bar stays live.
        SetTimer(hwnd, kStatsTimerId, 250, nullptr);

        //: two-tier autosave timers (30 s recent / 5 min stable),
        // mirroring legacy main.cpp:2227. Gated on !useTestHost so harness
        // runs never write autosave files — those would orphan into a
        // recovery prompt for the user's real editor. WM_TIMER writes the
        // live ParticleSystem (dirty-gated) below.
        if (!useTestHost)
        {
            SetTimer(hwnd, Autosave::RECENT_TIMER_ID, Autosave::RECENT_INTERVAL_MS, nullptr);
            SetTimer(hwnd, Autosave::STABLE_TIMER_ID, Autosave::STABLE_INTERVAL_MS, nullptr);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kStatsTimerId && dispatcher)
        {
            float fps      = fpsMeasurer.getFPS();
            int emitters   = engine ? engine->GetNumEmitters()  : 0;
            int particles  = engine ? engine->GetNumParticles() : 0;
            int instances  = engine ? engine->GetNumInstances() : 0;
            dispatcher->EmitStatsTick(fps, emitters, particles, instances);
        }
        //: autosave tick. Best-effort + dirty-gated — skip the write
        // when nothing changed since the last save (no point autosaving an
        // unmodified saved file). Runs on the host UI thread between frames
        // (single-threaded pump), same as legacy's WM_TIMER write.
        else if ((wp == Autosave::RECENT_TIMER_ID || wp == Autosave::STABLE_TIMER_ID)
                 && dispatcher && particleSystem && dispatcher->GetDirty())
        {
            Autosave::Tier tier = (wp == Autosave::RECENT_TIMER_ID)
                                ? Autosave::Tier::Recent
                                : Autosave::Tier::Stable;
            bool wrote = Autosave::Write(*particleSystem, dispatcher->GetCurrentFilePath(), tier);
#ifndef NDEBUG
            fprintf(stderr, "[autosave] %s tier=%s\n",
                    wrote ? "wrote" : "write-FAILED",
                    tier == Autosave::Tier::Recent ? "recent" : "stable");
#else
            (void)wrote;
#endif
        }
        return 0;

    case WM_SIZE:
        ResizeWebViewToClient();
        // Phase 3 Stage 3b: under composition hosting, the
        // DComp tree's root visual clip needs to track the host
        // client size or chrome gets clipped on resize.
        if (m_compositionMode && m_compositor && m_compositor->IsReady())
        {
            RECT r;
            GetClientRect(hwnd, &r);
            m_compositor->SetSize(r.right - r.left, r.bottom - r.top);
        }
        return 0;

    // Phase 3 Stage 3f (path b+): host HWND gained focus
    // (initial show, Alt-Tab back, click into the window). Forward
    // logical keyboard focus to WebView2 so its DOM event chain
    // sees WM_KEY*/WM_IME_*. Without this, after Alt-Tab away and
    // back the host owns focus, WebView2 doesn't, and keyboard
    // silently breaks until the next mouse click happens to
    // re-trigger something. Gated on m_compositionMode — under
    // HWND mode WebView2 owns its own HWND focus chain.
    case WM_SETFOCUS:
        if (m_compositionMode && webController)
        {
            webController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        break;  // fall through so DefWindowProc sees it too

    // Phase 3 Stage 3e: DPI changed (window moved to a
    // monitor with different DPI). HIWORD(wp) is the new system DPI;
    // lp points to a suggested RECT in screen coords. Update the
    // composition controller's rasterization scale so chrome
    // re-rasterises crisp at the new DPI, then resize/reposition
    // the host HWND to Windows's suggested rect (recommended
    // per-monitor-v2 best practice). Gated on m_compositionMode —
    // under HWND mode WebView2 handles DPI on its own HWND.
    case WM_DPICHANGED:
        if (m_compositionMode && m_compositionController)
        {
            ComPtr<ICoreWebView2Controller3> ctrl3;
            if (webController && SUCCEEDED(webController.As(&ctrl3)) && ctrl3)
            {
                UINT dpi = HIWORD(wp);  // HIWORD and LOWORD are the same
                if (dpi == 0) dpi = 96;
                double scale = static_cast<double>(dpi) / 96.0;
                ctrl3->put_RasterizationScale(scale);
                Log("[host] WM_DPICHANGED dpi=%u scale=%.2f\n", dpi, scale);
            }
        }
        if (lp)
        {
            const RECT* prc = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr,
                prc->left, prc->top,
                prc->right - prc->left, prc->bottom - prc->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;

    // [F8] Async composition setup failed — tear down partial
    // state and re-dispatch to HWND mode via the stashed webEnv. wParam
    // is the original failure HRESULT (informational; we don't act on
    // it differently per code). PostMessage'd from OnCompositionController-
    // Ready so the WebView2 callback can unwind before we touch its state.
    case WM_APP_COMPOSITION_FALLBACK:
    {
        HRESULT failHr = static_cast<HRESULT>(wp);
        Log("[host] composition: async failure hr=0x%08lx — tearing down + falling back to HWND mode\n", failHr);

        // Tear down composition state. webController may be partly set
        // if FinishWebView2ControllerSetup got far enough; Close it
        // before reset so WebView2's internal state unwinds cleanly.
        if (webController)
        {
            webController->Close();
            webController.Reset();
        }
        m_compositionController.Reset();
        // Phase 3 Stage 5 — clear LayoutBroker's pointer BEFORE
        // releasing the Compositor so any late SetSceneRect dispatch
        // that slips through (e.g. an in-flight BridgeDispatcher message
        // queued before the fallback) doesn't dereference a freed
        // Compositor. Mirrors the SetAlphaCompositor(nullptr) pattern
        // and the symmetric WM_DESTROY teardown below.
        layout.SetCompositor(nullptr);
        m_compositor.reset();
        m_compositionMode = false;

        // Re-dispatch via the stashed webEnv. Same env, new controller
        // path — no need to re-create CoreWebView2EnvironmentWithOptions.
        if (webEnv)
        {
            HRESULT hr = DispatchHwndModeController(webEnv.Get());
            if (FAILED(hr))
            {
                Log("[host] composition: HWND fallback dispatch failed hr=0x%08lx\n", hr);
            }
        }
        else
        {
            Log("[host] composition: webEnv not stashed; cannot fall back\n");
        }
        return 0;
    }

    // Phase 3 Stage 3d: cursor sync. Under composition the
    // host HWND owns WM_SETCURSOR; consult the cached cursor that
    // the composition controller's add_CursorChanged handler last
    // delivered. Returning TRUE tells Windows we set the cursor
    // ourselves — skip default class-arrow behaviour. Gated on
    // m_compositionMode + cached cursor existing so default new-UI
    // paths fall through unchanged.
    case WM_SETCURSOR:
        if (m_compositionMode && m_webViewCursor &&
            LOWORD(lp) == HTCLIENT)
        {
            SetCursor(m_webViewCursor);
            return TRUE;
        }
        break;

    // Phase 3 Stage 3c: forward mouse input to WebView2's
    // composition controller. Under HWND mode, WebView2's child HWND
    // gets WM_MOUSE* directly from the OS — under composition the
    // host owns input and forwards via SendMouseInput. Gated on
    // m_compositionMode so the default new-UI path falls through to
    // DefWindowProc unchanged.
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        if (m_compositionMode && m_compositionController)
        {
            // F10: arm TME_LEAVE on each fresh WM_MOUSEMOVE
            // so WM_MOUSELEAVE fires when the pointer exits the host
            // HWND. Without this, WebView2 keeps last-known CSS :hover
            // state and cursor when the pointer leaves the window.
            if (msg == WM_MOUSEMOVE && !m_mouseTracked)
            {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize    = sizeof(tme);
                tme.dwFlags   = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (TrackMouseEvent(&tme)) m_mouseTracked = true;
            }
            ForwardMouseToCompositionWebView2(msg, wp, lp);
            return 0;
        }
        break;

    // F10: forward COREWEBVIEW2_MOUSE_EVENT_KIND_MOUSE_LEAVE
    // when the pointer exits the host HWND so WebView2 clears CSS :hover
    // state and the cursor. WM_MOUSELEAVE's wp/lp don't carry coords or
    // virtual-key state — use POINT{-1, -1} per WebView2 docs.
    case WM_MOUSELEAVE:
        m_mouseTracked = false;
        if (m_compositionMode && m_compositionController)
        {
            // WebView2 SDK 1.0.3967.48 doesn't expose a named
            // COREWEBVIEW2_MOUSE_EVENT_KIND_MOUSE_LEAVE constant — the
            // enum values are numerically identical to the WM_* codes
            // (per ForwardMouseToCompositionWebView2's existing
            // direct-cast pattern), so casting WM_MOUSELEAVE works.
            POINT pt = { -1, -1 };
            m_compositionController->SendMouseInput(
                static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(WM_MOUSELEAVE),
                COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE,
                0,
                pt);
        }
        return 0;

    case WM_MOVE:
        //: when main moves, the viewport popup follows. Position
        // changes only — size stays cached.
        layout.RefreshScreenPosition();
        return 0;

    case WM_WINDOWPOSCHANGED:
        // polish: WM_WINDOWPOSCHANGED fires for every position/
        // size change BEFORE WM_SIZE / WM_MOVE / WM_PAINT.
        //
        // (1) PredictAndApply resizes the popup synchronously to
        //     match main's new client extent, using cached layout
        //     offsets.
        // (2) RenderD3D9 forces a Present after the swap chain is
        //     Reset. Without this, Windows' modal resize loop holds
        //     my PeekMessage idle pump and D3D9 never gets to render
        //     fresh — the popup just stretches the LAST presented
        //     frame, so a wider/taller resize reveals dark purple
        //     where the ground plane should be.
        if (hViewport)
        {
            layout.PredictAndApply();
            RenderD3D9();
        }
        break;  // fall through so DefWindowProc continues processing

    // polish: during the modal sizemove loop, WM_SIZE/WM_MOVE
    // fire continuously. Each one calls RefreshScreenPosition so
    // the popup tracks main's new position. The cached client-coord
    // rect from the last layout/viewport-rect message is the source
    // — React's ResizeObserver will fire AFTER the sizemove loop
    // exits, sending a fresh layout/viewport-rect, but in the
    // meantime the popup at least stays anchored to roughly the
    // right place via owner-client translation. No
    // WM_ENTERSIZEMOVE/EXITSIZEMOVE handling: hiding the popup
    // during resize just exposes the bare WebView2 transparent
    // region (which paints white through the parent's null brush),
    // which is worse than a slightly-stale-sized popup.

    case WM_DESTROY:
        KillTimer(hwnd, kStatsTimerId);
        //: stop autosave + delete THIS session's autosave files on a
        // clean exit so no orphan prompts on the next launch. A crash skips
        // WM_DESTROY, leaving the orphan for recovery — exactly the point.
        if (!useTestHost)
        {
            KillTimer(hwnd, Autosave::RECENT_TIMER_ID);
            KillTimer(hwnd, Autosave::STABLE_TIMER_ID);
            Autosave::DeleteOurSession();
        }
        // G8: release the class background brush. Per
        // WNDCLASSEX docs the system would free it on UnregisterClass,
        // but the class is never explicitly unregistered. Doing it
        // here is safe for the single-window-per-process host.
        if (m_classBrush)
        {
            DeleteObject(m_classBrush);
            m_classBrush = nullptr;
        }
        // G5: unregister the WebMessageReceived handler
        // explicitly before tearing down webView, mirroring the
        // accelKeyTok pattern below. The handler lambda captures
        // `this`; explicit unsubscribe before destruction prevents
        // any in-flight message dispatch from racing with
        // HostWindowImpl teardown.
        if (webView && webMessageTok.value != 0)
        {
            webView->remove_WebMessageReceived(webMessageTok);
            webMessageTok = {};
        }
        // G11: unsubscribe the nav/new-window/permission handlers
        // before webView teardown, same rationale as the G5 removal above
        // (the lambdas capture `this`).
        if (webView)
        {
            if (navStartingTok.value != 0)
            {
                webView->remove_NavigationStarting(navStartingTok);
                navStartingTok = {};
            }
            if (newWindowTok.value != 0)
            {
                webView->remove_NewWindowRequested(newWindowTok);
                newWindowTok = {};
            }
            if (permissionTok.value != 0)
            {
                webView->remove_PermissionRequested(permissionTok);
                permissionTok = {};
            }
        }
        if (webController)
        {
            // Unregister the accelerator hook before closing the controller
            // so the callback lambda (which captures `this`) is never invoked
            // after HostWindowImpl starts destructing.
            if (accelKeyTok.value != 0)
            {
                webController->remove_AcceleratorKeyPressed(accelKeyTok);
                accelKeyTok = {};
            }
            webController->Close();
            webController.Reset();
        }
        webView.Reset();
        // Phase 3 Stage 3b/3d: release composition controller +
        // DComp tree. Order matters per dxgi_spike.cpp:783-818:
        // controller is released AFTER webController->Close() (which
        // already settles WebView2's pending work) and BEFORE
        // m_compositor.reset() (so the Compositor's defensive
        // put_RootVisualTarget(nullptr) in its dtor still has a live
        // controller via its internal Impl::controller ComPtr — the
        // Compositor holds its own reference). m_compositor.reset()
        // then releases the visual tree.
        //
        // Stage 3d: unregister the CursorChanged handler before
        // releasing the controller so the lambda (which captures
        // `this`) can't fire after HostWindowImpl starts destructing.
        // Same pattern as AcceleratorKeyPressed above.
        if (m_compositionController && m_cursorChangedTok.value != 0)
        {
            m_compositionController->remove_CursorChanged(m_cursorChangedTok);
            m_cursorChangedTok = {};
        }
        m_webViewCursor = nullptr;
        m_compositionController.Reset();
        // Phase 3 Stage 5 — clear LayoutBroker's pointer BEFORE
        // releasing the Compositor so any late SetSceneRect dispatch
        // (e.g. an in-flight BridgeDispatcher message that's already
        // past the WM_DESTROY barrier in the message-pump shutdown
        // sequence) doesn't dereference a freed Compositor.
        layout.SetCompositor(nullptr);
        m_compositor.reset();
        //: detach the compositor from Engine BEFORE either is
        // destroyed so Render() (if scheduled before WM_QUIT drains
        // the queue) can't dereference a freed compositor. Drop the
        // compositor first since Engine owns the D3D9 device the
        // compositor's resources are bound to.
        // Phase 1: drop the FramePublisher first — it holds a
        // raw pointer back into the compositor, so this MUST go before
        // alphaCompositor.reset().
        m_framePublisher.reset();
        // Phase 2: drop InputDispatcher too. It holds the
        // popup HWND raw; the popup itself is destroyed below as part
        // of the standard WM_DESTROY cleanup. Order between the two
        // archC publishers doesn't matter — neither references the
        // other — but tearing them down before the engine/compositor
        // matches the FramePublisher pattern.
        m_inputDispatcher.reset();
        if (engine) engine->SetAlphaCompositor(nullptr);
        layout.SetAlphaCompositor(nullptr);
        alphaCompositor.reset();
        //: engine owns its D3D9 device; just drop the engine and it
        // tears the device down in its destructor.
        engine.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT HostWindowImpl::ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        //: rendering happens on the main-loop idle path
        // (PeekMessage-drain → render). WM_PAINT just validates the
        // invalid region so Windows doesn't keep firing it. Same pattern
        // as legacy src/main.cpp's main loop, where WM_PAINT also does
        // nothing visible and the idle render owns the pipeline.
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        // Suppress GDI erase — D3D9 owns the surface.
        return 1;

    // ---------------------------------------------------------------
    // viewport interaction — camera controls.
    //
    // Mirrors the legacy handler at src/main.cpp:2917-3060. The math
    // for MOVE / ROTATE / ZOOM is lifted verbatim from legacy so the
    // user's muscle-memory carries over: drag delta scales /2.0f for
    // rotate (full-window-width drag ≈ 180°), distance/1000 for
    // MOVE multiplier, sqrt(olddist)-based scaling for ZOOM.
    //
    // Scope: camera only. The shift-click-to-spawn path
    // (legacy 2956) depends on the MouseCursor Object3D port and is
    // explicitly deferred. The status-bar mouse-coord push
    // (legacy 3041) is a Screen 1 polish item.
    //
    // Engine state emission: SetCamera bypasses the dispatcher
    // setter ladder, so we must call EmitEngineStateChanged()
    // ourselves after each mutation to keep React subscribers in
    // sync. View state is not file content — no markDirty here
    // (matches legacy, which never calls SetFileChanged for camera).
    // ---------------------------------------------------------------
    case WM_LBUTTONDOWN:
    {
        if (!engine) return 0;
        // Phase 2 smoke instrumentation — verify what wParam
        // actually arrived from the synthesized PostMessage.
        Log("[ArchC-engine] WM_LBUTTONDOWN wp=0x%llx MK_SHIFT=%d MK_CONTROL=%d hasPS=%d emitters=%zu attached=%d\n",
            static_cast<unsigned long long>(wp),
            (wp & MK_SHIFT) ? 1 : 0,
            (wp & MK_CONTROL) ? 1 : 0,
            particleSystem ? 1 : 0,
            particleSystem ? particleSystem->getEmitters().size() : 0,
            m_attachedParticleSystem ? 1 : 0);
        // Phase 2: in archC mode the popup is hidden and WebView2
        // owns keyboard routing; we forward keystrokes through the bridge.
        // SetFocus on the hidden popup briefly succeeds (visibility isn't
        // a precondition; WS_EX_NOACTIVATE only blocks user-driven
        // activation), then OS focus management snaps it back, firing a
        // spurious WM_KILLFOCUS that the defensive kill below interprets
        // as "user Alt-Tab'd, drop the spawn." Skip SetFocus to break
        // the focus-thrash → kill loop. Legacy mode keeps the original
        // semantic (popup must own focus for WM_KEYDOWN VK_SHIFT spawn).
        if (!m_archCMode) SetFocus(hwnd);
        // polish: Shift+LMB also triggers cursor-bound spawn. The
        // legacy keydown-only path (case WM_KEYDOWN below) requires the
        // viewport HWND to have focus when Shift is pressed, but
        // WebView2 holds focus from the React UI by default — the
        // user's typical "shift-then-click" gesture swallows the
        // initial WM_KEYDOWN in WebView2 and the spawn never fires.
        // By trapping the click while MK_SHIFT is set, we provide a
        // click-based entry point that doesn't depend on WM_KEYDOWN
        // routing. Skip the camera drag so the spawn doesn't compete
        // with a MOVE drag. Release on Shift-keyup or LBUTTONUP — the
        // existing WM_KEYUP handler kills the attached instance.
        // Legacy parity: if a cursor-bound preview already exists (spawned
        // by an earlier WM_KEYDOWN VK_SHIFT or by the B1.3.1 fallback below),
        // LMB-down enters OBJECT_Z drag mode for height adjustment. LMB-up
        // will then detach the preview, placing it permanently in the scene.
        // Matches legacy src/main.cpp:2891-2898. Do NOT enter a camera drag
        // — placement is the entire intent of this click while a preview
        // is alive.
        if (m_attachedParticleSystem != nullptr)
        {
            m_dragMode     = DragMode::OBJECT_Z;
            m_dragStartCam = engine->GetCamera();
            m_dragStartX   = (short)LOWORD(lp);
            m_dragStartY   = (short)HIWORD(lp);
            SetCapture(hwnd);
            Log("[ArchC-engine] LMB-down OBJECT_Z drag (placing attached=%p)\n",
                static_cast<void*>(m_attachedParticleSystem));
            return 0;
        }
        // B1.3.1 round 5 fallback: Shift+LMB with no existing preview spawns
        // one in-place (covers the case where WM_KEYDOWN VK_SHIFT didn't
        // fire because WebView2 held focus). Then immediately enter
        // OBJECT_Z so the user can drag-Z in the same gesture and LMB-up
        // places it.
        if ((wp & MK_SHIFT) && particleSystem && !particleSystem->getEmitters().empty())
        {
            int cx = (short)LOWORD(lp);
            int cy = (short)HIWORD(lp);
            m_lastCursorX = cx;
            m_lastCursorY = cy;
            D3DXVECTOR3 pos;
            GetCursorPos3D(engine.get(), (short)cx, (short)cy, pos);
            m_mouseCursor.SetPosition(pos);
            m_attachedParticleSystem =
                engine->SpawnParticleSystem(*particleSystem, &m_mouseCursor);
            Log("[ArchC-engine] SHIFT+LMB spawn cx=%d cy=%d pos=(%.3f,%.3f,%.3f) result=%p\n",
                cx, cy, pos.x, pos.y, pos.z,
                static_cast<void*>(m_attachedParticleSystem));
#ifndef NDEBUG
            // / handoff item 14] Mirror the cursor-unproject
            // diagnostic at this alternate spawn entry. Consistent
            // grep prefix lets all three call sites (WM_MOUSEMOVE
            // throttled emit, WM_KEYDOWN VK_SHIFT, WM_LBUTTONDOWN
            // SHIFT-fallback) be filtered together.
            int dx, dy, dw, dh;
            const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
            Log("[cursor-unproject] SHIFT+LMB in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
                cx, cy,
                dscene ? "scene" : "full-rt",
                dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
                pos.x, pos.y, pos.z);
#endif
            m_dragMode     = DragMode::OBJECT_Z;
            m_dragStartCam = engine->GetCamera();
            m_dragStartX   = cx;
            m_dragStartY   = cy;
            SetCapture(hwnd);
            return 0;
        }
        // Plain LMB drag — camera MOVE / ZOOM (no preview involved).
        m_dragMode     = (wp & MK_CONTROL) ? DragMode::ZOOM : DragMode::MOVE;
        m_dragStartCam = engine->GetCamera();
        m_dragStartX   = (short)LOWORD(lp);
        m_dragStartY   = (short)HIWORD(lp);
        SetCapture(hwnd);
        return 0;
    }
    case WM_RBUTTONDOWN:
    {
        if (!engine) return 0;
        m_dragMode     = (wp & MK_CONTROL) ? DragMode::ZOOM : DragMode::ROTATE;
        m_dragStartCam = engine->GetCamera();
        m_dragStartX   = (short)LOWORD(lp);
        m_dragStartY   = (short)HIWORD(lp);
        SetCapture(hwnd);
        // Phase 2: see WM_LBUTTONDOWN — skip SetFocus in archC
        // to avoid the spurious WM_KILLFOCUS → defensive-kill loop.
        if (!m_archCMode) SetFocus(hwnd);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        // Legacy parity: if a cursor-bound preview was being dragged
        // for placement (OBJECT_Z mode, or any state with an attached
        // preview), DETACH it now. After Detach the system stays in
        // the world at its current position and continues to emit —
        // it is no longer parented to m_mouseCursor. The user can
        // then click again (while still holding Shift) to spawn a
        // fresh preview, repeating the click-to-place gesture.
        // Matches legacy src/main.cpp:2877-2883.
        if (m_attachedParticleSystem && engine)
        {
            Log("[ArchC-engine] LMB-up placing attached=%p (Detach, system stays alive)\n",
                static_cast<void*>(m_attachedParticleSystem));
            engine->DetachParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem = nullptr;
        }
        m_dragMode = DragMode::NONE;
        ReleaseCapture();
        return 0;
    }
    case WM_RBUTTONUP:
    {
        m_dragMode = DragMode::NONE;
        ReleaseCapture();
        return 0;
    }
    case WM_CAPTURECHANGED:
    {
        // Capture lost (Alt-Tab away mid-drag, foreign SetCapture, etc.).
        // Drop drag state so the next mouse-move doesn't ride a stale
        // start camera.
        m_dragMode = DragMode::NONE;
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!engine) return 0;

        int mx = (short)LOWORD(lp);
        int my = (short)HIWORD(lp);
        m_lastCursorX = mx;
        m_lastCursorY = my;

        // Legacy parity: in OBJECT_Z drag (placing a cursor-bound preview),
        // only Z tracks the drag. X/Y stay frozen at the click position so
        // the user can rake the mouse vertically to set height without the
        // preview sliding sideways. Matches legacy src/main.cpp:2939-2948.
        if (m_dragMode == DragMode::OBJECT_Z)
        {
            long y = my - m_dragStartY;
            D3DXVECTOR3 diff = m_dragStartCam.Target - m_dragStartCam.Position;
            float len = D3DXVec3Length(&diff);
            D3DXVECTOR3 pos = m_mouseCursor.GetPosition();
            pos.z = -static_cast<float>(y) * len / 1000.0f;
            m_mouseCursor.SetPosition(pos);
            return 0;
        }

        // shift-click-to-spawn: always-update cursor block, regardless
        // of (non-OBJECT_Z) drag mode. Mirrors legacy src/main.cpp:2982-2987
        // — without this, the attached ParticleSystemInstance (parented to
        // m_mouseCursor via Object3D) wouldn't track the mouse during
        // Shift-hold. Cache the (x,y) so WM_KEYDOWN can use it for the
        // spawn coords (WM_KEYDOWN's lParam is NOT mouse coords; legacy
        // bug at src/main.cpp:2960).
        D3DXVECTOR3 cursorWorld;
        GetCursorPos3D(engine.get(), (short)mx, (short)my, cursorWorld);
        m_mouseCursor.SetPosition(cursorWorld);

        // (Group A): push the world-space cursor to the React
        // status bar, throttled. 33 ms ≈ 30 Hz — fast enough to read,
        // slow enough that the bridge channel doesn't bottleneck.
        const DWORD now = GetTickCount();
        if (dispatcher && (now - m_lastCursorEmitTick) >= 33u)
        {
            m_lastCursorEmitTick = now;
            dispatcher->EmitCursorPosition3D(cursorWorld.x, cursorWorld.y, cursorWorld.z);
#ifndef NDEBUG
            // / handoff item 14] Throttled diagnostic for the
            // cursor-unproject path. Piggybacks on the bridge-emit gate
            // so the cadence is ~30 Hz (rather than per-WM_MOUSEMOVE,
            // which is 60+ Hz and would flood host.log). `mode` names
            // which viewport GetCursorPos3D used — `scene` under
            // composition mode (architecture C), `full-rt` under legacy
            // mode (architecture A, or pre-scene-rect-dispatch boot).
            int dx, dy, dw, dh;
            const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
            Log("[cursor-unproject] in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
                mx, my,
                dscene ? "scene" : "full-rt",
                dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
                cursorWorld.x, cursorWorld.y, cursorWorld.z);
#endif
        }

        if (m_dragMode == DragMode::NONE) return 0;

        long x = mx - m_dragStartX;
        long y = my - m_dragStartY;

        Engine::Camera camera = m_dragStartCam;
        D3DXVECTOR3    orthVec;
        D3DXVECTOR3    diff = m_dragStartCam.Position - m_dragStartCam.Target;

        // Orthogonal vector in the camera plane (legacy line 2997-2998).
        D3DXVec3Cross(&orthVec, &diff, &camera.Up);
        D3DXVec3Normalize(&orthVec, &orthVec);

        if (m_dragMode == DragMode::ROTATE)
        {
            // Orbit Position around Target. Z rotation around camera-up
            // axis (horizontal drag); XY rotation around orthVec
            // (vertical drag). /2.0f keeps a full-window drag at ~180°.
            D3DXMATRIX rotateXY, rotateZ, rotate;
            D3DXMatrixRotationZ(&rotateZ, -D3DXToRadian(x / 2.0f));
            D3DXMatrixRotationAxis(&rotateXY, &orthVec, D3DXToRadian(y / 2.0f));
            D3DXMatrixMultiply(&rotate, &rotateXY, &rotateZ);
            D3DXVec3TransformCoord(&camera.Position, &diff, &rotate);
            camera.Position += camera.Target;
        }
        else if (m_dragMode == DragMode::MOVE)
        {
            // Translate Target (Position rides along). Multiplier scales
            // with distance so a far camera moves proportionally faster —
            // legacy comment: "Large distance: move a lot, small
            // distance: move a little".
            D3DXVECTOR3 Up;
            D3DXVec3Cross(&Up, &orthVec, &diff);
            D3DXVec3Normalize(&Up, &Up);

            float multiplier = D3DXVec3Length(&diff) / 1000;

            camera.Target  += (float)x * multiplier * orthVec;
            camera.Target  += (float)y * multiplier * Up;
            camera.Position = diff + camera.Target;
        }
        else if (m_dragMode == DragMode::ZOOM)
        {
            // Scale (Position - Target) by a sqrt(distance)-based
            // factor. Floor at 1.0f to prevent flipping through the
            // target. -y so dragging up zooms in (matches legacy).
            float olddist = D3DXVec3Length(&diff);
            float newdist = max(1.0f, olddist - sqrtf(olddist) * (float)-y);
            D3DXVec3Scale(&camera.Position, &diff, newdist / olddist);
            camera.Position += camera.Target;
        }

        engine->SetCamera(camera);
        if (dispatcher) dispatcher->EmitEngineStateChanged();
        return 0;
    }
    // -----------------------------------------------------------------
    // shift-click-to-spawn — cursor-bound particle system.
    //
    // Hold Shift over the viewport to spawn an instance of the active
    // ParticleSystem parented to m_mouseCursor. Drag the mouse to fling
    // it around; release Shift to kill it. Matches legacy
    // src/main.cpp:2945-2966.
    //
    // Cursor-coords-on-KEYDOWN: WM_KEYDOWN's lParam is repeat-count +
    // scan-code + flags — NOT mouse coords. Legacy reads `LOWORD(lParam),
    // HIWORD(lParam)` and gets garbage; instead we use m_lastCursorX/Y
    // cached from WM_MOUSEMOVE. Fallback (cache stale or zero at boot):
    // GetCursorPos + ScreenToClient.
    // -----------------------------------------------------------------
    case WM_KEYDOWN:
    {
        if (wp != VK_SHIFT || !engine) break;
        // Filter auto-repeats. WM_KEYDOWN sets bit 30 of lParam on
        // repeat presses; clear bit 30 means initial press. Legacy
        // `(~lParam & 0x40000000)` test.
        if (lp & 0x40000000) return 0;
        // Spawn precondition: a non-empty ParticleSystem and no
        // attached instance already. Empty-system guard goes beyond
        // legacy's `particleSystem != NULL` to also require >= 1
        // root emitter — SpawnParticleSystem on an emitter-less system
        // misbehaves.
        if (m_attachedParticleSystem != nullptr) return 0;
        if (!particleSystem || particleSystem->getEmitters().empty()) return 0;

        // Resolve cursor coords. Prefer the cached MOUSEMOVE position;
        // fall back to GetCursorPos+ScreenToClient if the cache hasn't
        // been seeded (e.g. user pressed Shift before moving the mouse
        // over the viewport at all).
        int cx = m_lastCursorX;
        int cy = m_lastCursorY;
        if (cx == 0 && cy == 0)
        {
            POINT pt = {};
            if (GetCursorPos(&pt))
            {
                ScreenToClient(hwnd, &pt);
                cx = pt.x;
                cy = pt.y;
            }
        }

        D3DXVECTOR3 pos;
        GetCursorPos3D(engine.get(), (short)cx, (short)cy, pos);
        m_mouseCursor.SetPosition(pos);
        m_attachedParticleSystem =
            engine->SpawnParticleSystem(*particleSystem, &m_mouseCursor);
#ifndef NDEBUG
        // / handoff item 14] One-shot diagnostic at the actual
        // spawn site so a misplaced spawn can be tied to the input
        // coords + viewport in host.log without re-running with a
        // breakpoint. Per-Shift-press, not per-frame, so untrottled.
        int dx, dy, dw, dh;
        const bool dscene = engine->GetSceneViewport(dx, dy, dw, dh);
        Log("[cursor-unproject] SPAWN in=(%d,%d) mode=%s vp=(%d,%d,%d,%d) world=(%.2f,%.2f,%.2f)\n",
            cx, cy,
            dscene ? "scene" : "full-rt",
            dscene ? dx : 0, dscene ? dy : 0, dscene ? dw : 0, dscene ? dh : 0,
            pos.x, pos.y, pos.z);
#endif
        return 0;
    }
    case WM_KEYUP:
    {
        if (wp != VK_SHIFT) break;
        if (m_attachedParticleSystem && engine)
        {
            Log("[ArchC-kill] WM_KEYUP VK_SHIFT killing attached=%p\n",
                static_cast<void*>(m_attachedParticleSystem));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem = nullptr;
        }
        return 0;
    }
    case WM_KILLFOCUS:
    {
        // Defensive: if the viewport loses focus while Shift is held
        // (Alt-Tab away, foreign focus steal), WM_KEYUP may never arrive
        // and the attached instance leaks. Drop it here.
        //
        // Phase 2: in archC mode the popup is hidden, never
        // genuinely owns focus, but receives spurious WM_KILLFOCUS from
        // Win32 focus churn whenever ANY focus assignment touches it
        // (other apps activating, modal dialogs, etc.). Treating those
        // as user-Alt-Tab triggers and killing the cursor-bound spawn
        // is a regression. The legitimate Alt-Tab case is now covered
        // by the window.blur → viewport/input { type: "blur" } bridge
        // path (renderer-side), which goes through THIS handler too —
        // so we still need the kill, just not unconditionally. Gate
        // the kill on legacy mode for now; a future refinement could
        // distinguish bridge-routed blur from spurious OS focus events
        // via a sentinel wParam from InputDispatcher.
        if (!m_archCMode && m_attachedParticleSystem && engine)
        {
            Log("[ArchC-kill] WM_KILLFOCUS killing attached=%p\n",
                static_cast<void*>(m_attachedParticleSystem));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem = nullptr;
        }
        else if (m_archCMode && m_attachedParticleSystem)
        {
            Log("[ArchC-kill] WM_KILLFOCUS suppressed (archC mode, attached=%p preserved)\n",
                static_cast<void*>(m_attachedParticleSystem));
        }
        return 0;
    }
    case WM_DESTROY:
    {
        // Viewport HWND is going away. Defensively drop any attached
        // instance before the Engine tears down (Engine reset happens
        // on the main window's WM_DESTROY which fires after this).
        if (m_attachedParticleSystem && engine)
        {
            Log("[ArchC-kill] WM_DESTROY killing attached=%p\n",
                static_cast<void*>(m_attachedParticleSystem));
            engine->KillParticleSystem(m_attachedParticleSystem);
            m_attachedParticleSystem = nullptr;
        }
        return 0;
    }

    case WM_MOUSEWHEEL:
    {
        // Wheel-zoom only when no drag is in progress (legacy line 3046).
        // wParam high word is the wheel delta in WHEEL_DELTA units (120).
        if (m_dragMode != DragMode::NONE || !engine) return 0;

        Engine::Camera camera = engine->GetCamera();
        D3DXVECTOR3    diff   = camera.Position - camera.Target;

        float olddist = D3DXVec3Length(&diff);
        float wheel   = (float)((SHORT)HIWORD(wp)) / (float)WHEEL_DELTA;
        float newdist = max(1.0f, olddist - sqrtf(olddist) * wheel);
        D3DXVec3Scale(&camera.Position, &diff, newdist / olddist);
        camera.Position += camera.Target;

        engine->SetCamera(camera);
        if (dispatcher) dispatcher->EmitEngineStateChanged();
        return 0;
    }
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

namespace {

LRESULT CALLBACK HostMainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (auto* self = reinterpret_cast<HostWindowImpl*>(g_self))
        return self->MainWndProc(hwnd, msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HostViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (auto* self = reinterpret_cast<HostWindowImpl*>(g_self))
        return self->ViewportWndProc(hwnd, msg, wp, lp);
    return DefWindowProc(hwnd, msg, wp, lp);
}

// rendering-fidelity] Composite-output capture for --capture. The
// engine-RT capture (AlphaCompositor::CaptureSnapshotToFile) shows the
// engine's pre-composite pixels; this captures the FINAL DWM/Direct-
// Composition-composited window — what the user actually sees — so the
// composition-only darkening class (d9b690f) is testable offline.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

static bool GetPngClsidHW(CLSID& out)
{
    UINT num = 0, bytes = 0;
    if (Gdiplus::GetImageEncodersSize(&num, &bytes) != Gdiplus::Ok || bytes == 0) return false;
    std::vector<BYTE> buf(bytes);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, bytes, info) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i)
        if (wcscmp(info[i].MimeType, L"image/png") == 0) { out = info[i].Clsid; return true; }
    return false;
}

// PrintWindow with PW_RENDERFULLCONTENT (Win8.1+) is required to capture
// DirectComposition / WebView2 composited content; plain BitBlt or
// PrintWindow(0) returns black for composed swapchain surfaces.
static bool CaptureWindowToPng(HWND hwnd, const std::wstring& path)
{
    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) return false;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC     screen = GetDC(nullptr);
    HDC     mem    = CreateCompatibleDC(screen);
    HBITMAP bmp    = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ oldb   = SelectObject(mem, bmp);
    const BOOL pw  = PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT);
    SelectObject(mem, oldb);

    bool saved = false;
    if (pw)
    {
        CLSID clsid = {};
        if (GetPngClsidHW(clsid))
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

} // namespace

// ---------- Run ----------

int HostWindowImpl::Run(int nCmdShow)
{
    OpenLog();

    if (useTestHost)
    {
        Log("[host] === --test-host MODE: CDP on :9222 + DevTools enabled ===\n");
    }

    // Task 1.4: when --dev-ui is requested, verify the Vite dev server
    // is reachable before proceeding. A missing server is a common mistake
    // (forgot to run `pnpm dev`) — fail fast with a clear message rather than
    // navigating to an empty page.
    if (useDevUi)
    {
        Log("[host] dev-ui: probing http://localhost:5174/ ...\n");
        if (!ProbeDevServer())
        {
            Log("[host] dev-ui: probe failed — server not reachable\n");
            CloseLog();
            MessageBoxW(nullptr,
                L"Dev UI mode requested but no dev server detected at http://localhost:5174.\n\n"
                L"Did you forget to run `pnpm dev` in `web/apps/editor/`?\n\n"
                L"Start the dev server in one terminal:\n"
                L"    cd web/apps/editor\n"
                L"    pnpm dev\n\n"
                L"Then relaunch ParticleEditor.exe --new-ui --dev-ui.",
                L"Dev UI server not detected",
                MB_OK | MB_ICONERROR);
            return 1;
        }
        Log("[host] dev-ui: probe OK — navigating to Vite server\n");
    }

    // DPI awareness — PMv2 so child-window coords are physical pixels and
    // match what React sends from getBoundingClientRect under WebView2.
    // The PoC ran with this and the visual gate passed.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // COM init — WebView2 needs an STA. main.cpp doesn't call
    // CoInitializeEx before invoking host::Run, so we do it here.
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("[host] CoInitializeEx hr=0x%08lx\n", coHr);

    // Task 1.5: verify the WebView2 Evergreen runtime is present before
    // creating any window. If missing the dialog is the only visible artifact.
    if (!WebView2RuntimeInstalled())
    {
        Log("[host] WebView2 runtime not found — showing install dialog\n");
        int r = MessageBoxW(nullptr,
            L"AloParticleEditor requires the Microsoft Edge WebView2 Runtime.\n\n"
            L"Install it from https://aka.ms/webview2 and relaunch.\n\n"
            L"Click OK to open the download page in your browser.\n"
            L"Click Cancel to exit.",
            L"WebView2 Runtime Required",
            MB_OKCANCEL | MB_ICONERROR);
        if (r == IDOK)
        {
            ShellExecuteW(nullptr, L"open", L"https://aka.ms/webview2",
                          nullptr, nullptr, SW_SHOWNORMAL);
        }
        CoUninitialize();
        CloseLog();
        return 1;
    }
    Log("[host] WebView2 runtime detected — proceeding\n");

    // B1.3.1.1: GDI+ init for AlphaCompositor::CaptureSnapshotPng (the
    // modal frosted-glass backdrop). One-time per process; matching
    // Gdiplus::GdiplusShutdown runs right before CoUninitialize at the
    // bottom of this function. The two earlier early-return paths
    // (CreateWindowEx failure, InitWebView2 failure) skip shutdown
    // because the process is dying anyway and the leaked allocation
    // is bounded.
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    g_self = this;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostMainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    // IDI_LOGO == 109 in src/Resources/resource.h. Fall back to the
    // generic application icon if the resource isn't linked in (e.g.
    // running the host TU as part of a stripped-down test binary).
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(109));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kHostWindowClassName;
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    // polish: paint the parent in the same dark purple as the
    // D3D9 viewport's clear color (engine.cpp m_background default).
    // When the popup is briefly mispositioned during a window resize
    // — the popup tracks main on each WM_SIZE but its cached size
    // lags React's ResizeObserver — the uncovered area paints in
    // dark purple instead of the WebView2 transparent-region's white
    // default. Smoothly indistinguishable from the actual viewport
    // until React resends the rect.
    // G8: stash the brush so WM_DESTROY can DeleteObject it.
    // Pre-fix the CreateSolidBrush handle was assigned directly to the
    // class without being stored, and no UnregisterClass call exists,
    // so the brush leaked for process lifetime. The host only ever has
    // one instance per process; storing as a member is the simplest
    // ownership shape.
    m_classBrush = CreateSolidBrush(RGB(0x14, 0x08, 0x34));
    wc.hbrBackground = m_classBrush;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{};
    vc.cbSize        = sizeof(vc);
    vc.lpfnWndProc   = HostViewportWndProc;
    vc.hInstance     = hInstance;
    vc.lpszClassName = kHostViewportClassName;
    vc.hbrBackground = nullptr;  // D3D9 owns the surface
    // Without an explicit hCursor on the popup's class, Windows
    // leaves whatever cursor was active when the pointer left the
    // previous window — so the main HWND's resize-edge cursor
    // would persist while hovering inside the viewport popup if
    // the user crossed in from the right border.
    vc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&vc);

    hMain = CreateWindowExW(
        0, kHostWindowClassName, L"AloParticleEditor",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, kInitialWidth, kInitialHeight,
        nullptr, nullptr, hInstance, nullptr);
    if (!hMain)
    {
        Log("[host] CreateWindowEx parent failed (gle=%lu)\n", GetLastError());
        g_self = nullptr;
        CoUninitialize();
        CloseLog();
        return 1;
    }

    // Theme the native title bar to the OS app theme at startup so it
    // doesn't flash a white caption before React mounts and pushes the
    // real theme via host/backing-color (BridgeDispatcher re-applies on
    // every theme toggle). The app's initial theme also follows the OS
    // preference, so the two agree for the common case. AppsUseLightTheme
    // (HKCU) is 0 when the OS app theme is dark.
    {
        DWORD appsUseLight = 1, sz = sizeof(appsUseLight);
        RegGetValueW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &appsUseLight, &sz);
        BOOL dark = (appsUseLight == 0) ? TRUE : FALSE;
        DwmSetWindowAttribute(hMain, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark, sizeof(dark));
    }

    // Construct dispatcher AFTER hMain exists (it captures the WebView2
    // pointer-to-PostWebMessageAsString via its EmitFn). engine ptr is
    // wired in WM_CREATE when the Engine is built.
    auto emitFn = [this](const std::string& js)
    {
        if (!webView) return;
        std::wstring w = Utf8ToUtf16(js);
        webView->PostWebMessageAsJson(w.c_str());
    };
    dispatcher = std::make_unique<BridgeDispatcher>(/*engine*/nullptr, layout, accelerator, emitFn,
                                                    /*useTestHost*/useTestHost);
    dispatcher->SetUndoStack(&undoStack);
    dispatcher->SetHostHwnd(hMain);
    // D6: ModManager is already discovered + restored in the impl
    // ctor. Bind it so the dispatcher can service `mods/list`,
    // `mods/select`, `mods/refresh` and include `activeModPath` in
    // snapshots.
    dispatcher->SetModManager(modManager.get());

    // WM_CREATE fired during CreateWindowEx; viewport + engine now exist.
    // Wire the engine into the dispatcher (it was null when we constructed
    // the dispatcher because hMain hadn't been created yet). LayoutBroker
    // already received the engine inside WM_CREATE; re-binding here is a
    // defensive no-op for symmetry with the dispatcher path.
    if (engine)
    {
        dispatcher->SetEngine(engine.get());
        layout.SetEngine(engine.get());
    }

    // host-state plumbing: construct the live ParticleSystem +
    // SpawnerDriver and hand pointer-to-pointer access to the
    // dispatcher. file/new and file/open below will swap the
    // particleSystem unique_ptr; the dispatcher reads through
    // `*m_pParticleSystem` to always see the current instance.
    // Mirrors legacy seed: DoNewFile() at src/main.cpp:1289 starts
    // with an empty ParticleSystem + one root emitter, so do the
    // same here for parity with the React UI's "fresh untitled" state.
    particleSystem = std::make_unique<ParticleSystem>();
    particleSystem->addRootEmitter();
    spawnerDriver  = std::make_unique<SpawnerDriver>();
    dispatcher->BindHostState(&particleSystem, spawnerDriver.get(), &fileManager);
    // Seed the dirty-bit baseline against the freshly-bound boot-state
    // ParticleSystem so Ctrl+Z back to it clears dirty without needing
    // a File → New first. file/new + file/open + file/save re-seed via
    // their own paths.
    dispatcher->ResetSavedBaseline();
    // shift-click-to-spawn: expose the attached-system slot so
    // file/new + file/open can kill any in-flight cursor-bound instance
    // before swapping the ParticleSystem under it.
    dispatcher->BindAttachedSystem(&m_attachedParticleSystem);
    // Phase 2: hand the InputDispatcher to the bridge so
    // `viewport/input` requests route into it. Nullable — in legacy-
    // popup mode this stays null and the handler is a no-op ack.
    dispatcher->SetInputDispatcher(m_inputDispatcher.get());
    Log("[host] host state bound (particleSystem + spawnerDriver)\n");

    HRESULT hr = InitWebView2();
    if (FAILED(hr))
    {
        wchar_t msg[256];
        swprintf(msg, 256,
                 L"WebView2 initialisation failed (0x%08lx).\n"
                 L"Is the Evergreen runtime installed?", hr);
        MessageBoxW(hMain, msg, L"AloParticleEditor", MB_ICONERROR);
        DestroyWindow(hMain);
        g_self = nullptr;
        CoUninitialize();
        CloseLog();
        return 1;
    }

    // B1.4 T4c: size the popup HWND to the main window's
    // full client rect just before showing the window. Without this,
    // the popup is stuck at CreateWindowExW's bootstrap rect
    // (screen 16,16,320,240) and renders as a tiny preview at the
    // monitor's top-left until the user first resizes. By this
    // point WM_CREATE has completed, the engine + AlphaCompositor +
    // particleSystem are fully bound, and Engine::Reset can handle
    // the resize cleanly.
    layout.ApplyFullClient();

    // Phase 2: in (canvas-in-DOM) mode, hide the
    // viewport popup. The popup still spans the full main client
    // (ApplyFullClient above), the D3D9 swapchain on its hidden HWND
    // keeps rendering at the popup-client size, and FramePublisher
    // continues reading the AlphaCompositor's pre-stamp DIB —
    // `UpdateLayeredWindow` becomes a no-op since the window is
    // hidden (cleanup of that wasted call is a Phase 5 follow-up).
    // The canvas in the WebView2 DOM is now the visible viewport;
    // input flows through InputDispatcher rather than the OS-routed
    // path.
    if (m_archCMode)
    {
        HWND hPopup = layout.GetViewport();
        if (hPopup) ShowWindow(hPopup, SW_HIDE);
        Log("[ArchC] viewport popup hidden (canvas-in-DOM is the visible surface)\n");
    }

    ShowWindow(hMain, nCmdShow);
    UpdateWindow(hMain);

    // rendering-fidelity] --capture: load the requested .alo now,
    // using the exact swap+notify sequence file/open uses (BindHostState
    // bound &particleSystem to the dispatcher, so this slot is what the
    // render loop below reads via particleSystem.get()). The loop then
    // renders m_captureFrames frames and writes the engine RT to a PNG.
    const bool captureMode = !m_captureAlo.empty() && !m_capturePng.empty();
    bool captureFailed = false;
    if (captureMode)
    {
        if (!engine)
        {
            Log("[capture] no engine available — cannot capture\n");
            captureFailed = true;
        }
        else
        {
            // Select the mod that owns this .alo BEFORE loading, so its
            // texture overrides (Mod etc.) resolve instead
            // of base-game art. The editor does this on mod-select; a
            // direct --capture load must do it explicitly or particles
            // render with the wrong textures. Match the .alo path against
            // discovered mods by case-insensitive path prefix; SelectMod
            // swaps FileManager's mod path + reloads textures (engine is
            // already bound via SetEngine in WM_CREATE).
            if (modManager)
            {
                bool matched = false;
                for (const auto& mod : modManager->GetMods())
                {
                    const size_t n = mod.path.size();
                    if (n > 0 && _wcsnicmp(m_captureAlo.c_str(), mod.path.c_str(), n) == 0
                        && (m_captureAlo.size() == n
                            || m_captureAlo[n] == L'\\' || m_captureAlo[n] == L'/'))
                    {
                        modManager->SelectMod(mod.path);
                        Log("[capture] selected mod for .alo: %ls\n", mod.path.c_str());
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    Log("[capture] no mod matched .alo path — using base-game/restored textures\n");
            }

            std::string err;
            std::unique_ptr<ParticleSystem> loaded = LoadParticleSystem(m_captureAlo, &err);
            if (!loaded)
            {
                Log("[capture] LoadParticleSystem(%ls) failed: %s\n",
                    m_captureAlo.c_str(), err.c_str());
                captureFailed = true;
            }
            else
            {
                particleSystem = std::move(loaded);
                engine->Clear();
                engine->OnParticleSystemChanged(-1);
                engine->ReloadTextures();
                // Loading only populates the effect *definition*; nothing
                // emits until a live instance is spawned (the editor does
                // this via the SpawnerDriver, default Auto+disabled). Fire
                // one manual burst at the origin with no lifetime cap so
                // the system's emitters keep filling for the whole capture.
                if (spawnerDriver)
                {
                    SpawnerConfig cfg;
                    cfg.mode           = SpawnerConfig::Mode::Manual;
                    cfg.burstSize      = 1;
                    cfg.position       = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
                    cfg.maxLifetimeSec = 0.0f;  // no cap — emit through the capture
                    spawnerDriver->SetConfig(cfg);
                    spawnerDriver->Trigger(particleSystem.get(), engine.get());
                }
                // Apply the requested skydome slot so a --capture run can render
                // (and verify) particles over a background skydome. Slot 0
                // (default) leaves the solid-colour background untouched.
                if (m_captureSkydomeSlot > 0)
                {
                    const bool sok = engine->SetSkydomeSlot(m_captureSkydomeSlot);
                    Log("[capture] skydome slot %d -> %s\n",
                        m_captureSkydomeSlot, sok ? "ok" : "FAILED");
                }
                Log("[capture] loaded %ls; spawned instance; rendering %d frames -> %ls\n",
                    m_captureAlo.c_str(), m_captureFrames, m_capturePng.c_str());
            }
        }
    }

    // main loop: switched from blocking GetMessage to PeekMessage
    // idle-render. The blocking variant produces no continuous WM_PAINT
    // events, so the per-frame spawner tick + engine render had no driver.
    // Now: drain queued messages, then render once on idle, loop until
    // WM_QUIT. Mirrors legacy src/main.cpp:8023.
    //
    // No IsDialogMessage routing — the host has no modeless Win32
    // dialogs; tool panels live in React under WebView2 (which has its
    // own input routing and doesn't need TranslateAccelerator either).
    MSG m = {};
    bool quit = false;
    int  capturedFrames = 0;
    while (!quit)
    {
        while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&m);
            DispatchMessage(&m);
            if (m.message == WM_QUIT)
            {
                quit = true;
            }
        }
        if (quit) break;
        // rendering-fidelity] Load/capture failure → bail to cleanup
        // without rendering (exit code set below).
        if (captureFailed) break;

        // Idle: render one frame. Cheap enough to always run (Engine has
        // its own paused / IsPreviewPaused gates that skip the simulation
        // step when set; render still presents to keep the surface valid).
        if (engine)
        {
            RenderD3D9();

            if (captureMode)
            {
                // Pace the sim with a fixed ~16 ms wall-clock step so
                // RenderD3D9's real-time dt advances particles a useful
                // amount per frame (the uncapped pump would otherwise run
                // dozens of frames in a few ms, leaving particles bunched
                // at the spawn point and never overlapping — which is
                // exactly the additive-over-smoke case we need to see).
                Sleep(16);
                if (++capturedFrames >= m_captureFrames)
                {
                    // (1) engine RT — the engine's own pre-composite pixels.
                    const bool ok = alphaCompositor &&
                                    alphaCompositor->CaptureSnapshotToFile(m_capturePng);
                    // (2) composite — the final DWM/DComp-composited window
                    // (what the user sees). Derive "<name>-composite.<ext>".
                    std::wstring compPath = m_capturePng;
                    const size_t dot = compPath.find_last_of(L'.');
                    if (dot != std::wstring::npos) compPath.insert(dot, L"-composite");
                    else                            compPath += L"-composite.png";
                    const bool okc = CaptureWindowToPng(hMain, compPath);
                    Log("[capture] frame %d: engine-RT %ls -> %s; composite %ls -> %s\n",
                        capturedFrames, m_capturePng.c_str(), ok ? "ok" : "FAILED",
                        compPath.c_str(), okc ? "ok" : "FAILED");
                    if (!ok) captureFailed = true;
                    quit = true;
                }
            }
        }
        else
        {
            // No engine yet — yield rather than spin so WebView2 / WM_TIMER
            // get pump cycles. WM_TIMER will arrive in the PeekMessage
            // drain above (stats timer is 250ms).
            WaitMessage();
        }
    }

    g_self = nullptr;
    // B1.3.1.1: matching shutdown for the GdiplusStartup above. Safe
    // here because the message pump has drained: no dispatcher
    // handlers (CaptureSnapshotPng et al) can run after WM_QUIT.
    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    CloseLog();
    // rendering-fidelity] In --capture mode we break the loop via
    // the `quit` flag (not PostQuitMessage), so m.wParam is stale; return
    // an explicit 0/2 so a script can detect a bad load / failed write.
    if (captureMode) return captureFailed ? 2 : 0;
    return static_cast<int>(m.wParam);
}

// -----------------------------------------------------------------------------
// HostWindow public surface
// -----------------------------------------------------------------------------

HostWindow::HostWindow(HINSTANCE hInstance,
                       ITextureManager& textureManager,
                       IShaderManager&  shaderManager,
                       IFileManager&    fileManager,
                       const std::vector<std::wstring>& gameRoots,
                       bool useDevUi,
                       bool useTestHost,
                       const std::wstring& captureAlo,
                       const std::wstring& capturePng,
                       int captureFrames,
                       int captureSkydome)
    : m_impl(new HostWindowImpl(hInstance, textureManager, shaderManager, fileManager,
                                gameRoots, useDevUi, useTestHost,
                                captureAlo, capturePng, captureFrames, captureSkydome))
{
}

HostWindow::~HostWindow()
{
    delete static_cast<HostWindowImpl*>(m_impl);
    m_impl = nullptr;
}

int HostWindow::Run(int nCmdShow)
{
    return static_cast<HostWindowImpl*>(m_impl)->Run(nCmdShow);
}

// -----------------------------------------------------------------------------
// host::Run entry point
// -----------------------------------------------------------------------------

int Run(HINSTANCE hInstance,
        int nCmdShow,
        ITextureManager& textureManager,
        IShaderManager&  shaderManager,
        IFileManager&    fileManager,
        const std::vector<std::wstring>& gameRoots,
        bool useDevUi,
        bool useTestHost,
        const std::wstring& captureAlo,
        const std::wstring& capturePng,
        int captureFrames,
        int captureSkydome)
{
    HostWindow host(hInstance, textureManager, shaderManager, fileManager,
                    gameRoots, useDevUi, useTestHost,
                    captureAlo, capturePng, captureFrames, captureSkydome);
    return host.Run(nCmdShow);
}

} // namespace host
