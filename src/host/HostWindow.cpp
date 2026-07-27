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
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM for mouse forwarding
#include <shellapi.h>
#include <shlobj.h>
#include <wrl.h>
#include <wrl/implements.h>
#include <d3d9.h>
#include <winhttp.h>
#include <shlwapi.h>  // SHCreateMemStream for WebResourceRequested response
#include <dwmapi.h>   // title-bar dark-mode (DWMWA_USE_IMMERSIVE_DARK_MODE)
#include <psapi.h>
#include <timeapi.h>  // [resize-perf] timeBeginPeriod/timeEndPeriod for the paced pump
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")   // [resize-perf] timeBeginPeriod

// See BridgeDispatcher.cpp for the runtime (theme-toggle) title-bar sync;
// this is the startup default. Guarded for older SDKs (value 20 on modern
// Windows, which the editor targets via WebView2 + DComp).
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#include <algorithm>   // [resize-perf] per-kind bridge-probe sort
#include <atomic>
#include <cstdarg>
#include <cmath>       // roundf for drag-time grid/angle snap
#include <cstdio>
#include <cstdlib>     // C runtime helpers
#include <cstring>
#include <cwctype>
#include <share.h>     // _SH_DENYNO for _wfsopen sharing
#include <filesystem>
#include <fstream>     // --record cursor-sidecar.json verify artifact
#include <map>         // [resize-perf] per-kind bridge-probe tally
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "HostWindow.h"
#include "Run.h"
#include "WindowCapture.h"  // host::CaptureWindowToPng (factored out for --capture/--snap-window)
#include "StringConv.h"     // host::Utf8ToWide / WideToUtf8 (consolidated, DRY audit cpp-host-0)
#include "CacheBust.h"   // app.local index.html cache-bust query (workaround)
#include "PerfTrace.h"
#include "WebViewModalPolicy.h"

#include "AcceleratorBridge.h"
#include "AlphaCompositor.h"
#include "Compositor.h"
#include "InputDispatcher.h"

#include <objbase.h>
#include <gdiplus.h>
#include "BridgeDispatcher.h"
#include "HostBridgeProxy.h"
#include "HostMessages.h"        // WM_APP_QUIT_CONFIRMED
#include "../CloseDecision.h"    // ShouldVetoClose
#include "LayoutBroker.h"

#include "../engine.h"
#include "../ManipReadout.h"   // pure projection/label helpers for the readout pill
#include "../PlaneHandle.h"
#include "../managers.h"
#include "../ModManager.h"
#include "../MouseCursor.h"
#include "../UI/TexturePalette.h"   // Store::SetEphemeral (automation palette isolation)
#include "../ParticleSystem.h"
#include "../ParticleSystemIO.h"
#include "../ParticleSystemInstance.h"
#include "../SpawnerDriver.h"
#include "../UndoStack.h"
#include "../Autosave.h"  // two-tier autosave timers + clean-exit cleanup
#include "DriveRunner.h"   // --drive: scripted non-CDP composite capture
#include "ClipRunner.h"    // --record: deterministic clip recording (PNG sequence)
#include "RecordOutputSafety.h"  // --record: refuse to remove_all a non-output dir (an-audit-finding)
#include "CaptureRunner.h" // --capture/--capture-ref: one-shot render + PNG (Phase C split)
#include "HostRunUtil.h"   // PerfQpcNow/PerfQpcFreq/QpcMs/DeriveSibling (shared with the runners)
#include "AsyncFrameEncoder.h"   // --record Branch B: background PNG encode (tasks/todo.md §3)

using namespace Microsoft::WRL;

namespace host {

// --drive: read a (small) UTF-8/ASCII JSON script file into a std::string.
// Returns empty on any error; DriveRunner::Init reports a bad/empty script.
static std::string ReadFileUtf8(const std::wstring& path)
{
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) { out.resize(static_cast<size_t>(n)); fread(&out[0], 1, static_cast<size_t>(n), f); }
    fclose(f);
    return out;
}

namespace {

constexpr wchar_t kHostWindowClassName[]     = L"AloHostMain";
constexpr wchar_t kHostViewportClassName[]   = L"AloHostViewport";
constexpr int     kInitialWidth              = 1280;
constexpr int     kInitialHeight             = 800;
constexpr wchar_t kVirtualHostName[]         = L"app.local";
constexpr INTERNET_PORT kDevServerPort       = 5174;
constexpr UINT_PTR    kStatsTimerId          = 0x100;  // 4 Hz stats broadcast
// [resize-perf] one-shot safety net: re-armed on every
// size tick while in sizemove; fires 150 ms after the ticks stop and
// re-resets ONLY if a per-tick cheap reset failed mid-gesture (normally
// a no-op — see LayoutBroker::SettleDeferredReset). Covers a lost
// WM_EXITSIZEMOVE too.
constexpr UINT_PTR    kResizeSettleTimerId   = 0x101;
constexpr UINT        kResizeSettleDelayMs   = 150;

// WebView2 origin allow-list. The host must trust only the
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
// ported from the original src/main.cpp `FPSMeasurer` (since removed), but
// later work swapped GetTickCount() for QueryPerformanceCounter so the math
// stays meaningful in the uncapped (no-vsync) UpdateLayeredWindow
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

// [PERF] per-stage frame timing. The QPC helpers (PerfQpcFreq/PerfQpcNow)
// moved to HostRunUtil.h (shared with CaptureRunner). A tiny per-stage
// accumulator, always-on (QPC is ~20 ns/call, ~6 calls/frame), emitted to
// host.log at 1 Hz under the [PERF] prefix to localise which
// composition-path stage's cost scales with window area.
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
    unsigned over16 = 0;
    unsigned over33 = 0;
    unsigned over50 = 0;
    void   add(double us) {
        sumUs += us;
        if (us > maxUs) maxUs = us;
        if (us > 16666.7) ++over16;
        if (us > 33333.3) ++over33;
        if (us > 50000.0) ++over50;
        ++n;
    }
    double avg() const    { return n ? sumUs / n : 0.0; }
    void   reset()        { sumUs = 0.0; maxUs = 0.0; n = 0; over16 = over33 = over50 = 0; }
};

struct ProcessMemorySnapshot
{
    SIZE_T workingSetBytes = 0;
    SIZE_T privateUsageBytes = 0;
};

static ProcessMemorySnapshot GetProcessMemorySnapshot()
{
    ProcessMemorySnapshot out;
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                             sizeof(pmc)))
    {
        out.workingSetBytes = pmc.WorkingSetSize;
        out.privateUsageBytes = pmc.PrivateUsage;
    }
    return out;
}

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

// Absolute path of the release zip's bundled Evergreen bootstrapper, or empty
// if it is not beside the exe (a dev-tree run, or a hand-assembled copy).
//
// GetModuleFileNameW is called in a grow-until-it-fits loop rather than with a
// fixed MAX_PATH buffer: on a long extraction path the fixed form TRUNCATES and
// — on older Windows — does not null-terminate, so building a path from it
// would silently point somewhere else. It reports the truncation as
// ERROR_INSUFFICIENT_BUFFER with the return value equal to the buffer size.
static std::wstring BundledWebView2SetupPath()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return std::wstring();                       // genuinely failed
        if (n < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        if (buf.size() >= 32768) return std::wstring();          // past the Win32 ceiling
        buf.resize(buf.size() * 2);
    }
    std::filesystem::path p =
        std::filesystem::path(buf.data()).parent_path() / L"MicrosoftEdgeWebview2Setup.exe";
    return (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) ? p.wstring()
                                                                     : std::wstring();
}

// The runtime is missing (or unusable). Offer to fix it, in plain language.
//
// Release zips bundle Microsoft's bootstrapper precisely so this is a one-click
// recovery rather than "go install something else and come back" — the same
// reason d3dx9_43.dll is vendored. When it is not present (dev tree) fall back
// to opening the download page.
//
// Callers MUST gate on IsFullyInteractive(): a headless capture/record/drive run
// has nobody to answer a modal and would hang on it.
static void OfferWebView2Install(HWND owner, const wchar_t* detail)
{
    const std::wstring setup = BundledWebView2SetupPath();

    std::wstring msg =
        L"The Particle Editor needs the Microsoft Edge WebView2 runtime, "
        L"which this PC does not have yet.\n\n";
    msg += setup.empty()
        ? L"Install it from https://aka.ms/webview2, then start the editor again.\n\n"
          L"Click OK to open that page, or Cancel to exit."
        : L"Install it now? The installer is included beside the editor; it needs "
          L"an internet connection and takes about a minute. Start the editor "
          L"again once it finishes.";
    if (detail && *detail) { msg += L"\n\n"; msg += detail; }

    if (setup.empty())
    {
        if (MessageBoxW(owner, msg.c_str(), L"WebView2 Runtime Required",
                        MB_OKCANCEL | MB_ICONERROR) == IDOK)
            ShellExecuteW(owner, L"open", L"https://aka.ms/webview2",
                          nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (MessageBoxW(owner, msg.c_str(), L"WebView2 Runtime Required",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) == IDYES)
    {
        // Fire-and-forget: the bootstrapper elevates and runs its own UI, and
        // the editor cannot continue in this process either way — the runtime
        // is picked up on the next launch.
        ShellExecuteW(owner, L"open", setup.c_str(), L"/silent /install",
                      nullptr, SW_SHOWNORMAL);
    }
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
//
// `isolated` = headless --capture mode: a capture instance must NOT share the
// daily-driver editor's WebView2 profile. The runtime LOCKS the user-data
// folder, so a capture launched alongside the live editor fails env-creation
// ("unable to open file") and pops a modal on the user's screen. Give capture
// runs a throwaway, per-process profile so they never contend with the editor.
std::wstring ComputeUserDataFolder(bool isolated = false)
{
    wchar_t pidSuffix[32] = {};
    if (isolated) swprintf(pidSuffix, 32, L"-capture-%lu", GetCurrentProcessId());

    PWSTR localAppData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))
        && localAppData)
    {
        std::wstring folder = localAppData;
        CoTaskMemFree(localAppData);
        folder += L"\\AloParticleEditor\\WebView2";
        folder += pidSuffix;
        SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr); // best-effort
        return folder;
    }
    // Fallback to temp.
    wchar_t tempDir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempDir);
    return std::wstring(tempDir) + L"AloParticleEditor_WebView2" + pidSuffix;
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

std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf)
{
    if (dir.empty()) return leaf ? std::wstring(leaf) : std::wstring();
    std::filesystem::path p(dir);
    p /= leaf;
    return p.wstring();
}

std::wstring LowerAscii(std::wstring value)
{
    for (wchar_t& ch : value) ch = static_cast<wchar_t>(std::towlower(ch));
    return value;
}

bool IsKnownPerfTraceMode(const std::wstring& mode)
{
    const std::wstring m = LowerAscii(mode);
    return m.empty() || m == L"off" || m == L"null" || m == L"file";
}

std::wstring AppendQueryParam(const std::wstring& url, const wchar_t* param)
{
    if (!param || !param[0]) return url;
    return url + (url.find(L'?') == std::wstring::npos ? L"?" : L"&") + param;
}

// UTF-8 ↔ UTF-16 conversions now live in StringConv.h (host::Utf8ToWide /
// WideToUtf8), shared with BridgeDispatcher + HostBridgeProxy (DRY audit cpp-host-0).

LRESULT CALLBACK HostMainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HostViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Custom message posted when composition setup fails AFTER the async
// CreateCoreWebView2CompositionController dispatch (OnCompositionController-
// Ready). wParam carries the failure HRESULT. Composition is a hard
// requirement — there is no HWND fallback — so the handler surfaces a clear
// fatal error and exits (FailFatalComposition). PostMessage'd rather than
// acting inline so the WebView2 callback stack unwinds first.
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
    // needed by the WebResourceRequested handler to
    // construct the response stream via env->CreateWebResourceResponse.
    ComPtr<ICoreWebView2Environment> webEnv;
    EventRegistrationToken          accelKeyTok = {};
    EventRegistrationToken          docTitleTok = {};
    // Stash the WebMessageReceived registration token so
    // WM_DESTROY can explicitly remove the handler before tearing down
    // webView. Pre-fix the token was a local in InitWebView2 and the
    // handler stayed subscribed (the lambda captures `this`) — masked
    // today by webView.Reset(), but the explicit-unsubscribe pattern
    // mirrors accelKeyTok above and is materially safer.
    EventRegistrationToken          webMessageTok = {};
    // Navigation / new-window / permission policy tokens.
    // Registered alongside webMessageTok in InitWebView2 and removed in
    // WM_DESTROY (mirroring the G5 webMessageTok lifecycle). The handlers
    // enforce the IsApprovedWebViewOrigin allow-list (cancel off-origin
    // top-level navigation), deny all popups, and deny every permission
    // request — defence-in-depth against a redirected/compromised renderer.
    EventRegistrationToken          navStartingTok = {};
    EventRegistrationToken          newWindowTok   = {};
    EventRegistrationToken          permissionTok  = {};
    // --capture diagnosability: logs when app.local finishes loading so a
    // ui-ready timeout is attributable to "navigation never finished" vs
    // "loaded but never signaled". Registered in FinishWebView2ControllerSetup,
    // removed in WM_DESTROY (same lifecycle as the tokens above).
    EventRegistrationToken          navCompletedTok = {};
    // TME_LEAVE arming state. WebView2 needs a
    // COREWEBVIEW2_MOUSE_EVENT_KIND_MOUSE_LEAVE input when the pointer
    // exits the host HWND so CSS :hover / cursor state clears. Re-arm
    // on each WM_MOUSEMOVE after the leave fires.
    bool                            m_mouseTracked = false;
    // Owned class background brush. Created in Run(),
    // released in WM_DESTROY.
    HBRUSH                          m_classBrush = nullptr;

    ITextureManager& textureManager;
    IShaderManager&  shaderManager;
    IFileManager&    fileManager;
    std::unique_ptr<Engine> engine;

    // layered-window alpha compositor. Constructed after the
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
    // through the new-UI bridge surface (emitter work), so the
    // stack stays empty for now and `undo/perform` resolves with
    // `applied: false`. The plumbing exists so later work wraps the
    // engine setter handlers in Capture() without re-touching this file.
    UndoStack                          undoStack;

    LayoutBroker                       layout;
    AcceleratorBridge                  accelerator;
    std::unique_ptr<BridgeDispatcher>  dispatcher;
    FPSMeasurer                        fpsMeasurer;

    // [PERF] per-stage frame-timing accumulators. Reset each 1 Hz
    // emit in RenderD3D9. Always-on.
    PerfStage          perfUpdate, perfRender, perfWait, perfComposite, perfFrame;
    // [PERF2] round-2 — engine Render() per-pass sub-timing (us).
    PerfStage          perfRScene, perfRBloom, perfRDistort, perfRCompose, perfRPresent;
    unsigned long long perfWaitSpinsSum = 0;
    unsigned           perfWaitSpinsMax = 0;
    DWORD              perfLastEmitTick = 0;

    // [resize-perf] probes.
    // Always-on 1 Hz aggregates, same convention as [PERF] above.
    // perfWmpos times the per-tick PredictAndApply+RenderD3D9 chain in
    // WM_WINDOWPOSCHANGED (the suspected reset storm); the reset counter
    // baseline turns Engine's monotonic ResetPerf.count into resets/sec.
    // perfSceneRectMsgs counts layout/scene-rect arrivals in OnWebMessage
    // (the RO→bridge stream rate during splitter drags).
    PerfStage perfWmpos;
    unsigned  perfWmposResetBase = 0;
    DWORD     perfWmposLastEmit  = 0;
    unsigned  perfWebMsgs        = 0;
    DWORD     perfMsgLastEmit    = 0;
    // [resize-perf] per-kind tally for the bridge probe (cleared each
    // 1 Hz emit). Keyed by the wire `kind` string.
    std::map<std::wstring, unsigned> perfMsgKinds;

    // [resize-perf] true between WM_ENTERSIZEMOVE and
    // WM_EXITSIZEMOVE — gates the main-window WM_ERASEBKGND
    // suppression and arms the settle-safety quiescence timer.
    bool      m_inSizeMove       = false;

    // mod state shared with React. ModManager constructed in
    // the impl ctor (DiscoverMods + RestoreLastLayerStack run before
    // any UI shows); SetEngine called in WM_CREATE once the Engine
    // exists. Passed to BridgeDispatcher via SetModManager.
    std::unique_ptr<::ModManager>      modManager;

    // render loop bookkeeping. m_lastRenderTime drives dt for the
    // per-frame SpawnerDriver::Tick — matches the legacy
    // `g_spawnerLastFrameTime` flow in the legacy main.cpp. First frame
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

    // viewport interaction (camera controls). Mirror of the legacy
    // main.cpp drag-state. On WM_LBUTTONDOWN /
    // WM_RBUTTONDOWN we snapshot the camera + cursor XY, then
    // WM_MOUSEMOVE deltas are applied relative to the snapshot
    // (matches legacy "drag relative to start" feel — releasing and
    // re-pressing resets the reference frame). NONE means no drag in
    // progress; the wheel handler only fires when dragMode == NONE.
    // OBJECT_Z: cursor-bound preview is being dragged for placement.
    // Only Z (height) tracks the drag delta; X/Y stay frozen at the
    // click position. WM_LBUTTONUP detaches the preview (place it).
    // Matches the legacy main.cpp.
    // MANIPULATE: a manipulator handle (translate arrow or rotate
    // ring) was grabbed; LMB drag moves/rotates the object (wins over camera orbit
    // only when a handle is actually under the cursor at press).
    enum class DragMode { NONE, MOVE, ROTATE, ZOOM, OBJECT_Z, MANIPULATE };
    DragMode        m_dragMode      = DragMode::NONE;
    Engine::Camera  m_dragStartCam  = {};
    int             m_dragStartX    = 0;
    int             m_dragStartY    = 0;
    // Manipulator drag state: the grabbed handle (kind + axis),
    // the transform snapshot at grab, and the no-jump anchors. TRANSLATE 
    // accumulates precision-scaled per-move axis-param deltas: each WM_MOUSEMOVE adds
    // (tNow - m_manipPrevT) * factor to m_manipAccumT (factor = 0.2 while Shift held,
    // else 1.0) and applies newPos = startPos + axis*m_manipAccumT. m_manipGrabT0 is
    // the axis param at press (seeds m_manipPrevT so the first move's delta is 0 -> no
    // jump). With factor==1 throughout, m_manipAccumT telescopes to (tNow - grabT0),
    // matching the old absolute-from-grab formula; a mid-drag Shift toggle only rescales
    // subsequent deltas (no jump, since m_manipPrevT tracks the raw param). ROTATE
    // accumulates wrapped, precision-scaled per-move ring angle deltas
    // (m_manipGrabAngle/Prev/Accum) onto the snapshot rotation.
    Engine::ManipHandle::Kind m_manipKind = Engine::ManipHandle::NONE;
    int             m_manipAxis        = -1;
    D3DXVECTOR3     m_manipStartPos    = D3DXVECTOR3(0, 0, 0);
    D3DXVECTOR3     m_manipStartRot    = D3DXVECTOR3(0, 0, 0);
    float           m_manipGrabT0      = 0.0f;
    float           m_manipPrevT       = 0.0f;   // translate accumulate-per-move: last raw axis param
    float           m_manipAccumT      = 0.0f;   // accumulated (precision-scaled) translate offset from grab
    float           m_manipPrevU       = 0.0f;   // plane drag: last raw in-plane U
    float           m_manipPrevV       = 0.0f;   //                     last raw in-plane V
    float           m_manipAccumU      = 0.0f;   //   accumulated (precision-scaled) U offset from grab
    float           m_manipAccumV      = 0.0f;   //   accumulated V offset from grab
    float           m_manipGrabAngle   = 0.0f;   // ring angle at grab (rad)
    float           m_manipPrevAngle   = 0.0f;   // previous-move ring angle (rad)
    float           m_manipAccumAngle  = 0.0f;   // accumulated rotation (rad)
    // Per-gesture latch: false until the FIRST per-move mutation
    // of a manipulator drag pushes its (one) pre-mutation undo point. A grab
    // that never moves the object captures nothing — no phantom undo step.
    bool            m_manipUndoCaptured = false;
    // ~30 Hz throttle for the per-move engine/state/changed emit (the snapshot is
    // heavy; the gizmo render still moves every frame via SetReferenceObjectTransform).
    DWORD           m_lastManipEmitTick = 0;

    // Readout pill scratch: each MANIPULATE branch fills these post-snap;
    // the throttle gate projects the gizmo origin and emits one event.
    std::string m_readoutKind;                 // "translate" | "plane" | "rotate"
    std::string m_readoutLabels[2];            // axis / euler names
    float       m_readoutValues[2] = {0,0};    // absolute values
    int         m_readoutN = 0;                // 1 or 2
    int         m_readoutDecimals = 1;         // 1 for units, 0 for degrees

    // Re-validate the cursor-bound Shift-preview borrow before ANY use. See the
    // m_attachedParticleSystem note below: Engine::Clear() frees the pointee
    // behind our back on three paths that never null this slot. Returns nullptr
    // and self-heals the slot when the borrow has gone stale, so a caller can
    // neither act on a freed instance nor be blocked forever by a non-null
    // pointer that will never clear on its own. Factored out for the same
    // reason as ResetManipDragState below -- so a new use site can't forget it.
    ParticleSystemInstance* LiveAttachedSystem()
    {
        if (m_attachedParticleSystem == nullptr || !engine) return nullptr;
        if (!engine->HasInstance(m_attachedParticleSystem))
        {
            m_attachedParticleSystem = nullptr;   // stale borrow: drop it
            return nullptr;
        }
        return m_attachedParticleSystem;
    }

    // The invariant tail every MANIPULATE drag-end shares: drop the grabbed handle, zero the
    // accumulators, and clear the engine's active-drag (guide/sweep/dim) state. Per-site Commit /
    // ReleaseCapture / m_dragMode handling stays at the call site -- only this shared tail is factored
    // out so a new end-site can't forget the active-drag clear (the bug WM_KILLFOCUS originally had).
    void ResetManipDragState()
    {
        m_manipAxis = -1;
        m_manipKind = Engine::ManipHandle::NONE;
        m_manipAccumT = 0.0f;
        m_manipAccumAngle = 0.0f;
        m_manipAccumU = 0.0f;
        m_manipAccumV = 0.0f;
        m_manipUndoCaptured = false;   // next grab starts a fresh gesture
        if (engine) engine->SetManipulatorActiveDrag(Engine::ManipHandle(), 0.0f, 0.0f);
        // hide the readout pill (ResetManipDragState is called from the 4
        // capture-drag-end sites: LBUTTONUP, RBUTTONDOWN, CAPTURECHANGED, KILLFOCUS).
        if (dispatcher) dispatcher->EmitManipulatorDrag({ {"active", false} });
    }

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
    // It is a RAW BORROW of an Engine::m_instances entry, and Engine::Clear()
    // frees every instance without telling us. file/new + file/open + recover
    // + undo-apply null this slot themselves (BridgeDispatch_File.cpp,
    // BridgeDispatcher.cpp), but three paths reach Clear() without doing so:
    // engine/action/clear, the SetEstimatedLoad overload hard-guard, and a
    // gate-refused SpawnParticleSystem. Read it through LiveAttachedSystem()
    // rather than directly — a stale pointer otherwise blocks every future
    // Shift-spawn (the non-null precondition never clears) and puts LMB-down
    // into a placement drag for an instance that no longer exists.
    //
    // m_lastCursorX/Y: cache of the most recent (x,y) seen by
    // WM_MOUSEMOVE. Used as the spawn coords on WM_KEYDOWN VK_SHIFT
    // because WM_KEYDOWN's lParam is NOT cursor coords (a legacy
    // main.cpp bug passed garbage). Fallback if the cache is
    // stale: GetCursorPos + ScreenToClient.
    MouseCursor             m_mouseCursor;
    ParticleSystemInstance* m_attachedParticleSystem = nullptr;
    int                     m_lastCursorX = 0;
    int                     m_lastCursorY = 0;
    // last GetTickCount() at which we pushed a
    // `cursor/position-3d` event. Throttled to ~30 Hz so the
    // WebView2 message channel isn't saturated by WM_MOUSEMOVE
    // (which fires per-pixel). The legacy status bar updates per
    // WM_MOUSEMOVE since SendMessage is free in-process; over the
    // bridge a 33 ms minimum interval is a good compromise.
    DWORD                   m_lastCursorEmitTick = 0;

    // viewport/input bridge surface owner. Constructed
    // alongside the AlphaCompositor in WM_CREATE; holds a raw HWND for the
    // viewport popup it PostMessages camera/keyboard input to. BridgeDispatcher
    // gets a borrow via SetInputDispatcher.
    std::unique_ptr<host::InputDispatcher> m_inputDispatcher;

    // WebView2 composition hosting. The host always
    // takes the CreateCoreWebView2CompositionController path, and a
    // host::Compositor owns the DirectComposition visual tree WebView2 plugs
    // into via put_RootVisualTarget.
    //
    // m_compositionController is the controller returned by
    // CreateCoreWebView2CompositionController. We also QI it to
    // ICoreWebView2Controller and store in `webController` so every existing
    // wire-up (put_Bounds, AcceleratorKeyPressed, etc.) works unchanged. Kept
    // here so WM_DESTROY can release the composition-specific reference before
    // releasing the base controller (the teardown ordering matters per the
    // spike's Shutdown sequence in dxgi_spike.cpp:783).
    std::unique_ptr<host::Compositor>          m_compositor;
    ComPtr<ICoreWebView2CompositionController> m_compositionController;
    // Frameless title bar: QI of the composition controller for
    // GetNonClientRegionAtPoint (WM_NCHITTEST caption drag). Null on an older
    // WebView2 Runtime → HTCAPTION fallback. m_ncRegionEnabled tracks whether
    // put_IsNonClientRegionSupportEnabled(TRUE) succeeded.
    ComPtr<ICoreWebView2CompositionController4> m_compositionController4;
    bool m_ncRegionEnabled = false;
    // Frameless title bar: emit window/state only on an ACTUAL maximized↔restored
    // CHANGE (WM_SIZE sends SIZE_RESTORED on every resize tick — a raw emit would
    // flood the bridge), and only once the emit can reach the web. A
    // launch-maximized state fires its first WM_SIZE before React/m_emit exists,
    // so EmitWindowState no-ops there; app/ready replays it. -1 = not yet sent.
    int m_lastMaximizedSent = -1;
    void EmitWindowStateIfChanged()
    {
        if (!dispatcher) return;
        const int cur = IsZoomed(hMain) ? 1 : 0;
        if (cur == m_lastMaximizedSent) return;
        if (dispatcher->EmitWindowState(cur != 0)) m_lastMaximizedSent = cur;
    }

    // Cursor sync. Under HWND hosting,
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
    // --capture mode: load m_captureAlo,
    // render m_captureFrames frames, write engine RT to m_capturePng,
    // then quit. Both paths empty = normal interactive run.
    std::wstring m_captureAlo;
    // --capture-ref <objectName>: render a game reference object (with its
    // shadow) headlessly instead of a particle system. Mutually exclusive
    // with m_captureAlo in practice; the capture branch checks it first.
    std::wstring m_captureRef;
    std::wstring m_capturePng;
    int          m_captureFrames = 60;
    // --skydome <slot>: apply this skydome slot in --capture mode before
    // rendering (0 = Off / solid colour, the default).
    int          m_captureSkydomeSlot = 0;
    // [world-lit] --ambient / --sun / --sun-intensity capture lighting drivers.
    bool         m_captureHasAmbient = false; float m_captureAmbient[3] = {0,0,0};
    bool         m_captureHasSun = false;     float m_captureSun[3] = {0,0,0};
    bool         m_captureHasSunI = false;    float m_captureSunIntensity = 1.0f;
    // --capture: set true when the React app posts its `app/ready` first-paint
    // signal (OnWebMessage). The capture loop gates the composite-window
    // screenshot on this. Written in OnWebMessage and read in the capture wait
    // loop — BOTH on the STA UI pump thread (WebView2 marshals
    // WebMessageReceived to this thread's message pump), so a plain non-atomic
    // bool is correct: no atomic/volatile needed.
    bool         m_uiReady = false;
    // --drive <script.json>: scripted non-CDP composite capture. m_ephemeral
    // (true in drive mode) suppresses ALL persistence (settings/MRU/mod-layer/
    // autosave) and isolates the WebView2 profile + log per-PID so a --drive
    // run never perturbs a concurrently-running daily-driver editor.
    std::wstring m_driveScriptPath;
    bool         m_ephemeral = false;
    // --record <timeline.json>: deterministic clip recording. m_recordMode routes
    // the record pump branch; m_automationMode (drive OR record) gates persistence
    // — keep them separate so a --record run takes the persistence isolation but
    // enters the RECORD branch, never the drive branch.
    std::wstring m_recordScriptPath;
    bool         m_recordMode = false;
    bool         m_automationMode = false;
    // True only when a human is present to dismiss a blocking modal — i.e. NOT in
    // any headless mode (capture / drive-or-record / test-host). Every fatal or
    // preflight MessageBoxW is gated on this so a headless run never hangs on a
    // dialog nobody can click (the log line + non-zero exit still carry the error).
    bool IsFullyInteractive() const
    {
        return IsFullyInteractiveSession(
            !m_captureAlo.empty() || !m_captureRef.empty(), m_automationMode, useTestHost);
    }
    int          m_recordTimelineFps = 0;    // latched at timeline-Init success; locks the
                                             // stats-tick FPS readout to the clip's virtual rate
    int          m_recordFrame = 0;          // current emitted frame (echoed in the ui/cursor `frame`)
    int          m_lastAckedFrame = -1;      // set by OnWebMessage on ui/frame-acked
    // Extended ack payload for the SEMANTIC-targeting cursor path: when a
    // ui/frame-acked carries a `cursor` object (web resolved the selectors),
    // OnWebMessage stashes the device-px cursor {x,y,vis,press} + the per-target
    // `resolved` array here, keyed by frame. ClipRunner reads it back via the
    // AckDataFn hook to fail-loud on an unresolved target + build the sidecar.
    int            m_lastAckCursorFrame = -1;
    nlohmann::json m_lastAckCursor;          // {x,y,vis,press}
    nlohmann::json m_lastAckResolved = nlohmann::json::array();  // [{ref,x,y,ok}]
    // [record-timing] Per-segment QPC accumulators for the record frame loop
    // (permanent Phase-0 instrumentation, tasks/todo.md §3). The four segments
    // are accumulated inside the ClipRunner hooks (dispatch/ack lambdas;
    // barrier/png split inside the capture lambda); the Tick call site pushes
    // one entry per frame. Buckets are exhaustive by construction:
    //   wall ≈ setup + Σframe + pump(between-tick loop overhead, incl. the
    //          unconditional pre-tick RenderD3D9)
    //   frame ≈ dispatch + ack + barrier + png + other(in-Tick, un-hooked)
    // The ack-wait loop's inner RenderD3D9 calls count as ACK time (they run
    // inside the ack hook), per the plan's segment definitions.
    struct RecordTiming
    {
        std::vector<double> dispatch, ack, barrier, png, frame;   // per-frame ms
        double curDispatch = 0, curAck = 0, curBarrier = 0, curPng = 0;
        double setupMs = 0.0;        // record-branch start -> first Tick
        bool   sawFirstTick = false;
        // [R1] adaptive-barrier accounting: total DwmFlush presents across
        // the run (avg = total/frames in the summary) and whether the
        // composition-timing probe ever failed (permanent fixed-3 fallback
        // must be VISIBLE, not silent).
        unsigned barrierFlushTotal   = 0;
        bool     barrierProbeFailed  = false;
    };
    RecordTiming m_recordTiming;
    // --record-timing-verbose: per-frame [record-timing] lines (default off —
    // 60 fps logging would perturb the measurement). Probed from the raw
    // command line rather than threaded through Run()/HostWindow ctor: a
    // log-verbosity toggle doesn't justify 4 files of signature churn, and
    // main.cpp's argv loop ignores unknown --flags harmlessly.
    bool m_recordTimingVerbose =
        wcsstr(GetCommandLineW(), L"--record-timing-verbose") != nullptr;
    // Headless capture path: message-ack (the web posts ui/frame-acked
    // synchronously via flushSync, no rAF-present dependency) + the same
    // PrintWindow/GrabWindowPixels grab the foreground path uses, run with the
    // record window OFFSCREEN (see --record-minimized) so the machine is free.
    // (#510 replaced the old CapturePreview + CPU-composite path.) Env gate
    // PE_RECORD_HEADLESS=1. Probed once; see the ack + capture hooks.
    bool m_recordHeadless = [] {
        wchar_t b[8] = {};
        return GetEnvironmentVariableW(L"PE_RECORD_HEADLESS", b, 8) > 0
            && b[0] != L'0';
    }();
    // --record-minimized: run the record window OUT OF SIGHT during a headless
    // render so the machine is free. Since #510 this moves the window OFFSCREEN
    // (a minimized window throttles DWM composition, which the window grab needs
    // — see the SetWindowPos in the record branch), NOT SW_MINIMIZE despite the
    // flag name. Only meaningful WITH PE_RECORD_HEADLESS. This is the mechanism
    // the machine-free acceptance test + build.mjs auto-minimize use.
    bool m_recordMinimized =
        wcsstr(GetCommandLineW(), L"--record-minimized") != nullptr;
    // Headless ack result for the LAST frame. In headless mode a missing ack
    // means the web's synchronous (flushSync) commit failed — the DOM did NOT
    // update, so capturing would publish a STALE frame. ClipRunner only aborts
    // TARGET clips on an ack timeout (literal clips continue), so the headless
    // capture hook checks this and fails the frame (exit 4) for BOTH — a
    // withheld ack is never silently captured. (pre-PR review finding 1.)
    bool m_headlessAckOk = true;
    // End-of-run summary (also emitted on the record watchdog so a timed-out
    // run still yields its measurement). p99/max reported beside p95 because
    // the 2000 ms ack deadline is fatal at p100, not p95 (plan §4 risk 1).
    // Defined in-class: the enclosing region around the file's later helpers
    // is an anonymous namespace, where a host::HostWindowImpl member can't be
    // defined (C2888). (std::min) parenthesized against windows.h's min macro.
    // [R3] Snapshot of the encoder's back-pressure stats, taken just before
    // the summary is logged (the encoder object lives in Run()'s locals).
    host::AsyncFrameEncoder::QueueStats m_recordEncoderStats = {};

    void LogRecordTimingSummary(double wallMs)
    {
        auto total = [](const std::vector<double>& v) {
            double s = 0.0; for (double x : v) s += x; return s; };
        auto pct = [](std::vector<double> v, double p) -> double {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            const size_t idx = (std::min)(v.size() - 1,
                static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
            return v[idx]; };
        auto line = [&](const char* name, const std::vector<double>& v) {
            const double t = total(v);
            Log("[record-timing] %-8s total=%8.0fms avg=%6.1fms p95=%6.1fms "
                "p99=%6.1fms max=%6.1fms\n",
                name, t, v.empty() ? 0.0 : t / static_cast<double>(v.size()),
                pct(v, 0.95), pct(v, 0.99),
                v.empty() ? 0.0 : *std::max_element(v.begin(), v.end()));
        };
        const RecordTiming& rt = m_recordTiming;
        const double frameTotal = total(rt.frame);
        const double segTotal = total(rt.dispatch) + total(rt.ack)
                              + total(rt.barrier)  + total(rt.png);
        Log("[record-timing] frames=%zu wall=%.0fms setup=%.0fms tick=%.0fms "
            "pump=%.0fms other-in-tick=%.0fms\n",
            rt.frame.size(), wallMs, rt.setupMs, frameTotal,
            wallMs - rt.setupMs - frameTotal, frameTotal - segTotal);
        line("dispatch", rt.dispatch);
        line("ack",      rt.ack);
        line("barrier",  rt.barrier);
        line("png",      rt.png);
        line("frame",    rt.frame);
        // [R1] Adaptive-barrier proof line: avg flushes/frame (fixed-3 was
        // the old behavior; ~2.0 = adaptive working) + loud fallback flag.
        if (!rt.barrier.empty() || rt.barrierFlushTotal > 0)
            Log("[record-timing] barrier-adaptive avg=%.2f flushes/frame%s\n",
                rt.frame.empty() ? 0.0
                    : static_cast<double>(rt.barrierFlushTotal)
                      / static_cast<double>(rt.frame.size()),
                rt.barrierProbeFailed ? "  (PROBE FAILED — fixed-3 fallback)" : "");
        // [R3] Encoder back-pressure: time the grab thread spent BLOCKED on
        // the 128 MB queue cap (silently folded into png/capture before) +
        // queue high-water marks — the second-worker/faster-encoder
        // decision datum.
        if (m_recordEncoderStats.waitCount > 0 || m_recordEncoderStats.depthHighWater > 0)
            Log("[record-timing] queue    blocked=%8.0fms x%u  hw=%zuMB depth=%zu\n",
                m_recordEncoderStats.waitMsTotal, m_recordEncoderStats.waitCount,
                m_recordEncoderStats.bytesHighWater / (1024 * 1024),
                m_recordEncoderStats.depthHighWater);
    }

    // [E5] Frame-pacing budget from the monitor the window actually sits on
    // (the old startup-only EnumDisplaySettings(nullptr) read the PRIMARY
    // display: it capped a 144 Hz secondary at 60 and over-drove a 60 Hz
    // secondary from a 144 Hz primary). Recomputed on WM_DISPLAYCHANGE and
    // when WM_WINDOWPOSCHANGED lands the window on a different monitor.
    HMONITOR m_pacingMonitor  = nullptr;
    DWORD    m_pacingHz       = 0;
    LONGLONG m_frameBudgetQpc = 0;
    void UpdatePacingBudget(HWND hwnd)
    {
        DWORD hz = 60;   // fallback, matches the old default
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        // 0 and 1 mean "hardware default" per EnumDisplaySettings docs —
        // treat anything below 30 as unknown and keep the 60 Hz fallback.
        if (mon && GetMonitorInfoW(mon, &mi)
            && EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
            && dm.dmDisplayFrequency >= 30)
        {
            hz = dm.dmDisplayFrequency;
        }
        m_pacingMonitor = mon;
        const bool changed = (hz != m_pacingHz);
        m_pacingHz = hz;
        m_frameBudgetQpc = PerfQpcFreq() > 0
            ? PerfQpcFreq() / static_cast<LONGLONG>(hz) : 0;
        if (changed)
            Log("[resize-perf] pump paced to %lu Hz (budget %.2f ms)\n",
                static_cast<unsigned long>(hz), 1000.0 / static_cast<double>(hz));
    }

    // [C4] Deferred-autosave latch (see the WM_TIMER autosave note): the
    // timer tick latches; the paced idle branch services right after a
    // presented frame when no mouse capture / size-move is active. force
    // = the busy-override (pending a full RECENT interval). The WM_DESTROY
    // path doesn't flush — it DELETES this session's autosaves on a clean
    // close, so a last-gasp write would be deleted one line later; the
    // dirty-close guard owns unsaved-changes safety at quit.
    bool               m_autosavePending      = false;
    Autosave::Tier     m_autosavePendingTier  = Autosave::Tier::Recent;
    unsigned long long m_autosavePendingSince = 0;
    void ServicePendingAutosave(bool force)
    {
        if (!m_autosavePending) return;
        if (!force && (GetCapture() != nullptr || m_inSizeMove)) return;
        m_autosavePending = false;
        const Autosave::Tier tier = m_autosavePendingTier;
        m_autosavePendingTier = Autosave::Tier::Recent;
        // Re-check the dirty gate at service time — a save between the
        // timer tick and this slot makes the write pointless.
        if (!dispatcher || !particleSystem || !dispatcher->GetDirty()) return;
        const LONGLONG t0 = PerfQpcNow();
        const bool wrote = Autosave::Write(
            *particleSystem, dispatcher->GetCurrentFilePath(), tier);
        // 1 write / ≥30 s — cheap to always log; the ms figure is the
        // follow-up datum for whether a worker-thread write is warranted.
        Log("[autosave] %s tier=%s in %.1f ms%s\n",
            wrote ? "wrote" : "write-FAILED",
            tier == Autosave::Tier::Recent ? "recent" : "stable",
            PerfUsSince(t0) / 1000.0,
            force ? " (busy-override)" : "");
    }

    // --drive bridge-selftest handshake: RunDriveSelftest arms the token +
    // nested-pumps; OnWebMessage completes it when the tokened result arrives
    // over the real page->host postMessage wire (single UI thread, no atomics).
    std::string  m_selftestToken;
    bool         m_selftestDone = false;
    bool         m_selftestOk = false;
    // --capture layout-determinism gate: OnWebMessage sets this; the
    // CaptureRunner (which owns the gate timer/warn state) reads it via Deps.
    bool         m_sceneRectSeen = false;
    std::unique_ptr<host::ClipRunner> m_clipRunner;
    std::wstring m_perfWebViewProfile;
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
                   int captureSkydome = 0,
                   const std::wstring& captureRef = L"",
                   bool hasAmbient = false, float ambR = 0.0f, float ambG = 0.0f, float ambB = 0.0f,
                   bool hasSun = false, float sunR = 0.0f, float sunG = 0.0f, float sunB = 0.0f,
                   bool hasSunI = false, float sunIntensity = 1.0f,
                   const std::wstring& driveScriptPath = L"",
                   const std::wstring& recordScriptPath = L"",
                   const std::wstring& perfWebViewProfile = L"")
        : hInstance(inst)
        , textureManager(tex)
        , shaderManager(shd)
        , fileManager(fil)
        , useDevUi(devUi)
        , useTestHost(testHost)
        , m_captureAlo(captureAlo)
        , m_captureRef(captureRef)
        , m_capturePng(capturePng)
        , m_captureFrames(captureFrames)
        , m_captureSkydomeSlot(captureSkydome)
        , m_captureHasAmbient(hasAmbient)
        , m_captureHasSun(hasSun)
        , m_captureHasSunI(hasSunI)
        , m_captureSunIntensity(sunIntensity)
        , m_driveScriptPath(driveScriptPath)
        , m_ephemeral(!driveScriptPath.empty())
        , m_recordScriptPath(recordScriptPath)
        , m_recordMode(!recordScriptPath.empty())
        , m_automationMode(!driveScriptPath.empty() || !recordScriptPath.empty())
        , m_perfWebViewProfile(perfWebViewProfile)
        , layout(nullptr)
        , accelerator()
        // Ephemeral = every headless mode — the same set IsFullyInteractive()
        // excludes (capture / drive-or-record / test-host). None of them may
        // rewrite the daily driver's persisted mod stack, and that includes the
        // startup write-back via RestoreLastLayerStack -> SetLayerStack: with a
        // mod folder temporarily unavailable (unmounted drive), a capture run
        // would otherwise ghost-drop those layers and PERSIST the reduced stack
        // (2026-07 audit follow-up). Previously only drive/record were covered.
        , modManager(std::make_unique<ModManager>(&fil, gameRoots_,
              !driveScriptPath.empty() || !recordScriptPath.empty() ||
              !captureAlo.empty() || !captureRef.empty() || testHost))
    {
        // [world-lit] capture lighting colours (arrays can't init in list).
        m_captureAmbient[0] = ambR; m_captureAmbient[1] = ambG; m_captureAmbient[2] = ambB;
        m_captureSun[0] = sunR; m_captureSun[1] = sunG; m_captureSun[2] = sunB;
        // Automation must isolate the palette BEFORE the saved mod stack is
        // restored: RestoreLastLayerStack activates a mod, which otherwise
        // loads the user's persisted pins/recents before OpenLog/Run begins.
        if (m_automationMode)
            TexturePalette::Store::Instance().SetEphemeral(true);
        // discover installed mods and restore the
        // previously-active one from the registry before any UI shows.
        // Both calls are quick; they don't touch GPU / WebView2 state.
        // Engine pointer is bound later via SetEngine() in WM_CREATE.
        modManager->DiscoverMods();
        modManager->RestoreLastLayerStack();
    }

    void Log(const char* fmt, ...);
    void OpenLog();
    bool RunDriveSelftest(const std::string& kind, int timeoutMs);
    void CloseLog();

    // InitD3D9 dropped; the Engine owns the live D3D9 device. The
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
    // Forward a Win32 mouse message arriving
    // at hMain into the WebView2 composition surface via
    // ICoreWebView2CompositionController::SendMouseInput. The host HWND
    // owns input under composition hosting and must forward. Also handles
    // SetCapture/ReleaseCapture for drag-past-window-edge continuity. The
    // caller (MainWndProc) returns 0 after this so DefWindowProc doesn't
    // double-process the message.
    void    ForwardMouseToCompositionWebView2(UINT msg, WPARAM wp, LPARAM lp);
    void    ResizeWebViewToClient();

    // [resize-perf] End-of-resize-gesture settle: the one deferred
    // Engine::Reset (via LayoutBroker), an exact final put_Bounds, and a
    // fresh frame. Called from WM_EXITSIZEMOVE and the quiescence timer.
    void    SettleResize(const char* why);

    void OnWebMessage(const std::wstring& json);

    // Composition is a hard requirement (there is no HWND fallback). On any
    // composition-setup failure — Compositor::Init, the Environment3 QI, or
    // the async composition-controller completion — surface a clear error
    // (MessageBox) and exit the process rather than leave a black window.
    // [[noreturn]]: flushes host.log, shows the dialog, then ExitProcess.
    [[noreturn]] void FailFatalComposition(HRESULT hr);

    LRESULT MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    int Run(int nCmdShow);
};

// ---------- logging ----------

void HostWindowImpl::OpenLog()
{
    std::wstring path = ComputeHostLogPath();
    const std::wstring perfArtifactDir = host::perf::CurrentConfig().artifactDir;
    // --drive (ephemeral): per-PID log filename so a --drive run's _wfsopen("w")
    // (truncate) never wipes a concurrently-running daily driver's host.log.
    if (m_automationMode)
    {
        const std::wstring suffix = (m_recordMode ? L"-record-" : L"-drive-") + std::to_wstring(GetCurrentProcessId());
        if (!perfArtifactDir.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(perfArtifactDir, ec);
            path = (std::filesystem::path(perfArtifactDir) / (L"host" + suffix + L".log")).wstring();
        }
        else
        {
            const size_t dot = path.find_last_of(L'.');
            path = (dot == std::wstring::npos) ? path + suffix
                                               : path.substr(0, dot) + suffix + path.substr(dot);
        }
    }
    // Hardening — _wfopen_s opens with
    // exclusive default share-mode (_SH_DENYRW) so concurrent readers
    // get EBUSY. Surfaced when the dxgi-transport.spec.ts tried to
    // read host.log via Node fs.readFileSync to assert [COMP-engine-*]
    // log lines. Switch to _wfsopen with _SH_DENYNO so readers (tests,
    // Get-Content -Wait, etc.) can open the file while the host is
    // writing to it. The host is the only writer so deny-no is safe.
    logFile = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (logFile) Log("[host] === host session started ===\n");
}

void HostWindowImpl::CloseLog()
{
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile)
    {
        fputs("[host] === host session ending ===\n", logFile);
        fclose(logFile);
        logFile = nullptr;
    }
}

// --drive bridge-selftest: round-trip one allowlisted request through the
// PRODUCTION page->host channel. ExecuteScript makes the page call
// window.bridge.request(kind) — in a non-CDP --drive run window.bridge IS the
// real NativeBridge — and post a tokened result back over the same wire;
// OnWebMessage completes the wait. A dropped wire fails by timeout.
bool HostWindowImpl::RunDriveSelftest(const std::string& kind, int timeoutMs)
{
    if (useTestHost)
    {
        // --test-host swaps window.bridge for the host-object channel, so the
        // "production wire" premise doesn't hold — refuse rather than lie green.
        Log("drive: bridge-selftest requires a non-test-host --drive run\n");
        return false;
    }
    if (!webView)
    {
        Log("drive: bridge-selftest — WebView2 not ready\n");
        return false;
    }

    static unsigned s_selftestSeq = 0;
    const std::string token =
        std::to_string(GetTickCount64()) + "-" + std::to_string(++s_selftestSeq);
    m_selftestToken = token;
    m_selftestDone = false;
    m_selftestOk = false;

    // kind is allowlist-validated at parse time; token is host-generated — both
    // are safe to embed in single-quoted JS.
    // A faithful inline mini-client of the production wire protocol: the same
    // {type:"req",id,kind,params} envelope NativeBridge sends (bridge/native.ts:80)
    // and the same {type:"res",id,ok} matching it awaits (:133). Deliberately NOT
    // window.bridge: that diagnostic global is currently a broken TestHostBridge
    // in every non-test-host launch (hostObjects.<name> is a truthy lazy proxy
    // even with nothing registered — latent bug, tracked separately). This tests
    // what matters: page->host postMessage, dispatcher handling, host->page
    // response delivery. setTimeout(0) defers out of the ExecuteScript
    // evaluation context, where postMessage throws 0x80070490.
    const std::string js =
        "setTimeout(function(){"
        "var post=function(ok,why){try{window.chrome.webview.postMessage(JSON.stringify("
        "{kind:'drive/selftest-result',token:'" + token + "',ok:!!ok,why:why||''}));}catch(e){}};"
        "var wv=window.chrome&&window.chrome.webview;"
        "if(!wv||typeof wv.postMessage!=='function'){post(false,'no-webview');return;}"
        "var id='selftest-" + token + "';var done=false;"
        "var onMsg=function(e){var m=e.data;"
        "if(typeof m==='string'){try{m=JSON.parse(m);}catch(err){return;}}"
        "if(!m||m.type!=='res'||m.id!==id)return;done=true;"
        "post(!!m.ok,m.ok?'':'res-error: '+(m.error||''));};"
        "wv.addEventListener('message',onMsg);"
        "try{wv.postMessage(JSON.stringify({type:'req',id:id,kind:'" + kind + "',params:{}}));}"
        "catch(e){post(false,'post-threw: '+(e&&e.message||''));return;}"
        "setTimeout(function(){if(!done)post(false,'response-timeout');},8000);"
        "},0);";
    const HRESULT hr = webView->ExecuteScript(
        Utf8ToWide(js).c_str(),
        Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
            [](HRESULT, LPCWSTR) -> HRESULT { return S_OK; }).Get());
    if (FAILED(hr))
    {
        Log("drive: bridge-selftest — ExecuteScript failed (0x%08X)\n", (unsigned)hr);
        m_selftestToken.clear();
        return false;
    }

    // Nested pump: the result arrives as a WebView2-delivered window message, so
    // we must keep dispatching while waiting. Drive mode is single-purpose and
    // the outer loop calls Tick() explicitly (not via messages), so this can't
    // re-enter the runner.
    const ULONGLONG start = GetTickCount64();
    while (!m_selftestDone && GetTickCount64() - start < (ULONGLONG)timeoutMs)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                // Don't swallow shutdown inside the nested pump: re-post so the
                // outer loop sees it, and abort the wait (fails the step).
                PostQuitMessage(static_cast<int>(msg.wParam));
                Log("drive: bridge-selftest aborted by WM_QUIT\n");
                m_selftestToken.clear();
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (m_selftestDone) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
    const bool ok = m_selftestDone && m_selftestOk;
    Log("drive: bridge-selftest %s (kind=%s)\n",
        ok ? "OK" : (m_selftestDone ? "FAILED" : "TIMEOUT"), kind.c_str());
    m_selftestToken.clear();
    return ok;
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

// Composition is the editor's only render transport — there is no HWND
// fallback (hosting-mode removal). When DirectComposition
// or the WebView2 composition controller can't be brought up, the viewport
// would be a permanent black window, so we surface a clear modal error and
// exit cleanly instead. Reached from the synchronous env-setup failures
// (Compositor::Init / Environment3 QI) and the async
// WM_APP_COMPOSITION_FALLBACK handler. host.log is flushed first so the
// failure HRESULT survives the hard exit.
[[noreturn]] void HostWindowImpl::FailFatalComposition(HRESULT hr)
{
    Log("[host] FATAL: composition unavailable (hr=0x%08lx) — exiting\n", hr);
    CloseLog();

    // Interactive only — a headless/automation run has no user to dismiss the box
    // and would hang forever on it; the log line + exit(1) already carry the error.
    if (IsFullyInteractive())
    {
        wchar_t msg[640];
        _snwprintf_s(msg, _TRUNCATE,
            L"Particle Editor could not initialize its DirectComposition "
            L"rendering surface (error 0x%08lX).\n\n"
            L"This build renders the viewport through DirectComposition + WebView2 "
            L"composition hosting and cannot run without it. Make sure your GPU "
            L"drivers are up to date and that the WebView2 runtime is installed.\n\n"
            L"The editor will now close.",
            static_cast<unsigned long>(hr));
        MessageBoxW(nullptr, msg, L"Particle Editor — composition unavailable",
                    MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}

// ---------- D3D9 ----------

// render loop + per-frame spawner tick. Replaces the prior
// placeholder clear-to-background path. The per-frame sequence here
// mirrors the legacy `Render` verbatim:
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

    // In --record mode the spawner is driven EXACTLY ONCE per emitted frame by
    // the ClipRunner step hook (at the fixed virtual dt), so keep incidental
    // renders out of that deterministic schedule.
    if (spawnerDriver && particleSystem && !m_recordMode)
        spawnerDriver->Tick(dt, particleSystem.get(), engine.get());

    // shift-click-to-spawn: refresh cursor velocity from
    // QueryPerformanceCounter deltas before the engine sees it. The
    // attached ParticleSystemInstance reads MouseCursor::GetVelocity
    // through its Object3D parent chain during Update. Mirrors legacy
    // the legacy main.cpp — the legacy render loop calls UpdateVelocity
    // unconditionally each frame whether or not a system is attached.
    m_mouseCursor.UpdateVelocity();

    const LONGLONG perfT0 = PerfQpcNow();
    engine->Update();
    const double perfUpdateUs = PerfUsSince(perfT0);

    // The game-object catalog builds off the UI thread; when Update() just
    // swapped a finished one in, broadcast engine/state/changed so an open reference
    // picker re-queries its now-ready object list (drops the "Loading objects…" state).
    if (dispatcher && engine->ConsumeCatalogReadyFlag())
        dispatcher->EmitEngineStateChanged();

    // Advance the dock-slide viewport interpolation to THIS frame's
    // wall-clock, so the engine below paints the time-lerped scene rect. Placed
    // before perfT1 so the (cheap, no-op-when-idle) advance stays OUTSIDE the
    // [PERF] render-timed region.
    layout.AdvanceSceneAnim(PerfQpcNow());

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

    // Per-frame composite.
    // engine->Render() above issued D3D9 draws into the AlphaCompositor's
    // shared texture. IssueEndFrameQuery markers the D3D9 command stream
    // after those draws; WaitEndFrameQuery spins until the GPU has
    // finished them — cross-device sync path (b).
    // Then CompositeEngineFrame CopyResources from the D3D11 alias into
    // the engine's DXGI swapchain back buffer and Present1's it. DComp
    // picks up the new content on its next composition cycle.
    //
    // Gated on Compositor::IsReady (attachment committed) +
    // engineVisualAttached (attach succeeded). When
    // AttachEngineVisual failed (LUID mismatch, D3D11 device, etc.),
    // CompositeEngineFrame returns S_FALSE and this block is a per-frame
    // no-op with the viewport area empty.
    if (m_compositor && m_compositor->IsReady())
    {
        engine->IssueEndFrameQuery();
        // [PERF] WaitEndFrameQuery is the suspected hot stage — time the
        // busy-spin and capture the spin count it now returns.
        const LONGLONG perfT2     = PerfQpcNow();
        const int      perfSpins  = engine->WaitEndFrameQuery();
        const double   perfWaitUs = PerfUsSince(perfT2);
        // Pass the engine's current shared
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
    // the user's healthy run; read per-stage ratios + spin counts.
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
        // [resize-perf] rps = RenderD3D9 calls in this ~1s window —
        // the REAL render cadence (the fps field is 1/frame-cost, the
        // theoretical max, and stopped tracking cadence once the pump
        // was paced).
        Log("[PERF] win=%ldx%ld rps=%u fps=%.0f frame=%.0f/%.0f update=%.0f/%.0f "
            "render=%.0f/%.0f wait=%.0f/%.0f spins=%.0f/%u composite=%.0f/%.0f (us avg/max)\n",
            pr.right - pr.left, pr.bottom - pr.top, perfFrame.n, fps,
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
        if (host::perf::Enabled())
        {
            const ProcessMemorySnapshot mem = GetProcessMemorySnapshot();
            host::perf::Emit({
                {"eventName", "engine.frame_summary"},
                {"eventType", "counter"},
                {"durationMs", perfFrame.avg() / 1000.0},
                {"windowWidth", pr.right - pr.left},
                {"windowHeight", pr.bottom - pr.top},
                {"frameCount", perfFrame.n},
                {"renderCallsPerSecond", perfFrame.n},
                {"estimatedFpsFromCost", fps},
                {"avgFrameMs", perfFrame.avg() / 1000.0},
                {"maxFrameMs", perfFrame.maxUs / 1000.0},
                {"over16Ms", perfFrame.over16},
                {"over33Ms", perfFrame.over33},
                {"over50Ms", perfFrame.over50},
                {"avgUpdateMs", perfUpdate.avg() / 1000.0},
                {"maxUpdateMs", perfUpdate.maxUs / 1000.0},
                {"avgRenderMs", perfRender.avg() / 1000.0},
                {"maxRenderMs", perfRender.maxUs / 1000.0},
                {"avgGpuWaitMs", perfWait.avg() / 1000.0},
                {"maxGpuWaitMs", perfWait.maxUs / 1000.0},
                {"avgCompositeMs", perfComposite.avg() / 1000.0},
                {"maxCompositeMs", perfComposite.maxUs / 1000.0},
                {"avgRenderSceneMs", perfRScene.avg() / 1000.0},
                {"maxRenderSceneMs", perfRScene.maxUs / 1000.0},
                {"avgRenderBloomMs", perfRBloom.avg() / 1000.0},
                {"maxRenderBloomMs", perfRBloom.maxUs / 1000.0},
                {"avgRenderDistortMs", perfRDistort.avg() / 1000.0},
                {"maxRenderDistortMs", perfRDistort.maxUs / 1000.0},
                {"avgRenderComposeMs", perfRCompose.avg() / 1000.0},
                {"maxRenderComposeMs", perfRCompose.maxUs / 1000.0},
                {"avgRenderPresentMs", perfRPresent.avg() / 1000.0},
                {"maxRenderPresentMs", perfRPresent.maxUs / 1000.0},
                {"avgGpuWaitSpins", spinAvg},
                {"maxGpuWaitSpins", perfWaitSpinsMax},
                {"workingSetBytes", static_cast<unsigned long long>(mem.workingSetBytes)},
                {"privateUsageBytes", static_cast<unsigned long long>(mem.privateUsageBytes)}
            });
        }
        perfUpdate.reset(); perfRender.reset(); perfWait.reset();
        perfComposite.reset(); perfFrame.reset();
        perfRScene.reset(); perfRBloom.reset(); perfRDistort.reset();
        perfRCompose.reset(); perfRPresent.reset();
        perfWaitSpinsSum = 0; perfWaitSpinsMax = 0;
    }

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
    // ([resize-perf] note: an earlier revision throttled put_Bounds to
    // ~30 Hz during sizemove. Reverted after the user's feel verdict —
    // halving the panels' tracking rate read as a regression, and with
    // the per-tick reset now on the cheap ResetEx path there is no
    // budget pressure to justify it.)
    RECT r;
    GetClientRect(hMain, &r);
    // A minimized window has a 0-area client rect; pushing that to put_Bounds
    // makes WebView2 stop rendering — which would blank the headless record's
    // window grab (WebView2 stops producing fresh UI). Keep the last good
    // bounds; on restore WM_SIZE fires again with the real rect. (Harmless
    // generally: a minimized window shows nothing, so a 0-resize is pure waste.)
    if (IsIconic(hMain) || r.right - r.left <= 0 || r.bottom - r.top <= 0) return;
    webController->put_Bounds(r);
    // When main resizes, the viewport popup's screen position
    // may need to change too (the main HWND's client origin shifted
    // in screen space). React will re-send a layout/viewport-rect
    // once its ResizeObserver fires, which is the authoritative
    // source. Just nudge the screen position from the cached client
    // rect in the meantime so the viewport doesn't lag visually.
    layout.RefreshScreenPosition();
}

void HostWindowImpl::SettleResize(const char* why)
{
    // Order matters: reset first so the engine RT matches the settled
    // popup size, exact WebView bounds second, then one fresh frame so
    // the next DWM composition shows post-reset pixels (mirrors the
    // forced render in WM_WINDOWPOSCHANGED).
    layout.SettleDeferredReset();
    ResizeWebViewToClient();
    RenderD3D9();
    Log("[resize-perf] settle (%s)\n", why);
}

void HostWindowImpl::OnWebMessage(const std::wstring& json)
{
    // [resize-perf] bridge message rate, tallied PER
    // KIND (the user's live splitter drag showed ~104/s of NON-scene-rect
    // traffic the dimension audit hadn't ranked; attribution found it was
    // viewport/input at mouse rate). Extracting the kind is a cheap
    // substring scan next to the UTF16→8 + JSON parse that follows.
    // 1 Hz emit of the top kinds; idle emits nothing by construction.
    std::wstring msgKind;
    {
        static const std::wstring kKindNeedle = L"\"kind\":\"";
        const size_t kp = json.find(kKindNeedle);
        if (kp != std::wstring::npos)
        {
            const size_t vs = kp + kKindNeedle.size();
            const size_t ve = json.find(L'"', vs);
            if (ve != std::wstring::npos && ve > vs && ve - vs < 64)
            {
                msgKind = json.substr(vs, ve - vs);
            }
        }
    }

    // --capture first-paint handshake: the React app posts {"kind":"app/ready"}
    // (web/apps/editor/src/lib/app-ready.ts — keep this literal in lockstep)
    // after its first meaningful paint. Intercept it here, BEFORE the
    // per-message log + dispatcher: it's a host-lifecycle signal, not a typed
    // request the dispatcher handles. Set the flag the capture loop waits on and
    // return. Harmless in normal runs (the flag is only read in --capture mode).
    if (msgKind == L"app/ready")
    {
        m_uiReady = true;
        Log("[capture] app/ready received (React first paint)\n");
        // Frameless title bar: replay the current maximized state now that the web
        // can receive it — a launch-maximized window's first WM_SIZE fired before
        // React existed, so the initial glyph would otherwise be stuck at Maximize.
        // app/ready is the ONE moment the web is guaranteed mounted + subscribed (the
        // page itself posts it), so FORCE the emit: reset the dedupe first. Without
        // this, a WM_SIZE in the [dispatcher-ready, app/ready] gap can latch
        // m_lastMaximizedSent to a value the web never actually received (the emit
        // lambda silently drops when webView is null yet EmitWindowState still
        // reports success), permanently short-circuiting this replay. (pre-PR review.)
        m_lastMaximizedSent = -1;
        EmitWindowStateIfChanged();
        return;
    }
    // --drive bridge-selftest result: the page posts {"kind":"drive/selftest-result",
    // "token":…, "ok":…} over the REAL postMessage wire — its arrival here IS the
    // thing under test. Host-lifecycle signal like app/ready (the dispatcher drops
    // non-"req" messages) — intercept + return. Token must match the armed step.
    if (msgKind == L"drive/selftest-result")
    {
        nlohmann::json msg = nlohmann::json::parse(WideToUtf8(json), nullptr, false);
        if (!msg.is_discarded() && !m_selftestToken.empty()
            && msg.value("token", std::string{}) == m_selftestToken)
        {
            m_selftestOk = msg.value("ok", false);
            m_selftestDone = true;
            if (!m_selftestOk)
                Log("drive: selftest page-side failure: %s\n",
                    msg.value("why", std::string("?")).c_str());
        }
        return;
    }
    if (msgKind == L"perf/clock-calibration")
    {
        nlohmann::json msg = nlohmann::json::parse(WideToUtf8(json), nullptr, false);
        nlohmann::json event = {
            {"eventName", "clock_calibration"},
            {"eventType", "instant"},
            {"hostReceiveQpc", host::perf::NowQpc()},
            {"hostQpcFrequency", host::perf::QpcFrequency()}
        };
        if (!msg.is_discarded())
        {
            if (auto it = msg.find("rendererNowMs"); it != msg.end() && it->is_number())
                event["rendererNowMs"] = *it;
            if (auto it = msg.find("rendererTimeOriginMs"); it != msg.end() && it->is_number())
                event["rendererTimeOriginMs"] = *it;
            if (auto it = msg.find("sampleId"); it != msg.end() && it->is_string())
                event["sampleId"] = *it;
        }
        host::perf::Emit(std::move(event));
        return;
    }
    if (msgKind == L"perf/trace")
    {
        nlohmann::json msg = nlohmann::json::parse(WideToUtf8(json), nullptr, false);
        if (!msg.is_discarded())
        {
            nlohmann::json event;
            if (auto it = msg.find("event"); it != msg.end() && it->is_object())
                event = *it;
            if (!event.is_object()) event = nlohmann::json::object();
            if (!event.contains("eventName")) event["eventName"] = "renderer.event";
            if (!event.contains("eventType")) event["eventType"] = "instant";
            event["sourceComponent"] = "renderer";
            host::perf::Emit(std::move(event));
        }
        return;
    }

    ++perfWebMsgs;
    if (!msgKind.empty())
        ++perfMsgKinds[msgKind];

    // --record per-frame ack: React posts {"type":"ui/frame-acked","frame":N}
    // after a double-rAF (frame N's cursor/state has painted). The record loop
    // waits on m_lastAckedFrame to grab a committed composite. Host-lifecycle
    // signal, not a dispatcher request — intercept + return.
    //
    // The SEMANTIC-targeting cursor path carries a nested `cursor` object
    // ({x,y,vis,press,resolved:[...]}) the web side computed against its live DOM;
    // stash it (keyed by frame) for the ClipRunner AckDataFn. The plain literal
    // ack has no `cursor` key — the substring frame scan still works for it.
    if (json.find(L"\"type\":\"ui/frame-acked\"") != std::wstring::npos)
    {
        static const std::wstring kFrameNeedle = L"\"frame\":";
        const size_t fp = json.find(kFrameNeedle);
        if (fp != std::wstring::npos)
            m_lastAckedFrame = _wtoi(json.c_str() + fp + kFrameNeedle.size());

        if (json.find(L"\"cursor\":") != std::wstring::npos)
        {
            nlohmann::json msg = nlohmann::json::parse(WideToUtf8(json), nullptr, false);
            if (!msg.is_discarded() && msg.is_object() && msg.contains("cursor")
                && msg["cursor"].is_object())
            {
                nlohmann::json cur = msg["cursor"];
                // Split the resolved array out of the cursor object so the sidecar
                // gets cursor:{x,y,vis,press} + resolved:[...] as siblings.
                m_lastAckResolved = cur.contains("resolved") && cur["resolved"].is_array()
                                        ? cur["resolved"] : nlohmann::json::array();
                cur.erase("resolved");
                m_lastAckCursor = std::move(cur);
                m_lastAckCursorFrame = msg.value("frame", -1);
            }
        }
        return;
    }

    // Per-message log hygiene: the interactive streams
    // (layout/scene-rect at ~28/s during a splitter drag, viewport/input
    // at mouse rate ~60-140/s whenever the cursor crosses the viewport)
    // each paid a host.log write + fflush — a synchronous DISK flush per
    // message on the UI thread. Skip their per-message line; the 1 Hz
    // [resize-perf] bridge tally above carries their rates, and every
    // other (low-frequency) kind keeps the full per-message log.
    const bool highFrequencyKind =
        msgKind == L"layout/scene-rect" || msgKind == L"viewport/input";
    if (!highFrequencyKind)
        Log("[host] WebMsg (%zu chars)\n", json.size());
    const DWORD rpNow = GetTickCount();
    if (perfMsgLastEmit == 0)
    {
        perfMsgLastEmit = rpNow;
    }
    else if ((rpNow - perfMsgLastEmit) >= 1000)
    {
        // Top-4 kinds by count, formatted "kind=count".
        std::vector<std::pair<std::wstring, unsigned>> kinds(
            perfMsgKinds.begin(), perfMsgKinds.end());
        std::sort(kinds.begin(), kinds.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        char detail[256] = "";
        size_t off = 0;
        for (size_t i = 0; i < kinds.size() && i < 4; ++i)
        {
            const int n = _snprintf_s(detail + off, sizeof(detail) - off, _TRUNCATE,
                                      "%s%ls=%u", i ? " " : "",
                                      kinds[i].first.c_str(), kinds[i].second);
            if (n < 0) break;
            off += static_cast<size_t>(n);
        }
        Log("[resize-perf] bridge: msgs=%u top[%s] (per ~1s)\n", perfWebMsgs, detail);
        if (host::perf::Enabled())
        {
            host::perf::Emit({
                {"eventName", "host.bridge_message_summary"},
                {"eventType", "counter"},
                {"messageCount", perfWebMsgs},
                {"topKinds", detail}
            });
        }
        perfWebMsgs = 0;
        perfMsgKinds.clear();
        perfMsgLastEmit = rpNow;
    }

    // --capture determinism gate: note the first layout/scene-rect BEFORE
    // dispatching it — the capture loop holds its frame counter until React's
    // layout has landed, because that message resizes the engine RT (counting
    // from process start raced it and produced phase-dependent capture sizes).
    if (msgKind == L"layout/scene-rect")
        m_sceneRectSeen = true;

    if (dispatcher)
        dispatcher->Dispatch(WideToUtf8(json));
}

HRESULT HostWindowImpl::InitWebView2()
{
    const bool captureIsolation = !m_captureAlo.empty() || !m_captureRef.empty() || m_automationMode;
    std::wstring userDataFolder = m_perfWebViewProfile.empty()
        ? ComputeUserDataFolder(captureIsolation)
        : m_perfWebViewProfile;
    if (!m_perfWebViewProfile.empty())
        SHCreateDirectoryExW(nullptr, userDataFolder.c_str(), nullptr);
    Log("[host] WebView2 user-data folder: %ls%s\n", userDataFolder.c_str(),
        !m_perfWebViewProfile.empty() ? " (perf profile)" :
        captureIsolation ? " (isolated capture profile)" : "");

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
            // --force-renderer-accessibility enables Blink's
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
                // Stash for WebResourceRequested.
                webEnv = env;

                // Composition hosting. Stand up the
                // host::Compositor (DComp V1 device only, no tree yet — tree
                // assembly is deferred until inside the composition-controller
                // completion callback) and create a
                // CompositionController. Composition is a hard requirement:
                // on Compositor::Init or the Environment3 QI
                // failing there is NO HWND fallback — fail with a clear error
                // and exit rather than leave a black window.
                m_compositor = std::make_unique<host::Compositor>(
                    hMain,
                    [this](const std::string& s) { Log("%s\n", s.c_str()); });
                HRESULT chr = m_compositor->Init();
                if (FAILED(chr))
                {
                    Log("[host] composition: Compositor::Init failed hr=0x%08lx\n", chr);
                    FailFatalComposition(chr);
                }

                // QI for Environment3 — exposes
                // CreateCoreWebView2CompositionController. Confirmed
                // available in SDK 1.0.3967.48 (WebView2.h:42610).
                ComPtr<ICoreWebView2Environment3> env3;
                HRESULT qihr = env->QueryInterface(IID_PPV_ARGS(&env3));
                if (FAILED(qihr) || !env3)
                {
                    Log("[host] composition: QI Environment3 failed hr=0x%08lx\n", qihr);
                    FailFatalComposition(qihr);
                }

                Log("[host] composition: CreateCoreWebView2CompositionController dispatching\n");
                return env3->CreateCoreWebView2CompositionController(
                    hMain,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [this](HRESULT cHr, ICoreWebView2CompositionController* ctl) -> HRESULT
                        {
                            return OnCompositionControllerReady(cHr, ctl);
                        }).Get());
            }).Get());
    Log("[host] CreateCoreWebView2EnvironmentWithOptions returned 0x%08lx (testHost=%d)\n",
        envCreateHr, useTestHost ? 1 : 0);
    return envCreateHr;
}

// ---------------------------------------------------------------------
// Shared per-controller setup. Runs after
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
    //     launch surfaces it.
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
            else
            {
                // Production must set this EXPLICITLY. The property was
                // previously only ever touched in the test-host branch, so a
                // shipped build inherited the WebView2 default — which
                // Microsoft documents as TRUE on ICoreWebView2Settings
                // (get_AreDevToolsEnabled: "The default value is TRUE").
                // F12 therefore opened DevTools on the privileged editor page,
                // where the native bridge is reachable from the console
                // (2026-07 audit, an-audit-finding).
                settings->put_AreDevToolsEnabled(FALSE);
                Log("[host] production: DevTools disabled\n");
            }
            // Frameless custom title bar: enable non-client region support so the
            // web title bar's `app-region: drag` region is reported as the window
            // caption (queried in WM_NCHITTEST via the composition controller).
            // Versioned interface (Settings9, runtime 1.0.2420.47+); takes effect
            // on the NEXT navigation (this runs before Navigate). A QI/put_ failure
            // on an older runtime leaves the flag false → host HTCAPTION fallback.
            ComPtr<ICoreWebView2Settings9> settings9;
            if (SUCCEEDED(settings.As(&settings9)) && settings9 &&
                SUCCEEDED(settings9->put_IsNonClientRegionSupportEnabled(TRUE)))
            {
                m_ncRegionEnabled = true;
                Log("[host] WebView2 non-client region support ENABLED (frameless title bar)\n");
            }
            else
            {
                Log("[host] WebView2 non-client region support UNAVAILABLE — HTCAPTION fallback\n");
            }
        }
    }

    // Task 2.2.1: expose hostBridge via AddHostObjectToScript
    // (--test-host only). WebView2 drops postMessage under
    // CDP attachment; the host-object
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

    // Mirror the web document title into the Win32 titlebar. React owns
    // the title format (dirty ● + basename + app name — see
    // web/apps/editor/src/lib/window-title.ts); the host just reflects
    // document.title so the titlebar, taskbar, and Alt-Tab always show
    // the open .alo file. Fires once for index.html's static <title> at
    // navigation, then on every document.title assignment.
    if (webView)
    {
        webView->add_DocumentTitleChanged(
            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this](ICoreWebView2* sender, IUnknown* /*args*/) -> HRESULT
                {
                    LPWSTR title = nullptr;
                    HRESULT thr = sender->get_DocumentTitle(&title);
                    if (SUCCEEDED(thr) && title)
                    {
                        SetWindowTextW(hMain, title);
                        CoTaskMemFree(title);
                    }
                    else
                    {
                        Log("[host] get_DocumentTitle failed hr=0x%08lx\n", thr);
                    }
                    return S_OK;
                }).Get(),
            &docTitleTok);
        Log("[host] DocumentTitleChanged handler registered\n");
    }

    // Fit to client. Skip a minimized/degenerate seed (#509 same-class guard):
    // a 0-area put_Bounds makes WebView2 stop rendering; a later positive-size
    // WM_SIZE (→ ResizeWebViewToClient) re-seeds. Non-iconic startup is the
    // normal case, so this is a no-op except a rare start-minimized launch.
    RECT bounds;
    GetClientRect(hMain, &bounds);
    if (!IsIconic(hMain) && bounds.right - bounds.left > 0 && bounds.bottom - bounds.top > 0)
        controller->put_Bounds(bounds);

    // Viewport is now a top-level WS_POPUP
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
    std::wstring prodNavUrl = L"https://app.local/index.html";
    if (!useDevUi)
    {
        std::wstring distPath = ComputeEditorDistPath();
        Log("[host] editor dist: %ls\n", distPath.c_str());

        ComPtr<ICoreWebView2_3> wv3;
        webView.As(&wv3);
        if (wv3)
        {
            wv3->SetVirtualHostNameToFolderMapping(
                kVirtualHostName, distPath.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        }

        // Cache-bust the (unhashed) entry document so a rebuilt dist is
        // served fresh — see CacheBust.h for why we can't set a
        // Cache-Control header on the mapped host. Append the build's
        // index.html mtime as ?v=…; it changes on every rebuild and is
        // stable across relaunches of the same build (unchanged hashed
        // assets stay cache-warm).
        std::error_code ec;
        std::filesystem::path indexPath =
            std::filesystem::path(distPath) / L"index.html";
        auto wt = std::filesystem::last_write_time(indexPath, ec);
        if (ec)
        {
            Log("[host] cache-bust: index.html mtime unavailable (%s); "
                "navigating without ?v=\n", ec.message().c_str());
        }
        else
        {
            long long ticks =
                static_cast<long long>(wt.time_since_epoch().count());
            std::wstring token = host::CacheBustToken(ticks);
            if (!token.empty())
            {
                prodNavUrl = host::AppendCacheBustQuery(prodNavUrl, token);
                Log("[host] cache-bust: %ls\n", prodNavUrl.c_str());
            }
            else
            {
                // Read succeeded but the tick count was non-positive (only
                // possible on a non-FILETIME file_clock epoch) — distinct
                // from the read-failure case above, so log it separately
                // instead of silently dropping the bust.
                Log("[host] cache-bust: index.html mtime read OK but ticks "
                    "non-positive (%lld); navigating without ?v=\n", ticks);
            }
        }
    }

    // Subscribe to JS → host messages.
    // Stash the registration token in the member
    // webMessageTok so WM_DESTROY can explicitly unsubscribe.
    if (host::perf::Enabled())
        prodNavUrl = AppendQueryParam(prodNavUrl, L"perfTrace=1");

    webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT
            {
                // Reject messages whose originating document
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

    // Navigation / new-window / permission policy. Registered
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

    // A WebResourceRequested handler will NOT fire for the
    // mapped app.local host (SetVirtualHostNameToFolderMapping short-circuits
    // user handlers), so we cannot inject a Cache-Control response header to
    // keep a rebuilt bundle fresh. The cache-bust above (prodNavUrl's
    // ?v=<mtime>) is the header-free workaround.

    // --capture diagnosability: log when the app document finishes loading, so a
    // ui-ready timeout in the capture loop is attributable ("navigation never
    // finished" vs "loaded but React never signaled app/ready"). Registered
    // before Navigate so the first load is observed; token removed in WM_DESTROY.
    webView->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT
            {
                BOOL ok = FALSE;
                COREWEBVIEW2_WEB_ERROR_STATUS err = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
                if (args)
                {
                    args->get_IsSuccess(&ok);
                    args->get_WebErrorStatus(&err);
                }
                Log("[capture] NavigationCompleted (success=%d webErrorStatus=%d)\n",
                    ok ? 1 : 0, static_cast<int>(err));
                return S_OK;
            }).Get(), &navCompletedTok);

    // Navigate to the React app.
    if (useDevUi)
    {
        Log("[host] dev-ui: Navigate to Vite dev server\n");
        const std::wstring devUrl = host::perf::Enabled()
            ? L"http://localhost:5174/?perfTrace=1"
            : L"http://localhost:5174/";
        webView->Navigate(devUrl.c_str());
    }
    else
    {
        webView->Navigate(prodNavUrl.c_str());
    }
    Log("[host] Navigate dispatched\n");
    return S_OK;
}

// ---------------------------------------------------------------------
// Composition controller completion callback.
// Mirrors dxgi_spike.cpp:OnCompositionControllerReady. Order:
//   1. Stash the composition controller (kept alive for WM_DESTROY).
//   2. QI down to ICoreWebView2Controller and run the shared
//      FinishWebView2ControllerSetup. All wire-up post-step is identical
//      to HWND mode (transparent bg, AcceleratorKeyPressed, put_Bounds,
//      Navigate, ...).
//   3. Build the DComp tree NOW (deferred — must happen AFTER
//      the controller exists). Compositor::AttachWebView2 plugs the
//      controller's RootVisualTarget into the webview visual + Commits.
// If step 3 fails: it's the opaque-white failure mode. Log and return; the
// editor still has the controller wired so the rest of the host stays
// alive, but the visual tree won't show anything. Per the acceptance
// criteria, this is the load-bearing observation.
// ---------------------------------------------------------------------
HRESULT HostWindowImpl::OnCompositionControllerReady(
    HRESULT chr, ICoreWebView2CompositionController* ctl)
{
    if (FAILED(chr) || !ctl)
    {
        Log("[host] composition: controller completion FAILED hr=0x%08lx ctl=%p\n",
            chr, static_cast<void*>(ctl));
        // Composition is required (no HWND fallback): signal a fatal error
        // on the next message-loop iteration. PostMessage so this callback
        // can unwind first.
        HRESULT failHr = (chr == S_OK) ? E_FAIL : chr;
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(failHr), 0);
        return failHr;
    }
    m_compositionController = ctl;
    Log("[host] composition: controller ready, QI to base for shared setup\n");

    // Frameless title bar: QI to ICoreWebView2CompositionController4 for
    // GetNonClientRegionAtPoint (used in WM_NCHITTEST to translate the web title
    // bar's app-region:drag into HTCAPTION). Null on an older runtime → fallback.
    if (SUCCEEDED(m_compositionController.As(&m_compositionController4)) && m_compositionController4)
        Log("[host] composition: NonClientRegion query interface (Controller4) ready\n");
    else
        Log("[host] composition: Controller4 unavailable — HTCAPTION fallback for drag\n");

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
        // Composition is required (no HWND fallback): signal a fatal error.
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(qihr), 0);
        return qihr;
    }

    HRESULT setupHr = FinishWebView2ControllerSetup(baseController.Get());
    if (FAILED(setupHr))
    {
        Log("[host] composition: shared controller setup failed hr=0x%08lx\n", setupHr);
        // Composition is required (no HWND fallback): signal a fatal error.
        PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(setupHr), 0);
        return setupHr;
    }

    // DPI. Composition hosting doesn't
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

    // Cursor sync. The composition
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
    // documented opaque-white failure mode. Per the acceptance gate:
    // STOP, capture binary + log + screenshot, surface to user. Do
    // not iterate beyond the 24h cap.
    if (m_compositor)
    {
        HRESULT bhr = m_compositor->AttachWebView2(ctl);
        if (FAILED(bhr))
        {
            // Composition-class failure: the WebView2 RootVisualTarget couldn't be
            // plugged into the DComp tree, so nothing composites — a black
            // window, exactly what the composition hard-requirement exists to
            // prevent. There is no HWND fallback: signal a fatal error.
            // PostMessage so this callback unwinds before the modal + exit.
            // (Engine-visual attach below is DIFFERENT — that failure keeps
            // the chrome usable, so it stays soft.)
            Log("[host] composition: Compositor::AttachWebView2 FAILED hr=0x%08lx — composition-class failure\n", bhr);
            PostMessageW(hMain, WM_APP_COMPOSITION_FALLBACK, static_cast<WPARAM>(bhr), 0);
            return bhr;
        }
        // Seed the tree to the current client size so the first paint
        // is sized correctly. SetSize commits internally. Skip a
        // minimized/degenerate seed (#509 same-class guard): a 0-/negative-area
        // SetSize corrupts the surface; a later positive-size WM_SIZE re-seeds.
        RECT r;
        GetClientRect(hMain, &r);
        const int clientW = r.right  - r.left;
        const int clientH = r.bottom - r.top;
        if (!IsIconic(hMain) && clientW > 0 && clientH > 0)
            m_compositor->SetSize(clientW, clientH);

        // Attach engine visual BEHIND the
        // WebView2 visual. On failure, log
        // and continue with composition mode intact: chrome works,
        // viewport area stays empty (explicit
        // no-chain-into-HWND-mode). The per-frame
        // CompositeEngineFrame call site is wired later; until then this attach
        // is functionally "load the engine visual into the tree
        // but don't Present it" — its smoke matches the
        // chrome-only output.
        if (engine && engine->GetSharedTextureHandle())
        {
            HANDLE sharedTex = engine->GetSharedTextureHandle();
            LUID   engineLuid = engine->GetAdapterLuid();
            HRESULT ehr = m_compositor->AttachEngineVisual(sharedTex, clientW, clientH, engineLuid);
            if (FAILED(ehr))
            {
                Log("[host] composition: AttachEngineVisual hr=0x%08lx — composition mode continues with engine visual NOT attached (viewport area will be empty)\n", ehr);
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

        // Inject the DComp Compositor into the
        // LayoutBroker so React-side layout/scene-rect dispatches start
        // routing into Compositor::SetEngineVisualTransform + Engine::
        // SetSceneViewport. The setter also replays the cached scene-
        // rect onto the newly-attached compositor via ReemitSceneRect,
        // so if React HAS already dispatched a scene-
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
            // invariant "engine visual ALWAYS has an
            // explicit transform under composition mode."
            //
            // The seed also makes the boot-time
            // [COMP-engine-transform] / [engine] SetSceneViewport log
            // lines appear before React's first dispatch — useful as
            // a positive control + asserted by the dxgi-scene-rect
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
                // Restore engine viewport seed with per-
                // pixel-FoV-vs-current-RT reference. At seed time
                // sceneH equals BackBufferHeight (full client), so
                // SetSceneViewport's per-pixel-FoV computes
                // fovY = 45° × clientH/RT_H = 45° — matches the
                // full-RT projection exactly. No FoV explosion at
                // attach.
                if (engine)
                {
                    engine->SetSceneViewport(sx, sy, sw, sh);
                }
            }
        }

        Log("[host] composition hosting ready (DComp tree committed)\n");
    }

    // Give WebView2 logical
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
// Mouse forwarding under composition hosting.
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
    //  the forwarder doesn't forward them. The 99-test suite doesn't
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

// Frameless title bar: a maximized borderless window fills the whole monitor and
// would cover an auto-hide taskbar so it can never re-show. Query the auto-hide
// bar per edge and leave a 1px sliver on its actual edge (top/left/right/bottom)
// so the reveal still works. Resolve the monitor from the PROPOSED rect (rgrc[0]),
// not the HWND: during a cross-monitor maximize the window's current position still
// resolves to the old monitor, so MonitorFromWindow would inset the wrong monitor's
// taskbar edge. (pre-PR Win32 review.)
static void InsetForAutoHideTaskbar(RECT& client)
{
    APPBARDATA state = { sizeof(state) };
    if (!(SHAppBarMessage(ABM_GETSTATE, &state) & ABS_AUTOHIDE)) return;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfo(MonitorFromRect(&client, MONITOR_DEFAULTTONEAREST), &mi))
    {
        client.bottom -= 1;   // fallback: assume the common bottom edge
        return;
    }
    for (const UINT edge : { ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT })
    {
        APPBARDATA q = { sizeof(q) };
        q.uEdge = edge;
        q.rc    = mi.rcMonitor;   // query the bar on this window's monitor
        if (SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q))
        {
            switch (edge)
            {
                case ABE_TOP:    client.top    += 1; break;
                case ABE_LEFT:   client.left   += 1; break;
                case ABE_RIGHT:  client.right  -= 1; break;
                default:         client.bottom -= 1; break;   // ABE_BOTTOM
            }
            return;
        }
    }
}

LRESULT HostWindowImpl::MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        // Viewport is a top-level WS_POPUP window OWNED by main
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
        // WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA) replaces
        // the earlier SetWindowRgn cut-out. The AlphaCompositor pushes a
        // pre-multiplied ARGB bitmap each tick, the OS composites the
        // popup onto the WebView2 underneath, and software alpha stamps
        // carve soft-edged holes for chrome occlusion rects.
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

        // no host-owned D3D9 device. The Engine constructs the
        // live device internally below, targeting this viewport HWND.

        // Construct the Engine now that both HWNDs exist. hFocus = parent,
        // hDevice = viewport child — same wiring as legacy main.cpp.
        try
        {
            engine = std::make_unique<Engine>(
                hwnd, hViewport, textureManager, shaderManager, fileManager);
            if (dispatcher) dispatcher->SetEngine(engine.get());
            layout.SetEngine(engine.get());
            // bind engine to ModManager so subsequent
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
                    // main.cpp's startup restore so the
                    // new-UI viewport opens with the user's persisted
                    // background / ground / skydome instead of engine ctor
                    // defaults. Same value names/types legacy reads, so
                    // settings round-trip between the two UIs. Same
                    // !useTestHost gate as bloom: the a11y goldens (e.g.
                    // dialog-lighting's "Show ground" toggle) must see
                    // deterministic ctor defaults. GroundZ is intentionally
                    // NOT restored — legacy resets it to 0 each launch by
                    // design.
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

                    // Game-dome environment: battle context + the two chosen
                    // GameObject Names. This restore block runs after the device is
                    // up, so SetSkydomeEnvironment resolves + uploads the meshes now
                    // (the only place the new UI re-resolves a name-based selection).
                    {
                        std::wstring primW = readSz(L"SkydomePrimaryName");
                        std::wstring secW  = readSz(L"SkydomeSecondaryName");
                        if (!primW.empty() || !secW.empty())
                        {
                            DWORD ctxDw = 1;   // default Space
                            readDword(L"SkydomeContext", ctxDw);
                            engine->SetSkydomeEnvironment(
                                ctxDw == 0 ? SkydomeContext::Land : SkydomeContext::Space,
                                WideToAnsi(primW), WideToAnsi(secW));
                        }
                    }

                    // Imported reference object + unit grid. At
                    // startup the catalog isn't built yet, so SetReferenceObject DEFERS:
                    // it kicks the off-thread catalog build and the mesh resolves/uploads
                    // a frame or more later, once Update() harvests the catalog and reruns
                    // the deferred rebuild (so a restored object isn't on the first frame).
                    // Transform / grid spacing are REG_BINARY floats; visibility / grid
                    // toggle / snap toggle are REG_DWORD.
                    {
                        auto readFloats = [&](const wchar_t* name, float* out, DWORD count) -> bool {
                            DWORD t = 0, cb = count * sizeof(float);
                            if (RegQueryValueExW(hKey, name, nullptr, &t,
                                    reinterpret_cast<BYTE*>(out), &cb) != ERROR_SUCCESS
                                || t != REG_BINARY || cb != count * sizeof(float))
                                return false;
                            // Reject NaN/Inf so a corrupt blob can't poison the
                            // transform matrix (mirrors the bloom readF guard above).
                            for (DWORD i = 0; i < count; ++i)
                                if (!(out[i] == out[i] && (out[i] - out[i]) == 0.0f))
                                    return false;
                            return true;
                        };
                        float xform[6] = { 0, 0, 0, 0, 0, 0 };
                        if (readFloats(L"ReferenceObjectTransform", xform, 6))
                            engine->SetReferenceObjectTransform(
                                D3DXVECTOR3(xform[0], xform[1], xform[2]),
                                D3DXVECTOR3(xform[3], xform[4], xform[5]));
                        if (readDword(L"ReferenceObjectVisible", dw))
                            engine->SetReferenceObjectVisible(dw != 0);
                        if (readDword(L"GridVisible", dw))
                            engine->SetGridVisible(dw != 0);
                        float spacing = 0.0f;
                        if (readFloats(L"GridSpacing", &spacing, 1))
                            engine->SetGridSpacing(spacing);
                        // Persistent gizmo snap toggle (REG_DWORD, like GridVisible).
                        if (readDword(L"SnapEnabled", dw))
                            engine->SetSnapEnabled(dw != 0);
                        // Restore the persisted lock so a frozen
                        // object comes back frozen. (Ordering vs. the Name read isn't
                        // load-bearing: the silent restore force-deselects below
                        // regardless, so the object lands deselected either way — the
                        // lock flag just needs to be set before the user can interact.)
                        if (readDword(L"ReferenceObjectLocked", dw))
                            engine->SetReferenceLocked(dw != 0);
                        // Name LAST so the mesh loads once with the transform in
                        // place; guard on non-empty so an unset selection doesn't
                        // clobber a debug ALO_LT7_TEST_OBJECT env-hook mesh.
                        //
                        // In headless --capture mode NEVER restore the persisted
                        // reference object: the capture supplies its own object (the
                        // ALO_LT7_TEST_OBJECT env hook, or --capture-ref via
                        // SetReferenceObject below), and restoring here would both
                        // clobber that mesh AND force the capture script to mutate the
                        // registry to suppress it — which, if the script is interrupted,
                        // wipes the user's saved selection. Skipping makes captures
                        // registry-inert and crash-safe by construction.
                        const bool inCaptureMode = !m_captureAlo.empty() || !m_captureRef.empty();
                        std::wstring nameW = inCaptureMode ? std::wstring()
                                                           : readSz(L"ReferenceObjectName");
                        if (!nameW.empty())
                        {
                            engine->SetReferenceObject(WideToAnsi(nameW));
                            // A silent startup restore is NOT a pick -- load the
                            // object inert so the gizmo + selection box appear only when
                            // the user clicks its body (honours the selection-gating).
                            engine->SetReferenceObjectSelected(false);
                        }
                    }

                    // [lighting-restore, session 12] Restore the persisted
                    // lighting (sun / fill1 / fill2 angles + colours +
                    // intensities, ambient, shadow) so the new-UI viewport
                    // opens with the user's saved lights instead of engine
                    // ctor defaults. Mirrors the legacy `PushLightingToEngine`
                    // (native Win32 UI, since removed) field-for-field, including the
                    // Force-Align fill-angle computation: when the
                    // LightingForceFillAlignment flag is ON the fill azimuths
                    // are derived from the sun (sun.z + 120° / + 210°, both at
                    // -10° tilt); when OFF the persisted free-edit angles feed
                    // the engine directly. Floats are REG_BINARY (readF),
                    // colours + the flag are REG_DWORD. Same !useTestHost gate
                    // as the rest of this block (the engine snapshot the
                    // dialog-lighting a11y golden seeds from must show ctor
                    // defaults under --test-host). Intensity is folded into the
                    // diffuse/specular channels exactly as the legacy `MakeLight`
                    // (native Win32 UI, since removed) did; fills pass specular=black.
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
                    // Ambient pushes alpha w=1 — game-faithful. The engine folds
                    // scene ambient into its SPH lighting as ambient.xyz * ambient.w
                    // (src/SphericalHarmonics.cpp:76), so w gates the per-vertex mesh
                    // ambient floor; per Petroglyph's shaders (reference/foc-shaders/
                    // AlamoEngine.fxh) production Mesh*/RSkin* light ambient ONLY via
                    // that SPH path, so w=1 reproduces the game's mesh brightness.
                    // This and the React `ambientToVec4` (LightingPanel.tsx) push the
                    // same w=1, so load == Reset (keep both in lockstep).
                    auto ambientToVec4 = [](COLORREF c) -> D3DXVECTOR4 {
                        return D3DXVECTOR4(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                                           GetBValue(c) / 255.0f, 1.0f);
                    };
                    // Shadow keeps w=0: m_shadow.xyz drives the reference-model shadow
                    // darken tint (Engine::RenderReferenceShadows), so it IS sampled at
                    // render time. The alpha (w) is unused by the darken — only .xyz is read.
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

                    // Force-align fill angles (verbatim from the legacy dialog).
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
                    engine->SetAmbient(ambientToVec4(sunAmbient));
                    engine->SetShadow (colorToVec4(sunShadow));

                    // The standing no-user verification channel for the
                    // lighting restore — distinct from [view-restore]
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
                    // confirmed (host.log is trusted under this architecture; agent
                    // screenshots are not).
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
            // Any headless mode is unattended: a modal would hang the run with
            // nothing to dismiss it (the pump's engine-null arm exits non-zero
            // instead). Covers --capture/--capture-ref/--test-host, not just
            // --drive/--record — IsFullyInteractive() is the complete predicate.
            if (IsFullyInteractive())
                MessageBoxA(hwnd, e.what(), "Engine init failed", MB_ICONERROR);
            // Continue — viewport will still clear, just without engine state.
        }
        catch (...)
        {
            Log("[host] Engine construction threw unknown exception\n");
            // Continue without engine; snapshot will return ok:false.
        }

        // Stand up the alpha compositor against the Engine's D3D9
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
                // Arm the eager reference-object catalog prefetch now
                // that the new-UI render path is up.
                engine->ArmCatalogPrefetch();
                layout.SetAlphaCompositor(alphaCompositor.get());
                Log("[host] AlphaCompositor up (%ldx%ld)\n",
                    vrc.right - vrc.left, vrc.bottom - vrc.top);

                // Stand up the InputDispatcher on the
                // viewport popup so DOM-routed camera/keyboard input reaches
                // the engine. Bound to BridgeDispatcher below in Run() once
                // `dispatcher` exists.
                if (alphaCompositor)
                {
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
                Log("[host] AlphaCompositor init failed: %s — engine will Present directly\n", e.what());
                alphaCompositor.reset();
                m_inputDispatcher.reset();
            }
        }

        // Seed the first paint (suppresses white-flash on startup; see
        // PoC visual gate notes in the task brief).
        InvalidateRect(hViewport, nullptr, FALSE);

        // Start the 4 Hz stats timer. Fires every 250 ms and emits a
        // stats/tick event to React so the status bar stays live.
        SetTimer(hwnd, kStatsTimerId, 250, nullptr);

        // Two-tier autosave timers (30 s recent / 5 min stable),
        // mirroring the legacy main.cpp. Gated on !useTestHost so harness
        // runs never write autosave files — those would orphan into a
        // recovery prompt for the user's real editor. WM_TIMER latches the
        // dirty-gated write (see the [C4] note below). Also gated on
        // !captureMode ([C4] review): a --capture run skips the paced idle
        // branch that services the latch, so its pending write could only
        // land via the busy-override — and an ephemeral capture has no
        // business writing recovery files anyway (same orphan-prompt
        // rationale as --drive).
        if (!useTestHost && !m_automationMode && m_captureAlo.empty())
        {
            SetTimer(hwnd, Autosave::RECENT_TIMER_ID, Autosave::RECENT_INTERVAL_MS, nullptr);
            SetTimer(hwnd, Autosave::STABLE_TIMER_ID, Autosave::STABLE_INTERVAL_MS, nullptr);
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == kStatsTimerId && dispatcher)
        {
            // [B1] Heartbeat flush: modal dialogs (file pickers etc.) run
            // their own message pump, which starves the paced idle branch —
            // but still dispatches WM_TIMER, so a coalesced trailing
            // broadcast is at worst one stats tick (250 ms) stale there.
            dispatcher->FlushPendingEmits();
            // Record mode: the sim advances exactly tl.fps virtual frames/sec
            // (StepPreviewFrames), so the wall-clock render rate is the wrong
            // number to show — and it swings with the capture barriers, which
            // made the FPS chip a run-variant in recorded clips. Locked from
            // the moment the timeline parses (before frame 0's settle) so no
            // captured frame ever carries a wall-clock value.
            float fps      = (m_recordMode && m_recordTimelineFps > 0)
                               ? static_cast<float>(m_recordTimelineFps)
                               : fpsMeasurer.getFPS();
            int emitters   = engine ? engine->GetNumEmitters()  : 0;
            int particles  = engine ? engine->GetNumParticles() : 0;
            int instances  = engine ? engine->GetNumInstances() : 0;
            bool overload  = engine ? engine->IsSpawnOverloadActive() : false;
            dispatcher->EmitStatsTick(fps, emitters, particles, instances, overload);
        }
        // Autosave tick. Best-effort + dirty-gated — skip the write
        // when nothing changed since the last save (no point autosaving an
        // unmodified saved file).
        //
        // [C4] DEFERRED: the timer no longer writes inline — a WM_TIMER can
        // fire mid-gesture (gizmo drag, splitter, modal resize pump) and the
        // serialize+temp-write+rename then stalls the UI thread at the worst
        // moment. The tick just latches m_autosavePending; the paced idle
        // branch services it right after a presented frame when no capture /
        // size-move is active (ServicePendingAutosave). Busy-override: if the
        // pending write can't land within one RECENT interval (continuous
        // gesture), the NEXT timer tick forces it inline — the crash-safety
        // window is bounded at ~2x the tier cadence, never unbounded.
        else if ((wp == Autosave::RECENT_TIMER_ID || wp == Autosave::STABLE_TIMER_ID)
                 && dispatcher && particleSystem && dispatcher->GetDirty())
        {
            Autosave::Tier tier = (wp == Autosave::RECENT_TIMER_ID)
                                ? Autosave::Tier::Recent
                                : Autosave::Tier::Stable;
            // Stable outranks Recent if both end up pending (rarer cadence,
            // and the stable slot is the one recovery prefers).
            if (m_autosavePendingTier != Autosave::Tier::Stable)
                m_autosavePendingTier = tier;
            if (!m_autosavePending)
            {
                m_autosavePending = true;
                m_autosavePendingSince = GetTickCount64();
            }
            else if (GetTickCount64() - m_autosavePendingSince
                     >= Autosave::RECENT_INTERVAL_MS)
            {
                // Busy-override: still pending a full interval later —
                // write now regardless of gesture state.
                ServicePendingAutosave(true);
            }
        }
        // [resize-perf] quiescence safety net — fires
        // 150 ms after size ticks stop; normally a no-op (per-tick
        // cheap resets keep sizes in sync), it only re-resets if a
        // mid-gesture reset failed. Covers a lost WM_EXITSIZEMOVE.
        else if (wp == kResizeSettleTimerId)
        {
            KillTimer(hwnd, kResizeSettleTimerId);
            SettleResize(m_inSizeMove ? "quiescence-pause" : "quiescence");
        }
        return 0;

    // ---- Frameless custom title bar (pre-PR Win32 review recipe) ----
    // The WebView is a COMPOSITION controller (no child HWND), so the host owns
    // ALL hit-testing: the web title bar's `app-region: drag` becomes HTCAPTION
    // only because WM_NCHITTEST translates it (via GetNonClientRegionAtPoint).
    // Returning HTCAPTION for the caption band hands DefWindowProc the full native
    // caption behavior — drag-move, double-click-maximize, drag-to-restore,
    // Alt+Space / right-click system menu — for free. WM_NC* aren't intercepted by
    // the client-area mouse forwarding, so those reach DefWindowProc unimpeded.
    case WM_NCCALCSIZE:
        if (wp == TRUE)
        {
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            const LONG originalTop = params->rgrc[0].top;
            const LRESULT dwp = DefWindowProcW(hwnd, WM_NCCALCSIZE, wp, lp);
            if (dwp != 0) return dwp;
            // A minimized window's proposed client rect is degenerate
            // (bottom - top == 0). The caption reclaim below (top += 1 / top +=
            // frameY) would invert it to a NEGATIVE-height client rect —
            // GetClientRect then reports win=0x-1, which zeroes the D3D9
            // backbuffer and drives the React viewport layout degenerate,
            // stalling the headless --record-minimized capture (#509, a #508
            // regression). Leave DefWindowProc's (0-height, non-inverted) rect
            // as-is while iconic; a later positive-size WM_SIZE re-seeds the
            // client size on restore. (The compositor/WebView sinks apply the
            // same non-positive-size policy at their own sites — the WebView2
            // setup seeds and the WM_SIZE / ResizeWebViewToClient sinks.)
            if (IsIconic(hwnd)) return 0;
            // Reclaim ONLY the caption/top into the client (removes the native
            // title bar); keep the L/R/bottom frame DefWindowProc computed.
            params->rgrc[0].top = originalTop;
            const UINT dpi = GetDpiForWindow(hwnd);
            const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                             + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            if (IsZoomed(hwnd))
            {
                // Maximized: add the frame inset so the client doesn't spill into
                // the invisible overhang; keep a sliver on an auto-hide taskbar.
                params->rgrc[0].top += frameY;
                InsetForAutoHideTaskbar(params->rgrc[0]);
            }
            else
            {
                params->rgrc[0].top += 1;   // 1px top keeps the resize/shadow line
            }
            return 0;
        }
        break;

    case WM_NCHITTEST:
    {
        LRESULT dwmHit = 0;
        if (DwmDefWindowProc(hwnd, msg, wp, lp, &dwmHit)) return dwmHit;

        const POINT screenPt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT wr; GetWindowRect(hwnd, &wr);
        const UINT dpi = GetDpiForWindow(hwnd);
        const int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

        // Resize edges/corners take priority over the drag band — the top ~frameY
        // is a resize grip even though it overlaps the title bar. Not when maximized.
        if (!IsZoomed(hwnd))
        {
            if (screenPt.y < wr.top + frameY)
            {
                if (screenPt.x < wr.left + frameX)   return HTTOPLEFT;
                if (screenPt.x >= wr.right - frameX)  return HTTOPRIGHT;
                return HTTOP;
            }
            if (screenPt.y >= wr.bottom - frameY)
            {
                if (screenPt.x < wr.left + frameX)   return HTBOTTOMLEFT;
                if (screenPt.x >= wr.right - frameX)  return HTBOTTOMRIGHT;
                return HTBOTTOM;
            }
            if (screenPt.x < wr.left + frameX)   return HTLEFT;
            if (screenPt.x >= wr.right - frameX)  return HTRIGHT;
        }

        // Ask WebView2 whether this pixel is the web title bar's caption region.
        POINT clientPt = screenPt; ScreenToClient(hwnd, &clientPt);
        if (m_ncRegionEnabled && m_compositionController4)
        {
            COREWEBVIEW2_NON_CLIENT_REGION_KIND kind = COREWEBVIEW2_NON_CLIENT_REGION_KIND_CLIENT;
            if (SUCCEEDED(m_compositionController4->GetNonClientRegionAtPoint(clientPt, &kind)))
                return kind == COREWEBVIEW2_NON_CLIENT_REGION_KIND_CAPTION ? HTCAPTION : HTCLIENT;
        }
        // Fallback (WebView2 Runtime lacks non-client support): the fixed 34px
        // TitleBar strip minus the 3×46px controls on the right is the caption.
        {
            const int stripH = MulDiv(34, dpi, 96);
            const int controlsW = MulDiv(46 * 3, dpi, 96);
            RECT cr; GetClientRect(hwnd, &cr);
            if (clientPt.y >= 0 && clientPt.y < stripH && clientPt.x < cr.right - controlsW)
                return HTCAPTION;
        }
        return HTCLIENT;
    }

    // Frameless title bar: right-click the caption → the window system menu.
    // Alt+Space works via DefWindowProc, but a custom frame doesn't get the
    // right-click menu for free, so show it explicitly at the cursor and route
    // the chosen command back through WM_SYSCOMMAND (min/max/restore/move/close).
    case WM_NCRBUTTONUP:
        if (wp == HTCAPTION)
        {
            if (HMENU sysMenu = GetSystemMenu(hwnd, FALSE))
            {
                const int cmd = TrackPopupMenu(
                    sysMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                    GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0, hwnd, nullptr);
                if (cmd) PostMessage(hwnd, WM_SYSCOMMAND, static_cast<WPARAM>(cmd), 0);
            }
            return 0;
        }
        break;

    case WM_SIZE:
        ResizeWebViewToClient();
        // The DComp tree's root visual clip
        // needs to track the host client size or chrome gets clipped on
        // resize.
        if (m_compositor && m_compositor->IsReady())
        {
            RECT r;
            GetClientRect(hwnd, &r);
            // A minimized/degenerate client rect (#509: WM_NCCALCSIZE could even
            // yield a NEGATIVE height) must not reach the compositor — a 0- or
            // negative-area SetSize corrupts the surface. Same non-positive-size
            // policy as ResizeWebViewToClient's put_Bounds sink; a later
            // positive-size WM_SIZE re-seeds on restore.
            if (!IsIconic(hwnd) && r.right - r.left > 0 && r.bottom - r.top > 0)
                m_compositor->SetSize(r.right - r.left, r.bottom - r.top);
        }
        // Frameless title bar: sync the maximize↔restore glyph, DEDUPED — WM_SIZE
        // sends SIZE_RESTORED on every resize-drag tick, so a raw emit would flood
        // the bridge with {maximized:false}. Skip minimize (glyph doesn't change).
        if (wp != SIZE_MINIMIZED)
            EmitWindowStateIfChanged();
        return 0;

    // [resize-perf] During the modal sizemove loop DefWindowProc
    // erases the full client with the class brush on every tick (the
    // main class registers CS_HREDRAW|CS_VREDRAW) — pure GDI cost:
    // WebView2 repaints the whole client continuously anyway. Suppress
    // only while in sizemove; normal paints keep the dark theme brush
    // (first-paint / expose flashes are the reason it exists).
    case WM_ERASEBKGND:
        if (m_inSizeMove) return 1;
        break;  // DefWindowProc fills with the class brush as today

    // Host HWND gained focus
    // (initial show, Alt-Tab back, click into the window). Forward
    // logical keyboard focus to WebView2 so its DOM event chain
    // sees WM_KEY*/WM_IME_*. Without this, after Alt-Tab away and
    // back the host owns focus, WebView2 doesn't, and keyboard
    // silently breaks until the next mouse click happens to
    // re-trigger something.
    case WM_SETFOCUS:
        if (webController)
        {
            webController->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        }
        break;  // fall through so DefWindowProc sees it too

    // DPI changed (window moved to a
    // monitor with different DPI). HIWORD(wp) is the new system DPI;
    // lp points to a suggested RECT in screen coords. Update the
    // composition controller's rasterization scale so chrome
    // re-rasterises crisp at the new DPI, then resize/reposition
    // the host HWND to Windows's suggested rect (recommended
    // per-monitor-v2 best practice).
    case WM_DPICHANGED:
        // In --record we pin RasterizationScale to the timeline's `scale` (see the
        // record branch); don't let a stray DPI-change clobber it back to monitor DPI.
        if (m_compositionController && !m_recordMode)
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

    // Async composition setup failed (controller completion / QI / shared
    // setup). Composition is a hard requirement — there is no HWND fallback —
    // so surface a clear error and exit. wParam is the original failure
    // HRESULT. PostMessage'd from OnCompositionControllerReady so the WebView2
    // callback unwinds before we tear down (the modal + exit happens here on
    // the message-loop thread, off the callback stack).
    case WM_APP_COMPOSITION_FALLBACK:
        FailFatalComposition(static_cast<HRESULT>(wp));   // [[noreturn]]

    // Cursor sync. Under composition the
    // host HWND owns WM_SETCURSOR; consult the cached cursor that
    // the composition controller's add_CursorChanged handler last
    // delivered. Returning TRUE tells Windows we set the cursor
    // ourselves — skip default class-arrow behaviour.
    case WM_SETCURSOR:
        if (m_webViewCursor && LOWORD(lp) == HTCLIENT)
        {
            SetCursor(m_webViewCursor);
            return TRUE;
        }
        break;

    // Forward mouse input to WebView2's
    // composition controller. The host owns input and forwards via
    // SendMouseInput.
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_MOUSEWHEEL:  case WM_MOUSEHWHEEL:
        if (m_compositionController)
        {
            // Arm TME_LEAVE on each fresh WM_MOUSEMOVE
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

    // Forward COREWEBVIEW2_MOUSE_EVENT_KIND_MOUSE_LEAVE
    // when the pointer exits the host HWND so WebView2 clears CSS :hover
    // state and the cursor. WM_MOUSELEAVE's wp/lp don't carry coords or
    // virtual-key state — use POINT{-1, -1} per WebView2 docs.
    case WM_MOUSELEAVE:
        m_mouseTracked = false;
        if (m_compositionController)
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
        // When main moves, the viewport popup follows. Position
        // changes only — size stays cached.
        layout.RefreshScreenPosition();
        return 0;

    case WM_DISPLAYCHANGE:
        // [E5] Display mode changed (resolution/refresh-rate switch, monitor
        // hot-plug): re-derive the pacing budget. DefWindowProc continues.
        UpdatePacingBudget(hwnd);
        break;

    case WM_APP_PREVIEW_READY:
        // [C3] Background preview encode finished — cache + notify the web
        // (BridgeDispatcher::DrainPreviewResults emits textures/preview-ready).
        if (dispatcher) dispatcher->DrainPreviewResults();
        return 0;

    case WM_WINDOWPOSCHANGED:
        // WM_WINDOWPOSCHANGED fires for every position/
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
        //
        // [resize-perf] PredictAndApply's per-tick reset
        // runs on the cheap ResetEx path (~3-5 ms — textures/shaders
        // persist per D3D9Ex semantics; only size-keyed RTs rebuild),
        // so the scene renders at the CORRECT size every tick — no
        // deferred-settle snap. RenderD3D9 stays the modal-loop frame
        // driver (the idle pump is starved in here). The
        // kResizeSettleTimerId one-shot is a safety net that re-resets
        // only if a mid-gesture reset failed.
        // [E5] A move can land the window on a different monitor —
        // re-derive the pacing budget from that monitor's refresh rate.
        if (MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY) != m_pacingMonitor)
            UpdatePacingBudget(hwnd);
        // [C1] Position-only ticks (window drags: SWP_NOSIZE set) skip the
        // predict/render chain — the client extent is unchanged, so
        // PredictAndApply would early-out into RefreshScreenPosition anyway,
        // and the unconditional RenderD3D9 was pure extra work on top of the
        // paced idle loop (one wasted render per drag tick). The popup still
        // tracks the move. SWP_FRAMECHANGED is excluded: a non-client recalc
        // can change the CLIENT extent even under SWP_NOSIZE (window rect
        // unchanged), so those ticks keep the full predict/render path.
        if (hViewport && lp != 0
            && (reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_NOSIZE)
            && !(reinterpret_cast<const WINDOWPOS*>(lp)->flags & SWP_FRAMECHANGED))
        {
            layout.RefreshScreenPosition();
            break;  // DefWindowProc still generates WM_MOVE etc.
        }
        if (hViewport)
        {
            // [resize-perf] time the per-tick chain and
            // emit a 1 Hz aggregate with the engine's reset sub-stage
            // breakdown (cheap = ResetForResize successes).
            const LONGLONG rpT0 = PerfQpcNow();
            layout.PredictAndApply();
            RenderD3D9();
            perfWmpos.add(PerfUsSince(rpT0));

            if (m_inSizeMove)
                SetTimer(hwnd, kResizeSettleTimerId, kResizeSettleDelayMs, nullptr);

            const DWORD rpNow = GetTickCount();
            if (perfWmposLastEmit == 0 || (rpNow - perfWmposLastEmit) >= 1000)
            {
                if (engine)
                {
                    const Engine::ResetPerf& rp = engine->GetResetPerf();
                    Log("[resize-perf] wmpos: ticks=%u apply+render(ms av/mx)=%.1f/%.1f "
                        "resets=%u (cheap-total=%u) last(ms tot=%.1f lost=%.1f dev=%.1f reload=%.1f alpha=%.1f)\n",
                        perfWmpos.n,
                        perfWmpos.avg() / 1000.0, perfWmpos.maxUs / 1000.0,
                        rp.count - perfWmposResetBase, rp.cheapCount,
                        rp.lastTotalMs, rp.lastLostMs, rp.lastDeviceResetMs,
                        rp.lastReloadMs, rp.lastAlphaResizeMs);
                    perfWmposResetBase = rp.count;
                }
                perfWmpos.reset();
                perfWmposLastEmit = rpNow;
            }
        }
        break;  // fall through so DefWindowProc continues processing

    // During the modal sizemove loop, WM_SIZE/WM_MOVE
    // fire continuously. Each one calls RefreshScreenPosition so
    // the popup tracks main's new position. The cached client-coord
    // rect from the last layout/viewport-rect message is the source
    // — React's ResizeObserver will fire AFTER the sizemove loop
    // exits, sending a fresh layout/viewport-rect, but in the
    // meantime the popup at least stays anchored to roughly the
    // right place via owner-client translation. (An earlier design
    // note rejected HIDING the popup during sizemove — that exposes
    // the bare WebView2 transparent region, which paints white. The
    // resize-settle handlers below don't hide anything; they only defer the
    // per-tick engine reset.)

    // [resize-perf] Modal sizemove bracket. m_inSizeMove
    // gates the WM_ERASEBKGND suppression below; per-tick engine resets
    // now run unconditionally on the cheap ResetEx path (LayoutBroker::
    // ResetEngineForResize), so EXITSIZEMOVE's settle is a no-op safety
    // net that only acts if a mid-gesture reset FAILED. Both fall
    // through to DefWindowProc, which runs its own modal-loop
    // bookkeeping on these messages.
    case WM_ENTERSIZEMOVE:
        m_inSizeMove = true;
        break;

    case WM_EXITSIZEMOVE:
        m_inSizeMove = false;
        KillTimer(hwnd, kResizeSettleTimerId);
        SettleResize("exitsizemove");
        break;

    case WM_CLOSE:
        // Data-loss BLOCKER: the native frame-X / Alt-F4 used to fall
        // straight to DefWindowProc → WM_DESTROY, which deletes the recovery
        // autosave — silently destroying unsaved work AND its safety net. Route
        // a dirty interactive session to the SAME React Save/Discard/Cancel
        // prompt File→Exit uses; swallow the default destroy until React replies
        // (it dispatches app/quit → WM_APP_QUIT_CONFIRMED below).
        if (ShouldVetoClose(dispatcher && dispatcher->GetDirty(), m_automationMode, useTestHost))
        {
            if (dispatcher) dispatcher->EmitCloseRequested();
            return 0;
        }
        break;   // not dirty (or headless) → DefWindowProc → WM_DESTROY

    case WM_APP_QUIT_CONFIRMED:
        // React confirmed the close (saved or discarded). DestroyWindow
        // → WM_DESTROY (NOT WM_CLOSE), so a confirmed quit never re-enters the
        // veto above.
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, kStatsTimerId);
        // Stop autosave + delete THIS session's autosave files on a
        // clean exit so no orphan prompts on the next launch. A crash skips
        // WM_DESTROY, leaving the orphan for recovery — exactly the point.
        if (!useTestHost && !m_automationMode)
        {
            KillTimer(hwnd, Autosave::RECENT_TIMER_ID);
            KillTimer(hwnd, Autosave::STABLE_TIMER_ID);
            Autosave::DeleteOurSession();
        }
        // Release the class background brush. Per
        // WNDCLASSEX docs the system would free it on UnregisterClass,
        // but the class is never explicitly unregistered. Doing it
        // here is safe for the single-window-per-process host.
        if (m_classBrush)
        {
            DeleteObject(m_classBrush);
            m_classBrush = nullptr;
        }
        // Unregister the WebMessageReceived handler
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
        // Unsubscribe the nav/new-window/permission handlers
        // before webView teardown, same rationale as the WebMessageReceived removal above
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
            if (navCompletedTok.value != 0)
            {
                webView->remove_NavigationCompleted(navCompletedTok);
                navCompletedTok = {};
            }
            if (permissionTok.value != 0)
            {
                webView->remove_PermissionRequested(permissionTok);
                permissionTok = {};
            }
            if (docTitleTok.value != 0)
            {
                webView->remove_DocumentTitleChanged(docTitleTok);
                docTitleTok = {};
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
        // Release composition controller +
        // DComp tree. Order matters per dxgi_spike.cpp:783-818:
        // controller is released AFTER webController->Close() (which
        // already settles WebView2's pending work) and BEFORE
        // m_compositor.reset() (so the Compositor's defensive
        // put_RootVisualTarget(nullptr) in its dtor still has a live
        // controller via its internal Impl::controller ComPtr — the
        // Compositor holds its own reference). m_compositor.reset()
        // then releases the visual tree.
        //
        // Unregister the CursorChanged handler before
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
        // Clear LayoutBroker's pointer BEFORE
        // releasing the Compositor so any late SetSceneRect dispatch
        // (e.g. an in-flight BridgeDispatcher message that's already
        // past the WM_DESTROY barrier in the message-pump shutdown
        // sequence) doesn't dereference a freed Compositor.
        layout.SetCompositor(nullptr);
        m_compositor.reset();
        // Detach the compositor from Engine BEFORE either is
        // destroyed so Render() (if scheduled before WM_QUIT drains
        // the queue) can't dereference a freed compositor. Drop the
        // compositor first since Engine owns the D3D9 device the
        // compositor's resources are bound to.
        // Drop the InputDispatcher before the engine /
        // compositor. It holds the viewport popup HWND raw; the popup
        // itself is destroyed below as part of the standard WM_DESTROY
        // cleanup.
        m_inputDispatcher.reset();
        if (engine) engine->SetAlphaCompositor(nullptr);
        layout.SetAlphaCompositor(nullptr);
        alphaCompositor.reset();
        // engine owns its D3D9 device; just drop the engine and it
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
        // rendering happens on the main-loop idle path
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
    // Mirrors the legacy handler in the legacy main.cpp. The math
    // for MOVE / ROTATE / ZOOM is lifted verbatim from legacy so the
    // user's muscle-memory carries over: drag delta scales /2.0f for
    // rotate (full-window-width drag ≈ 180°), distance/1000 for
    // MOVE multiplier, sqrt(olddist)-based scaling for ZOOM.
    //
    // Scope: camera only. The shift-click-to-spawn path
    // (legacy 2956) depends on the MouseCursor Object3D port and is
    // explicitly deferred. The status-bar mouse-coord push
    // (legacy 3041) is a polish item.
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
        // Smoke instrumentation — verify what wParam
        // actually arrived from the synthesized PostMessage.
        Log("[ArchC-engine] WM_LBUTTONDOWN wp=0x%llx MK_SHIFT=%d MK_CONTROL=%d hasPS=%d emitters=%zu attached=%d\n",
            static_cast<unsigned long long>(wp),
            (wp & MK_SHIFT) ? 1 : 0,
            (wp & MK_CONTROL) ? 1 : 0,
            particleSystem ? 1 : 0,
            particleSystem ? particleSystem->getEmitters().size() : 0,
            LiveAttachedSystem() ? 1 : 0);
        // The viewport popup is hidden and WebView2 owns
        // keyboard routing; we forward keystrokes through the bridge. We do
        // NOT SetFocus the hidden popup — that briefly succeeds (visibility
        // isn't a precondition; WS_EX_NOACTIVATE only blocks user-driven
        // activation), then OS focus management snaps it back, firing a
        // spurious WM_KILLFOCUS the defensive kill below would read as "user
        // Alt-Tab'd, drop the spawn."
        // Shift+LMB also triggers cursor-bound spawn. The
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
        // by an earlier WM_KEYDOWN VK_SHIFT or by the fallback below),
        // LMB-down enters OBJECT_Z drag mode for height adjustment. LMB-up
        // will then detach the preview, placing it permanently in the scene.
        // Matches the legacy main.cpp. Do NOT enter a camera drag
        // — placement is the entire intent of this click while a preview
        // is alive.
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            m_dragMode     = DragMode::OBJECT_Z;
            m_dragStartCam = engine->GetCamera();
            m_dragStartX   = (short)LOWORD(lp);
            m_dragStartY   = (short)HIWORD(lp);
            SetCapture(hwnd);
            Log("[ArchC-engine] LMB-down OBJECT_Z drag (placing attached=%p)\n",
                static_cast<void*>(attached));
            return 0;
        }
        // Reference-object manipulator: if a handle (translate
        // arrow or rotate ring) is under the cursor, grab it (drag moves/rotates the
        // object) — this wins over camera orbit AND over the Shift+LMB particle-spawn
        // below (so Shift-clicking a handle is a precise grab, not a spawn).
        // PickManipulatorHandle returns NONE unless the object is selected, so a MISS
        // (including empty-space Shift-clicks) falls through to the spawn / camera path.
        {
            const Engine::ManipHandle h =
                engine->PickManipulatorHandle((short)LOWORD(lp), (short)HIWORD(lp));
            if (h.kind != Engine::ManipHandle::NONE)
            {
                m_manipKind     = h.kind;
                m_manipAxis     = h.axis;
                m_manipStartPos = engine->GetReferencePosition();
                m_manipStartRot = engine->GetReferenceRotation();
                if (h.kind == Engine::ManipHandle::TRANSLATE)
                {
                    // Grab offset (axis param at press) so the object doesn't jump
                    // on the first move; if degenerate, fall back to 0.
                    if (!engine->ManipulatorAxisParam((short)LOWORD(lp), (short)HIWORD(lp),
                                                      h.axis, m_manipStartPos, m_manipGrabT0))
                        m_manipGrabT0 = 0.0f;
                    m_manipPrevT  = m_manipGrabT0;   // seed accumulate-per-move (first move delta = 0 -> no jump)
                    m_manipAccumT = 0.0f;
                }
                else if (h.kind == Engine::ManipHandle::PLANE)
                {
                    // Seed prev from the in-plane offset at press so the first move
                    // delta is 0 (no jump); accumulators start at 0. Anchor to the FIXED
                    // grab position (m_manipStartPos) for the whole drag -- see below.
                    if (!engine->ManipulatorPlaneOffset((short)LOWORD(lp), (short)HIWORD(lp),
                                                        h.axis, m_manipStartPos, m_manipPrevU, m_manipPrevV))
                    { m_manipPrevU = 0.0f; m_manipPrevV = 0.0f; }
                    m_manipAccumU = 0.0f;
                    m_manipAccumV = 0.0f;
                }
                else   // ROTATE
                {
                    // Grab angle on the ring; the accumulator starts at 0 so the
                    // first move is a no-op delta (no jump). If degenerate, the
                    // first valid move re-seeds prev (accum stays 0 until then).
                    if (!engine->ManipulatorRingAngle((short)LOWORD(lp), (short)HIWORD(lp),
                                                      h.axis, m_manipGrabAngle))
                        m_manipGrabAngle = 0.0f;
                    m_manipPrevAngle  = m_manipGrabAngle;
                    m_manipAccumAngle = 0.0f;
                }
                // Tell the engine which handle is being dragged (drives the guide line / rotate sweep / dim).
                // Rotate seeds both angles to the grab angle (applied == grab at accum 0); translate uses 0.
                const float grabA = (h.kind == Engine::ManipHandle::ROTATE) ? m_manipGrabAngle : 0.0f;
                if (engine) engine->SetManipulatorActiveDrag(h, grabA, grabA);
                m_dragMode = DragMode::MANIPULATE;
                SetCapture(hwnd);
                return 0;
            }
        }
        // Round 5 fallback: Shift+LMB with no existing preview spawns
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
            // Mirror the cursor-unproject
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
        // Click-to-select: clicking an UNLOCKED object body selects it (the gizmo
        // appears); clicking empty space deselects it and falls through to camera
        // MOVE. A LOCKED object is navigation-transparent — a body hit is NOT
        // consumed, so the click reaches the camera MOVE/ZOOM path below (matching
        // RMB orbit, which never picks). The consume rule lives in the unit-tested
        // RefLockConsumeBodyClick (RefLock.h). (Handle grabs above already won when
        // the object was selected; a locked object is never selected, so none exist.)
        {
            const bool hit = engine->PickReferenceObject((short)LOWORD(lp), (short)HIWORD(lp));
            if (RefLockConsumeBodyClick(hit, engine->IsReferenceLocked()))
            {
                engine->SetReferenceObjectSelected(true);
                return 0;   // consume — don't pan when clicking an unlocked object
            }
            // Miss, or a hit on a LOCKED object: deselect, then fall through to
            // camera MOVE/ZOOM. While locked this is idempotent — selection is
            // already forced false and hover/active manip already cleared.
            engine->SetReferenceObjectSelected(false);
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
        // An RMB press mid-LMB-manipulate-drag takes over the mode;
        // commit the in-flight move first so its persist/dirty isn't dropped.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        }
        m_dragMode     = (wp & MK_CONTROL) ? DragMode::ZOOM : DragMode::ROTATE;
        m_dragStartCam = engine->GetCamera();
        m_dragStartX   = (short)LOWORD(lp);
        m_dragStartY   = (short)HIWORD(lp);
        SetCapture(hwnd);
        // See WM_LBUTTONDOWN — we don't SetFocus the hidden
        // popup, which would trigger the spurious WM_KILLFOCUS → kill loop.
        return 0;
    }
    case WM_LBUTTONUP:
    {
        // Manipulator drag release: commit the moved transform once
        // (gated persist + dirty + emit). Per-move only set+emitted (no persist),
        // so the registry/dirty flag is touched exactly once per gesture.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode  = DragMode::NONE;
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
            ReleaseCapture();
            return 0;
        }
        // Legacy parity: if a cursor-bound preview was being dragged
        // for placement (OBJECT_Z mode, or any state with an attached
        // preview), DETACH it now. After Detach the system stays in
        // the world at its current position and continues to emit —
        // it is no longer parented to m_mouseCursor. The user can
        // then click again (while still holding Shift) to spawn a
        // fresh preview, repeating the click-to-place gesture.
        // Matches the legacy main.cpp.
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-engine] LMB-up placing attached=%p (Detach, system stays alive)\n",
                static_cast<void*>(attached));
            engine->DetachParticleSystem(attached);
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
        // If a manipulator drag was interrupted, commit its current
        // position so the move isn't silently lost (the engine already holds the
        // last per-move position; CommitReferenceObjectTransform persists it).
        if (m_dragMode == DragMode::MANIPULATE && dispatcher)
            dispatcher->CommitReferenceObjectTransform();
        // Drop drag state so the next mouse-move doesn't ride a stale start camera
        // / grabbed axis.
        m_dragMode  = DragMode::NONE;
        ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!engine) return 0;

        int mx = (short)LOWORD(lp);
        int my = (short)HIWORD(lp);
        m_lastCursorX = mx;
        m_lastCursorY = my;

        // Manipulator drag. TRANSLATE: accumulate precision-
        // scaled per-move axis-param deltas (m_manipAccumT += (now - prev) * factor)
        // and apply newPos = startPos + axis*m_manipAccumT — with factor==1 this
        // telescopes to (now - grab), i.e. the old absolute-from-grab; factor=0.2 while
        // Shift is held gives a finer drag with no jump on toggle. ROTATE: accumulate
        // wrapped, precision-scaled per-move ring-angle deltas onto the snapshot
        // rotation's Euler component for that ring (no jump, multi-turn). When
        // engine->GetSnapEnabled(), TRANSLATE rounds X/Y to the grid spacing (Z/height
        // free) and ROTATE rounds the driven Euler component to 15° (both finer by the
        // same factor while Shift held). Only set + emit here so the picker spinners
        // track live; persistence is deferred to LMB-up.
        if (m_dragMode == DragMode::MANIPULATE && m_manipAxis >= 0)
        {
            // [gizmo-drag-teardown] A drag continues only while the object is still
            // selected AND unlocked. An out-of-band clear / mod-switch / new-file /
            // deselect / lock deselects it (lock deselects too), so self-abort here
            // before reading or applying any move — we must never drag a stale/gone/
            // frozen object. Fully end the gesture like the per-site drag-end tail, but
            // WITHOUT committing (we must not persist a stale transform): ResetManipDragState
            // zeroes accumulators + clears the engine active-drag guides; clear m_dragMode +
            // ReleaseCapture so the eventual LBUTTONUP doesn't commit a phantom dirty/registry
            // write. Set m_dragMode=NONE BEFORE ReleaseCapture so the WM_CAPTURECHANGED it posts
            // sees NONE and no-ops. Reuses the unit-tested freeze/lock predicate (RefLock.h).
            if (!RefLockResolveSelected(engine->IsReferenceObjectSelected(),
                                        engine->IsReferenceLocked()))
            {
                ResetManipDragState();
                m_dragMode = DragMode::NONE;
                ReleaseCapture();
                return 0;
            }
            const float factor = (wp & MK_SHIFT) ? 0.2f : 1.0f;   // read wParam, NOT GetKeyState
            bool moved = false;
            if (m_manipKind == Engine::ManipHandle::TRANSLATE)
            {
                float tNow;
                if (engine->ManipulatorAxisParam((short)mx, (short)my, m_manipAxis,
                                                 m_manipStartPos, tNow))
                {
                    m_manipAccumT += (tNow - m_manipPrevT) * factor;   // precision-scaled per-move delta
                    m_manipPrevT   = tNow;
                    const D3DXVECTOR3 ax(m_manipAxis == 0 ? 1.0f : 0.0f,
                                         m_manipAxis == 1 ? 1.0f : 0.0f,
                                         m_manipAxis == 2 ? 1.0f : 0.0f);
                    D3DXVECTOR3 newPos = m_manipStartPos + ax * m_manipAccumT;   // note: NOT const now
                    if (engine->GetSnapEnabled())                  // snap X/Y to grid; Z (height) free
                    {
                        const float step = engine->GetGridSpacing() * factor;    // finer step when Shift held
                        if (step > 0.0f) { newPos.x = roundf(newPos.x / step) * step; newPos.y = roundf(newPos.y / step) * step; }
                    }
                    // Capture the pre-drag transform ONCE, on the
                    // first move that ACTUALLY changes the position, BEFORE
                    // mutating — the engine still holds the grab-time transform
                    // here, so this is the PRE state. Gating on a real change
                    // (not just a successful projection) avoids a phantom undo
                    // step from a zero-delta first move — notably under snap,
                    // where a tiny move can round back to the grab point.
                    if (!m_manipUndoCaptured && newPos != m_manipStartPos) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
                    }
                    engine->SetReferenceObjectTransform(newPos, m_manipStartRot);
                    m_readoutKind = "translate";
                    m_readoutLabels[0] = manipreadout::AxisName(m_manipAxis);
                    m_readoutValues[0] = (&newPos.x)[m_manipAxis];
                    m_readoutN = 1; m_readoutDecimals = 1;
                    moved = true;
                }
            }
            else if (m_manipKind == Engine::ManipHandle::PLANE)
            {
                float uNow, vNow;
                // Anchor to the FIXED grab position (m_manipStartPos), NOT the live object
                // origin -- decomposing against the moving object fed its own motion back
                // into the delta and oscillated the position (mirrors the arrow's
                // ManipulatorAxisParam, which also anchors to m_manipStartPos).
                if (engine->ManipulatorPlaneOffset((short)mx, (short)my, m_manipAxis, m_manipStartPos, uNow, vNow))
                {
                    m_manipAccumU += (uNow - m_manipPrevU) * factor;   // precision-scaled per-move
                    m_manipAccumV += (vNow - m_manipPrevV) * factor;
                    m_manipPrevU = uNow;  m_manipPrevV = vNow;
                    // Compose via the unit-tested pure helper (basis = (normal+1,normal+2);
                    // ground normal 2 -> (X,Y); Z stays == start, structurally).
                    float np[3];
                    planehandle::ComposePlanePos(&m_manipStartPos.x, m_manipAxis,
                                                 m_manipAccumU, m_manipAccumV, np);
                    D3DXVECTOR3 newPos(np[0], np[1], np[2]);
                    if (engine->GetSnapEnabled())
                    {
                        // NOTE: snapping .x/.y is correct ONLY for the ground (normal-Z)
                        // plane, whose free axes ARE X and Y. A future YZ/ZX plane would
                        // need to snap ITS in-plane components -- revisit snap when adding
                        // those (YZ/ZX deferred).
                        const float step = engine->GetGridSpacing() * factor;
                        if (step > 0.0f) { newPos.x = roundf(newPos.x / step) * step;
                                           newPos.y = roundf(newPos.y / step) * step; }
                    }
                    if (!m_manipUndoCaptured && newPos != m_manipStartPos) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
#ifndef NDEBUG
                        Log("[Plane] grab-capture axis=%d accumUV=(%.3f,%.3f) newPos=(%.3f,%.3f,%.3f)\n",
                            m_manipAxis, m_manipAccumU, m_manipAccumV, newPos.x, newPos.y, newPos.z);
#endif
                    }
                    engine->SetReferenceObjectTransform(newPos, m_manipStartRot);
                    { int pu, pv; manipreadout::InPlaneAxes(m_manipAxis, pu, pv);
                      m_readoutKind = "plane";
                      m_readoutLabels[0] = manipreadout::AxisName(pu); m_readoutValues[0] = (&newPos.x)[pu];
                      m_readoutLabels[1] = manipreadout::AxisName(pv); m_readoutValues[1] = (&newPos.x)[pv];
                      m_readoutN = 2; m_readoutDecimals = 1; }
                    // active-drag (which drives the dim + faint X/Y guides) was set once at
                    // grab and never changes for a plane drag -- no per-move re-set needed
                    // (matches the TRANSLATE branch; only ROTATE re-sets, to feed live angles).
                    moved = true;
                }
            }
            else   // ROTATE
            {
                float aNow;
                if (engine->ManipulatorRingAngle((short)mx, (short)my, m_manipAxis, aNow))
                {
                    auto wrapPi = [](float a) {
                        const float twoPi = 2.0f * D3DX_PI;
                        while (a >   D3DX_PI) a -= twoPi;
                        while (a <= -D3DX_PI) a += twoPi;
                        return a;
                    };
                    m_manipAccumAngle += wrapPi(aNow - m_manipPrevAngle) * factor;   // precision
                    m_manipPrevAngle   = aNow;
                    // Euler component this ring drives (m_referenceRotation = [yaw=Z,
                    // pitch=X, roll=Y]): ring axis 2(Z)->yaw(.x), 0(X)->pitch(.y),
                    // 1(Y)->roll(.z).
                    const int comp = (m_manipAxis == 2) ? 0 : (m_manipAxis == 0) ? 1 : 2;
                    D3DXVECTOR3 newRot = m_manipStartRot;
                    (&newRot.x)[comp] += m_manipAccumAngle * (180.0f / D3DX_PI);
                    if (engine->GetSnapEnabled())                  // snap rotation to 15deg (3deg w/ Shift)
                    {
                        const float s = 15.0f * factor;
                        (&newRot.x)[comp] = roundf((&newRot.x)[comp] / s) * s;
                    }
                    // Capture the pre-drag transform ONCE, on the
                    // first move that ACTUALLY changes the rotation, BEFORE
                    // mutating (engine still at the grab-time transform → PRE
                    // state). Gating on a real change avoids a phantom undo step
                    // from a zero-delta first move (e.g. snap rounding back).
                    if (!m_manipUndoCaptured && newRot != m_manipStartRot) {
                        if (dispatcher) dispatcher->CaptureReferenceTransformUndoPoint();
                        m_manipUndoCaptured = true;
                    }
                    engine->SetReferenceObjectTransform(m_manipStartPos, newRot);
                    m_readoutKind = "rotate";
                    // Label = the WORLD AXIS the ring spins about (X/Y/Z); the value is the
                    // rotation about that axis = the Euler component RingComp(axis) selects.
                    m_readoutLabels[0] = manipreadout::AxisName(m_manipAxis);
                    m_readoutValues[0] = (&newRot.x)[comp];
                    m_readoutN = 1; m_readoutDecimals = 0;
                    // Push the active-drag AFTER snap so the rotate sweep's "applied" radial
                    // tracks the orientation the object ACTUALLY shows (snapped / precision-scaled),
                    // not the raw accumulator -- under snap the two would diverge by up to the snap
                    // step. Derive the applied angle from the final Euler delta in this ring's plane;
                    // with snap off this reduces to grab + m_manipAccumAngle (unchanged behavior).
                    Engine::ManipHandle activeH; activeH.kind = m_manipKind; activeH.axis = m_manipAxis;
                    const float appliedRad = m_manipGrabAngle
                        + ((&newRot.x)[comp] - (&m_manipStartRot.x)[comp]) * (D3DX_PI / 180.0f);
                    if (engine) engine->SetManipulatorActiveDrag(activeH, m_manipGrabAngle, appliedRad);
                    moved = true;
                }
            }
            // Throttle the (heavy) full-snapshot emit to ~30 Hz so the picker spinners
            // track without flooding the bridge; the final exact transform emits on
            // release (commit).
            if (moved)
            {
                const DWORD now = GetTickCount();
                if (dispatcher && (now - m_lastManipEmitTick) >= 33)
                {
                    dispatcher->EmitEngineStateChanged();
                    // readout pill: project the gizmo origin to normalized
                    // viewport coords, emit the live value. Hidden when no scene
                    // viewport yet (cold boot) or behind camera (visible:false).
                    int vx, vy, vw, vh;
                    manipreadout::ViewportPoint vp{0,0,false};
                    if (engine->GetSceneViewport(vx, vy, vw, vh))
                        vp = manipreadout::ProjectToViewport(engine->GetReferencePosition(),
                                                             engine->GetViewProjection(), vw, vh);
                    nlohmann::json vals = nlohmann::json::array(), labels = nlohmann::json::array();
                    for (int i = 0; i < m_readoutN; ++i) { vals.push_back(m_readoutValues[i]); labels.push_back(m_readoutLabels[i]); }
                    dispatcher->EmitManipulatorDrag({
                        {"active", true}, {"kind", m_readoutKind},
                        {"nx", vp.nx}, {"ny", vp.ny}, {"visible", vp.visible},
                        {"labels", labels}, {"values", vals}, {"decimals", m_readoutDecimals},
                    });
#ifndef NDEBUG
                    printf("[readout] kind=%s nx=%.3f ny=%.3f vis=%d v0=%.2f\n",
                           m_readoutKind.c_str(), vp.nx, vp.ny, (int)vp.visible, m_readoutValues[0]);
#endif
                    m_lastManipEmitTick = now;
                }
            }
            return 0;
        }

        // Hover feedback: when idle (not dragging), highlight the
        // handle under the cursor. PickManipulatorHandle returns NONE unless an object
        // is selected, so this is a cheap no-op when there's nothing to hover.
        if (m_dragMode == DragMode::NONE)
            engine->SetManipulatorHover(engine->PickManipulatorHandle((short)mx, (short)my));

        // Legacy parity: in OBJECT_Z drag (placing a cursor-bound preview),
        // only Z tracks the drag. X/Y stay frozen at the click position so
        // the user can rake the mouse vertically to set height without the
        // preview sliding sideways. Matches the legacy main.cpp.
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
        // of (non-OBJECT_Z) drag mode. Mirrors the legacy main.cpp
        // — without this, the attached ParticleSystemInstance (parented to
        // m_mouseCursor via Object3D) wouldn't track the mouse during
        // Shift-hold. Cache the (x,y) so WM_KEYDOWN can use it for the
        // spawn coords (WM_KEYDOWN's lParam is NOT mouse coords; a
        // legacy main.cpp bug).
        D3DXVECTOR3 cursorWorld;
        GetCursorPos3D(engine.get(), (short)mx, (short)my, cursorWorld);
        m_mouseCursor.SetPosition(cursorWorld);

        // Push the world-space cursor to the React
        // status bar, throttled. 33 ms ≈ 30 Hz — fast enough to read,
        // slow enough that the bridge channel doesn't bottleneck.
        const DWORD now = GetTickCount();
        if (dispatcher && (now - m_lastCursorEmitTick) >= 33u)
        {
            m_lastCursorEmitTick = now;
            dispatcher->EmitCursorPosition3D(cursorWorld.x, cursorWorld.y, cursorWorld.z);
#ifndef NDEBUG
            // Throttled diagnostic for the
            // cursor-unproject path. Piggybacks on the bridge-emit gate
            // so the cadence is ~30 Hz (rather than per-WM_MOUSEMOVE,
            // which is 60+ Hz and would flood host.log). `mode` names
            // which viewport GetCursorPos3D used — `scene` under
            // composition mode, `full-rt` under legacy
            // mode (or pre-scene-rect-dispatch boot).
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
    // it around; release Shift to kill it. Matches the legacy main.cpp.
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
        if (LiveAttachedSystem() != nullptr) return 0;
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
        // One-shot diagnostic at the actual
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
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_KEYUP VK_SHIFT killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(attached);
            m_attachedParticleSystem = nullptr;
        }
        return 0;
    }
    case WM_KILLFOCUS:
    {
        // End an in-flight gizmo drag on focus loss (archC routes Alt-Tab here as window.blur;
        // WM_CAPTURECHANGED may not fire for the hidden popup). Commit the moved transform so it isn't
        // lost, then clear drag + active-guide state. A spurious archC focus-churn mid-drag would also end
        // the drag, but commit preserves the position (accepted tradeoff -- a captured drag rarely churns).
        if (m_dragMode == DragMode::MANIPULATE) {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode  = DragMode::NONE;
            ResetManipDragState();          // drop handle + zero accumulators + clear active-drag
        }
        // Defensive: if the viewport loses focus while Shift is held
        // (Alt-Tab away, foreign focus steal), WM_KEYUP may never arrive
        // and the attached instance leaks. Drop it here.
        //
        // The viewport popup is hidden and never genuinely
        // owns focus, but receives spurious WM_KILLFOCUS from Win32 focus
        // churn whenever ANY focus assignment touches it (other apps
        // activating, modal dialogs, etc.). Treating those as user-Alt-Tab
        // triggers and killing the cursor-bound spawn is a regression, so we
        // suppress the OS-driven kill here. (The legitimate blur case is handled
        // renderer-side: window.blur → viewport/input { type:"blur" } → the
        // private WM_APP_VIEWPORT_BLUR message below, which DOES end the spawn.)
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_KILLFOCUS suppressed (attached=%p preserved)\n",
                static_cast<void*>(attached));
        }
        return 0;
    }
    case WM_APP_VIEWPORT_BLUR:
    {
        // Genuine renderer viewport blur (window.blur via InputDispatcher) -- end
        // any cursor-bound Shift spawn. Distinct from the OS WM_KILLFOCUS above
        // (suppressed for Win32 focus churn) so a real blur can't leak the
        // attached preview (release-audit #7). Tear down an in-flight OBJECT_Z
        // placement drag first: m_dragMode = NONE BEFORE ReleaseCapture (the gizmo
        // teardown order). No-op when nothing is attached / no drag.
        //
        // Preserve the WM_KILLFOCUS MANIPULATE behavior the renderer blur used to
        // trigger (it previously routed through WM_KILLFOCUS): commit an in-flight
        // gizmo drag so its moved transform isn't lost. Idempotent — if the OS
        // WM_KILLFOCUS also fires, whichever runs first sets m_dragMode=NONE and
        // the other skips.
        if (m_dragMode == DragMode::MANIPULATE)
        {
            if (dispatcher) dispatcher->CommitReferenceObjectTransform();
            m_dragMode = DragMode::NONE;
            ResetManipDragState();
        }
        if (m_dragMode == DragMode::OBJECT_Z)
        {
            m_dragMode = DragMode::NONE;
            ReleaseCapture();
        }
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_APP_VIEWPORT_BLUR killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(attached);
            m_attachedParticleSystem = nullptr;
        }
        return 0;
    }
    case WM_DESTROY:
    {
        // Viewport HWND is going away. Defensively drop any attached
        // instance before the Engine tears down (Engine reset happens
        // on the main window's WM_DESTROY which fires after this).
        if (ParticleSystemInstance* attached = LiveAttachedSystem())
        {
            Log("[ArchC-kill] WM_DESTROY killing attached=%p\n",
                static_cast<void*>(attached));
            engine->KillParticleSystem(attached);
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

// Composite-output capture for --capture (and the --snap-window CLI) now lives
// in host/WindowCapture.{h,cpp} — host::CaptureWindowToPng captures the FINAL
// DWM/DirectComposition-composited window (engine RT + WebView2 visual), which
// the engine-RT-only AlphaCompositor::CaptureSnapshotToFile can't see.

// QpcMs and DeriveSibling moved to HostRunUtil.h (shared with CaptureRunner).

} // namespace

// ---------- Run ----------

int HostWindowImpl::Run(int nCmdShow)
{
    OpenLog();

    if (useTestHost)
    {
        Log("[host] === --test-host MODE: CDP on :9222 + DevTools enabled ===\n");
    }

    // when --dev-ui is requested, verify the Vite dev server
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
            // Interactive only (headless has no user to dismiss it; logs + exits).
            if (IsFullyInteractive())
                MessageBoxW(nullptr,
                    L"Dev UI mode requested but no dev server detected at http://localhost:5174.\n\n"
                    L"Did you forget to run `pnpm dev` in `web/apps/editor/`?\n\n"
                    L"Start the dev server in one terminal:\n"
                    L"    cd web/apps/editor\n"
                    L"    pnpm dev\n\n"
                    L"Then relaunch ParticleEditor.exe --dev-ui.",
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

    // verify the WebView2 Evergreen runtime is present before
    // creating any window. If missing the dialog is the only visible artifact.
    if (!WebView2RuntimeInstalled())
    {
        Log("[host] WebView2 runtime not found\n");
        // Interactive only: a headless run has no user to answer the OK/Cancel
        // prompt and would hang on it — it just logs + exits(1) below.
        if (IsFullyInteractive())
        {
            // THIS is the missing-runtime case, not the InitWebView2 failure
            // further down: the pre-flight probe above returns before any
            // WebView2 environment is created, so an offer wired only into that
            // later path would never be reached by the users who need it.
            Log("[host] showing WebView2 install dialog\n");
            OfferWebView2Install(nullptr, nullptr);
        }
        CoUninitialize();
        CloseLog();
        return 1;
    }
    Log("[host] WebView2 runtime detected — proceeding\n");

    // GDI+ init for AlphaCompositor::CaptureSnapshotPng (the
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
    // Paint the parent in the same dark purple as the
    // D3D9 viewport's clear color (engine.cpp m_background default).
    // When the popup is briefly mispositioned during a window resize
    // — the popup tracks main on each WM_SIZE but its cached size
    // lags React's ResizeObserver — the uncovered area paints in
    // dark purple instead of the WebView2 transparent-region's white
    // default. Smoothly indistinguishable from the actual viewport
    // until React resends the rect.
    // Stash the brush so WM_DESTROY can DeleteObject it.
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
        0, kHostWindowClassName, L"Particle Editor",
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

    // Frameless custom title bar: force a WM_NCCALCSIZE re-evaluation so the native
    // caption is dropped immediately (the web TitleBar replaces it), and extend the
    // DWM frame a hair so the window keeps its drop shadow + smooth resize. The
    // {0,0,0,1} margin is the review's shadow-only starting point — DEVICE-VERIFY
    // the shadow (and watch for a 1px top hairline) and tune if needed.
    {
        if (!SetWindowPos(hMain, nullptr, 0, 0, 0, 0,
                          SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
        {
            Log("[frameless] WARN: SWP_FRAMECHANGED failed (err=%lu) — caption may not drop\n", GetLastError());
        }
        MARGINS shadowMargins = { 0, 0, 0, 1 };
        const HRESULT hrDwm = DwmExtendFrameIntoClientArea(hMain, &shadowMargins);
        if (FAILED(hrDwm))
        {
            Log("[frameless] WARN: DwmExtendFrameIntoClientArea failed (hr=0x%08lX) — no drop shadow\n", static_cast<unsigned long>(hrDwm));
        }
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
        std::wstring w = Utf8ToWide(js);
        webView->PostWebMessageAsJson(w.c_str());
    };
    dispatcher = std::make_unique<BridgeDispatcher>(/*engine*/nullptr, layout, accelerator, emitFn,
                                                    /*useTestHost*/useTestHost,
                                                    /*ephemeral*/m_automationMode);
    dispatcher->SetUndoStack(&undoStack);
    dispatcher->SetHostHwnd(hMain);
    // [#510] Throttle the panel-refresh broadcasts during a --record run only
    // (NOT --drive, whose asserts must see every state change) so the driver's
    // rapid host-side edits don't saturate the web + starve the capture/ack loop.
    dispatcher->SetRecordEmitThrottle(!m_recordScriptPath.empty());
    // ModManager is already discovered + restored in the impl
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
    // Legacy parity (DoNewFile started with the default emitter selected):
    // the boot system above was seeded with one root emitter at index 0, so
    // select it. React reads selectedEmitterId from the boot snapshot on mount,
    // so the Inspector + curve panel open populated instead of "Select an emitter…".
    dispatcher->SetSelectedEmitterId(0);
    // Seed the dirty-bit baseline against the freshly-bound boot-state
    // ParticleSystem so Ctrl+Z back to it clears dirty without needing
    // a File → New first. file/new + file/open + file/save re-seed via
    // their own paths.
    dispatcher->ResetSavedBaseline();
    // shift-click-to-spawn: expose the attached-system slot so
    // file/new + file/open can kill any in-flight cursor-bound instance
    // before swapping the ParticleSystem under it.
    dispatcher->BindAttachedSystem(&m_attachedParticleSystem);
    // Hand the InputDispatcher to the bridge so
    // `viewport/input` requests route into it. Nullable — the handler
    // is a no-op ack when no InputDispatcher is bound.
    dispatcher->SetInputDispatcher(m_inputDispatcher.get());
    Log("[host] host state bound (particleSystem + spawnerDriver)\n");

    HRESULT hr = InitWebView2();
    if (FAILED(hr))
    {
        // Runtime present but initialisation still failed (locked user-data
        // folder, corrupt install, policy). Reuse the same offer: when the
        // bundled bootstrapper is there, reinstalling is the likely fix.
        wchar_t detail[128];
        swprintf(detail, 128, L"(WebView2 initialisation failed, 0x%08lx.)", hr);
        if (IsFullyInteractive())
            OfferWebView2Install(hMain, detail);
        else
            Log("[host] WebView2 init failed (0x%08lx) — bailing headlessly\n", hr);
        DestroyWindow(hMain);
        g_self = nullptr;
        CoUninitialize();
        CloseLog();
        return 1;
    }

    // Size the popup HWND to the main window's
    // full client rect just before showing the window. Without this,
    // the popup is stuck at CreateWindowExW's bootstrap rect
    // (screen 16,16,320,240) and renders as a tiny preview at the
    // monitor's top-left until the user first resizes. By this
    // point WM_CREATE has completed, the engine + AlphaCompositor +
    // particleSystem are fully bound, and Engine::Reset can handle
    // the resize cleanly.
    layout.ApplyFullClient();

    // Hide the viewport popup. It still spans the full
    // main client (ApplyFullClient above) and the D3D9 swapchain on its
    // hidden HWND keeps rendering into the AlphaCompositor's shared RT,
    // which the host's DComp path presents — the WebView2 DOM canvas is the
    // visible viewport, and input flows through InputDispatcher rather than
    // the OS-routed path.
    {
        HWND hPopup = layout.GetViewport();
        if (hPopup) ShowWindow(hPopup, SW_HIDE);
        Log("[ArchC] viewport popup hidden (canvas-in-DOM is the visible surface)\n");
    }

    // --capture loads a scene + screenshots headlessly. Computed before the
    // window is shown so the show can avoid stealing focus in that mode.
    const bool captureMode = (!m_captureAlo.empty() || !m_captureRef.empty())
                             && !m_capturePng.empty();

    // In --capture mode show the window WITHOUT activating it: PrintWindow
    // (PW_RENDERFULLCONTENT) captures a non-foreground DComp/WebView2 window
    // fine, and the ui-ready gate below now keeps the window up for seconds — a
    // normal ShowWindow would pop a focus-stealing editor onto the user's screen
    // every capture. NOT SW_HIDE / SW_SHOWMINNOACTIVE: a hidden/minimized window
    // can stop DComp compositing and yield a black composite.
    // --drive shows the window too (PrintWindow needs a composed window) but,
    // like --capture, must NOT steal focus from a daily-driver editor.
    ShowWindow(hMain, (captureMode || m_automationMode) ? SW_SHOWNOACTIVATE : nCmdShow);
    UpdateWindow(hMain);

    // --capture: construct the one-shot runner (setup + per-frame tick +
    // exit mapping now live in CaptureRunner.cpp — Phase C split). Init
    // performs the exact swap+notify load sequence file/open uses (or the
    // synchronous --capture-ref catalog resolve); the pump below then
    // renders m_captureFrames frames and the runner writes the PNGs.
    host::CaptureRunner captureRunner(
        host::CaptureRunner::Params{
            m_captureAlo, m_captureRef, m_capturePng, m_captureFrames,
            m_captureSkydomeSlot,
            m_captureHasAmbient,
            {m_captureAmbient[0], m_captureAmbient[1], m_captureAmbient[2]},
            m_captureHasSun,
            {m_captureSun[0], m_captureSun[1], m_captureSun[2]},
            m_captureHasSunI, m_captureSunIntensity},
        host::CaptureRunner::Deps{
            engine, modManager, particleSystem, spawnerDriver,
            alphaCompositor, hMain, m_uiReady, m_sceneRectSeen,
            [this] { RenderD3D9(); },
            [this](const std::string& s) { Log("%s", s.c_str()); }});
    if (captureMode)
        captureRunner.Init();

    // main loop: switched from blocking GetMessage to PeekMessage
    // idle-render. The blocking variant produces no continuous WM_PAINT
    // events, so the per-frame spawner tick + engine render had no driver.
    // Now: drain queued messages, then render on idle, loop until
    // WM_QUIT. Mirrors the legacy main.cpp.
    //
    // No IsDialogMessage routing — the host has no modeless Win32
    // dialogs; tool panels live in React under WebView2 (which has its
    // own input routing and doesn't need TranslateAccelerator either).
    //
    // [resize-perf] The render is PACED to the display's refresh
    // cadence instead of free-running. The unpaced loop measured ~3000 fps
    // at idle ([PERF] probe): one core pegged and the GPU saturated with
    // queued frames, starving WebView2's renderer during splitter drags
    // (the dominant splitter-jank amplifier). Mechanics:
    //   - render only when the per-frame QPC budget has elapsed;
    //   - between frames, MsgWaitForMultipleObjectsEx sleeps until EITHER
    //     input/messages arrive (instant wake — input latency unchanged)
    //     or the next frame is due. MWMO_INPUTAVAILABLE because we consume
    //     via PeekMessage: input queued before the wait must still wake it.
    //   - timeBeginPeriod(1) for the loop's lifetime — without it the wait
    //     quantizes to the default ~15.6 ms timer and the cadence judders.
    //   - budget = one period of the primary display's refresh rate read at
    //     startup (fallback 60 Hz). This is a CAP, not vsync — Present
    //     stays unsynchronized; DWM composes whatever is latest.
    //   - QPC-frequency failure degrades to budget 0 = today's free-run.
    // Capture mode keeps its own Sleep(16) pacing and renders every
    // iteration (path unchanged).
    MSG m = {};
    bool quit = false;

    // [E5] Budget from the window's own monitor (helper logs the paced-to
    // line); WM_DISPLAYCHANGE + monitor moves recompute it live.
    UpdatePacingBudget(hMain);
    LONGLONG nextFrameQpc = PerfQpcNow();
    timeBeginPeriod(1);

    // --drive: scripted non-CDP composite capture. Its own top-level pump
    // branch (below) — built+ticked here, NOT under captureMode (which is false
    // in drive mode). Watchdogs: a startup deadline until app/ready, and a
    // render-budget cap of sum(settle)+60s once running.
    int       driveExitCode    = 0;
    bool      driveDcompSettled = false;
    LONGLONG  driveDcompStart  = 0;
    const LONGLONG driveStart  = PerfQpcNow();
    const LONGLONG driveFreq   = PerfQpcFreq();
    double    driveBudgetMs    = 0.0;
    std::unique_ptr<host::DriveRunner> driveRunner;

    // --record: deterministic clip recording. Its own pump branch (below), a
    // sibling of --drive. The runner is built once (after app/ready + the
    // one-time startup gate) then Ticked per emitted frame.
    int       recordExitCode   = 0;
    const LONGLONG recordStart = PerfQpcNow();
    const LONGLONG recordFreq  = PerfQpcFreq();
    double    recordBudgetMs   = 0.0;
    std::wstring recordTmpDir, recordOutDir;
    // Branch B: one background PNG-encode worker per record run. shared_ptr
    // because the capture hook (stored in m_clipRunner, a member that can
    // outlive this frame of Run) captures a copy — after the explicit
    // Finish() below, late destruction does no GDI+ work.
    std::shared_ptr<host::AsyncFrameEncoder> recordEncoder;
    constexpr int kBarrierPresents = 3;   // DComp-present barrier before each grab
                                          // (each present is DwmFlush'd in the capture
                                          // hook so the composited frame lands before
                                          // PrintWindow — see the capture lambda note)

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
        // Load/capture failure → bail to cleanup
        // without rendering (exit code set below).
        if (captureRunner.Failed()) break;

        // --drive: own top-level branch, FIRST (captureMode is false in drive
        // mode, so this must precede the !captureMode idle branch). Renders
        // every iteration; never blocks. States: wait app/ready -> build runner
        // -> one-shot DComp settle -> Tick per frame.
        if (engine && m_ephemeral)
        {
            RenderD3D9();
            const double elapsedMs = driveFreq > 0
                ? QpcMs(PerfQpcNow() - driveStart, driveFreq) : 0.0;

            if (driveFreq <= 0)
            {
                // No QPC clock: settles/watchdogs can't advance -> hard fail
                // rather than hang (theoretical on supported Windows).
                Log("[drive] no high-resolution timer available\n");
                driveExitCode = 5; quit = true;
            }
            else if (!m_uiReady)
            {
                // ready-gate: the top-of-loop PeekMessage drain delivers app/ready.
                if (elapsedMs >= 30000.0)
                {
                    Log("[drive] startup watchdog: app/ready never arrived\n");
                    driveExitCode = 5; quit = true;
                }
            }
            else if (!driveRunner)
            {
                std::string err;
                auto r = std::make_unique<host::DriveRunner>();
                if (!r->Init(ReadFileUtf8(m_driveScriptPath), err))
                {
                    Log("[drive] bad script: %s\n", err.c_str());
                    driveExitCode = r->ExitCode();   // 2
                    quit = true;
                }
                else
                {
                    r->SetHooks(
                        [this](const std::string& req){ return dispatcher->DispatchSync(req); },
                        [this](const std::string& u){
                            const std::wstring artifactDir = host::perf::CurrentConfig().artifactDir;
                            std::filesystem::path base = !artifactDir.empty()
                                ? std::filesystem::path(artifactDir)
                                : std::filesystem::current_path();
                            std::filesystem::path out = base / host::Utf8ToWide(u);
                            std::error_code ec;
                            std::filesystem::create_directories(out.parent_path(), ec);
                            return host::CaptureWindowToPng(hMain, out.wstring());
                        },
                        [df = driveFreq]{ return df > 0 ? QpcMs(PerfQpcNow(), df) : 0.0; },
                        [this](const std::string& msg){ Log("%s\n", msg.c_str()); });
                    r->SetProbeHook([this](double x0, double y0, double x1, double y1){
                        return host::ProbeWindowMaxLuma(hMain, x0, y0, x1, y1);
                    });
                    r->SetSelftestHook([this](const std::string& kind, int timeoutMs){
                        return RunDriveSelftest(kind, timeoutMs);
                    });
                    driveBudgetMs = r->TotalSettleMs() + 60000.0;
                    driveRunner = std::move(r);
                }
            }
            else if (!driveDcompSettled)
            {
                // one-shot 150 ms render-pumped settle so DComp commits the
                // deferred scene-rect crop before the first Tick/capture.
                if (driveDcompStart == 0) driveDcompStart = PerfQpcNow();
                if (driveFreq > 0 && QpcMs(PerfQpcNow() - driveDcompStart, driveFreq) >= 150.0)
                    driveDcompSettled = true;
            }
            else
            {
                if (driveRunner->Tick() == host::DriveRunner::Status::Done)
                {
                    driveExitCode = driveRunner->ExitCode();
                    quit = true;
                }
                else if (elapsedMs >= driveBudgetMs)
                {
                    Log("[drive] watchdog: exceeded %.0f ms budget\n", driveBudgetMs);
                    driveExitCode = 5; quit = true;
                }
            }

            Sleep(16);   // pace the sim (mirror the captureMode Sleep(16))
        }
        // --drive with a null engine (D3D9/device init failed): the drive
        // branch above can't run, so exit non-zero rather than spin forever or
        // return a silent exit-0 with nothing captured.
        else if (m_ephemeral && !engine)
        {
            Log("[drive] engine unavailable -- aborting drive run\n");
            driveExitCode = 5;
            quit = true;
        }
        // --record: own top-level branch (captureMode/m_ephemeral are false in
        // record mode, so this precedes the !captureMode idle branch). States:
        // wait app/ready -> parse timeline + one-time startup gate (seed/resize/
        // pause/open/catalog) + build runner -> Tick per emitted frame.
        else if (engine && m_recordMode)
        {
            RenderD3D9();
            const double elapsedMs = recordFreq > 0
                ? QpcMs(PerfQpcNow() - recordStart, recordFreq) : 0.0;

            if (recordFreq <= 0)
            {
                Log("[record] no high-resolution timer available\n");
                recordExitCode = 5; quit = true;
            }
            else if (!m_uiReady)
            {
                if (elapsedMs >= 30000.0)
                {
                    Log("[record] startup watchdog: app/ready never arrived\n");
                    recordExitCode = 5; quit = true;
                }
            }
            else if (!m_clipRunner)
            {
                // Parse the timeline (need width/height/openPath for the gate).
                std::string err;
                auto r = std::make_unique<host::ClipRunner>();
                // ${GAME} -> the resolved game install root (argv-or-registry, the
                // same root mods are discovered under), so a timeline's mod path
                // (e.g. "${GAME}/Mods/MyMod") survives a reinstall
                // elsewhere. Trailing separator stripped so the token joins cleanly
                // with "/Mods/...". Only defined when a root is known — a timeline
                // using ${GAME} without one fails loud in Init (exit 2).
                {
                    std::map<std::string, std::string> tokens;
                    if (modManager && !modManager->GameRoots().empty()
                        && !modManager->GameRoots().front().empty())
                    {
                        std::wstring g = modManager->GameRoots().front();
                        while (!g.empty() && (g.back() == L'\\' || g.back() == L'/')) g.pop_back();
                        tokens["GAME"] = WideToUtf8(g);
                    }
                    r->SetPathTokens(std::move(tokens));
                }
                const bool initOk = r->Init(ReadFileUtf8(m_recordScriptPath), err);
                // Strict mod-layer existence: token expansion (${GAME}) already ran
                // in Init, but ModManager::SetLayerStack SILENTLY DROPS a missing
                // directory and still reports success (records unmodded). Pre-check
                // the resolved non-empty layer paths so a wrong ${GAME} root or an
                // uninstalled mod FAILS LOUD instead of quietly rendering the
                // base-game look — the whole point of the token's fail-loud
                // contract. Empty `paths` (explicit Unmodded) is fine.
                std::string layerErr;
                if (initOk)
                {
                    for (const auto& ev : r->TL().ats)
                    {
                        if (ev.kind != "mods/set-layers") continue;
                        auto pit = ev.params.find("paths");
                        if (pit == ev.params.end() || !pit->is_array()) continue;
                        for (const auto& pe : *pit)
                        {
                            if (!pe.is_string()) continue;
                            const std::string p = pe.get<std::string>();
                            if (p.empty()) continue;
                            const DWORD attr = GetFileAttributesW(Utf8ToWide(p).c_str());
                            if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
                            {
                                layerErr = "mod layer not found: " + p
                                         + " (check the game install / ${GAME} root)";
                                break;
                            }
                        }
                        if (!layerErr.empty()) break;
                    }
                }
                if (!initOk || !layerErr.empty())
                {
                    Log("[record] bad timeline: %s\n", (!layerErr.empty() ? layerErr : err).c_str());
                    recordExitCode = 2;   // Init-fail and mod-miss are both exit 2
                    quit = true;
                }
                else
                {
                    const clip::Timeline tl = r->TL();   // small copy for the gate

                    // Lock the stats-tick FPS readout to the clip's virtual rate
                    // from this point on — BEFORE the settle loops below, so the
                    // 4 Hz timer can never paint a wall-clock FPS into a frame
                    // that ends up captured (the chip was a run-variant; see the
                    // WM_TIMER handler note).
                    m_recordTimelineFps = tl.fps;

                    // (a) deterministic particle RNG (Goal-A motion correctness).
                    srand(0x5EEDu);

                    // (b) resize the WINDOW to tl.width x tl.height via SetWindowPos
                    //     -> WM_WINDOWPOSCHANGED -> LayoutBroker -> Engine::ResetForResize.
                    //     CaptureWindowToPng grabs the WINDOW rect (GetWindowRect,
                    //     WindowCapture.cpp:31), so the window size IS the emitted
                    //     frame size — set it directly so frames are exactly
                    //     tl.width x tl.height (the engine viewport is the client
                    //     sub-rect, a bit smaller after chrome).
                    SetWindowPos(hMain, nullptr, 0, 0, tl.width, tl.height,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

                    // (b2) render the WebView chrome at tl.scale device-pixel ratio so
                    //      a zoomed crop (e.g. the F4 mod picker) stays sharp: the same
                    //      CSS layout (width/scale wide) rasterizes at higher device px.
                    //      MUST disable ShouldDetectMonitorScaleChanges first or WebView2's
                    //      auto-detection reverts our value to the monitor DPI. Pinned for
                    //      EVERY record run — scale:1 included — so a non-100% monitor
                    //      can't skew authored coordinates/CSS layout (record output must
                    //      be display-independent; wiki-media pipeline spec §1.7. This
                    //      changes scale:1 behavior on non-100% displays: determinism
                    //      wins). The batch preflight asserts the log line below.
                    if (webController)
                    {
                        ComPtr<ICoreWebView2Controller3> ctrl3;
                        if (SUCCEEDED(webController.As(&ctrl3)) && ctrl3)
                        {
                            ctrl3->put_ShouldDetectMonitorScaleChanges(FALSE);
                            HRESULT shr = ctrl3->put_RasterizationScale(tl.scale);
                            Log("[record] put_RasterizationScale(%.2f) hr=0x%08lx\n", tl.scale, shr);
                        }
                    }

                    // (c) freeze the sim clock; record steps it once per frame.
                    SetPreviewPaused(true);

                    // (d) open the scene (if any) via the synchronous bridge path.
                    //     Abort (exit 3) on a FAILED open: file/open reports failure
                    //     as nested {ok:true,data:{ok:false}}, so check it or we'd
                    //     silently record an empty/wrong scene.
                    bool gateOk = true;
                    if (!tl.openPath.empty())
                    {
                        nlohmann::json req = {
                            {"type", "req"}, {"id", "record-open"},
                            {"kind", "file/open"}, {"params", {{"path", tl.openPath}}}};
                        if (drive::ClassifyResponse(dispatcher->DispatchSync(req.dump())) != drive::Outcome::Ok)
                        {
                            Log("[record] file/open failed for %s\n", tl.openPath.c_str());
                            recordExitCode = 3; quit = true; gateOk = false;
                        }
                    }

                    if (gateOk)
                    {
                    // (e) build the catalog + a render-pumped settle so
                    //     ReloadTextures + the resize reflow + any async catalog
                    //     harvest land BEFORE t=0 (paused -> no sim advance).
                    engine->BuildCatalogSync();
                    {
                        const LONGLONG s = PerfQpcNow();
                        while (QpcMs(PerfQpcNow() - s, recordFreq) < tl.openSettleMs)
                        {
                            MSG mw;
                            while (PeekMessage(&mw, nullptr, 0, 0, PM_REMOVE))
                            { TranslateMessage(&mw); DispatchMessage(&mw); }
                            RenderD3D9();
                            Sleep(8);
                        }
                    }

                    // (e0) headless capture mode: tell the web to ack each frame
                    //      SYNCHRONOUSLY (flushSync, no double-rAF) — the ack is a
                    //      message commit, not a presented frame, so the rAF
                    //      "proof of paint" wait (which stalls when the window
                    //      isn't presented) is dropped. Latched once here, before
                    //      the frame loop; the legacy foreground path never sends
                    //      it (double-rAF stays, so the golden-diff baseline is
                    //      unchanged).
                    if (m_recordHeadless && webView)
                    {
                        nlohmann::json hm = {{"type","ui/record-headless"}};
                        webView->PostWebMessageAsJson(host::Utf8ToWide(hm.dump()).c_str());
                    }

                    // Run the record window out of sight for a machine-free render.
                    if (m_recordMinimized && m_recordHeadless)
                    {
                        // Move the window fully OFFSCREEN instead of minimizing it.
                        // A minimized window has no composited client area (and DWM
                        // throttles minimized-window composition anyway), so the
                        // PrintWindow/GrabWindowPixels grab below would read black or
                        // stall. An offscreen-but-visible window stays full-size and
                        // normally composited — correct + fast grab (headless ~90s
                        // minimized -> ~30s offscreen) — while invisible to the user.
                        // (#510)
                        SetWindowPos(hMain, nullptr, -32000, -32000, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        Log("[record] window moved offscreen for headless capture\n");
                    }

                    // (e1) hide the right-dock (Spawner/Lighting/Atlas) panel so the
                    //      recorded clip shows a clean layout + more curve editor.
                    {
                        nlohmann::json hp = {{"type","ui/hide-panel"}};
                        if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(hp.dump()).c_str());
                    }

                    // (e2) focus the curve panel on each track-key tween's channel so
                    //      a scripted scrub shows the channel it edits (the panel focus
                    //      is React-local + defaults to red). Posted once after settle;
                    //      focusChannel persists independent of the emitter selection.
                    for (const auto& tk : tl.trackKeys)
                    {
                        nlohmann::json fm = {{"type","ui/focus-channel"},{"channel", tk.track}};
                        if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(fm.dump()).c_str());
                    }

                    // (e3) let React APPLY the (e1)/(e2) pushes before frame capture.
                    //      PostWebMessageAsJson is async: without a short render-pumped
                    //      wait the first frames capture the still-open right dock (the
                    //      Spawner panel), so the hide-panel + focus-channel must settle
                    //      here, BEFORE t=0. Paused sim => no particle/clock advance.
                    //      HEADLESS also waits for layout/scene-rect (like --capture) so
                    //      the window grab reads a fully-laid-out viewport, not a
                    //      mid-reflow one — consistent with the --capture gate.
                    {
                        const LONGLONG s = PerfQpcNow();
                        while (QpcMs(PerfQpcNow() - s, recordFreq) < 500 ||
                               (m_recordHeadless && !m_sceneRectSeen &&
                                QpcMs(PerfQpcNow() - s, recordFreq) < 3000))
                        {
                            MSG mw;
                            while (PeekMessage(&mw, nullptr, 0, 0, PM_REMOVE))
                            { TranslateMessage(&mw); DispatchMessage(&mw); }
                            RenderD3D9();
                            Sleep(8);
                        }
                    }
                    if (m_recordHeadless && !m_sceneRectSeen)
                        Log("[record] WARNING headless: no layout/scene-rect after settle — "
                            "engine readback falls back to the full RT (offstage pixels are "
                            "hidden by opaque panels, so registration is unaffected)\n");

                    // (e4) semantic-targeting cursor: stream the cursor TRACK to the
                    //      web side ONCE here (it resolves the selectors against its
                    //      own live DOM per frame). The host then ticks per frame
                    //      ({type:"ui/cursor-tick"}) instead of pushing a computed
                    //      {ui/cursor}. A literal-only track posts nothing here — the
                    //      existing per-frame ui/cursor push path stays unchanged.
                    if (clip::CursorTrackIsTargetBearing(tl.cursor))
                    {
                        const nlohmann::json trackMsg = clip::BuildCursorTrackJson(tl.cursor);
                        if (webView)
                            webView->PostWebMessageAsJson(host::Utf8ToWide(trackMsg.dump()).c_str());
                    }

                    // (f) output dirs: render to <out>.tmp, move on success.
                    recordOutDir = host::Utf8ToWide(tl.out);
                    recordTmpDir = recordOutDir + L".tmp";
                    std::error_code ec;
                    std::filesystem::remove_all(recordTmpDir, ec);
                    std::filesystem::create_directories(recordTmpDir, ec);

                    // (g) hooks.
                    const std::wstring tmpDir = recordTmpDir;
                    // Branch B: start the background encoder BEFORE the hooks
                    // capture it. Ctor pre-warms the PNG CLSID on this (UI)
                    // thread (GdiplusEncode.h's cache is not first-call
                    // thread-safe) and spawns the single worker.
                    recordEncoder = std::make_shared<host::AsyncFrameEncoder>(
                        128ull * 1024 * 1024,
                        [this](const std::string& s){ Log("[record] %s\n", s.c_str()); });
                    r->SetHooks(
                        // dispatch (allowlisted bridge req -> response)
                        [this, rf = recordFreq](const std::string& req){
                            const LONGLONG t0 = PerfQpcNow();
                            std::string resp = dispatcher->DispatchSync(req);
                            m_recordTiming.curDispatch += QpcMs(PerfQpcNow() - t0, rf);
                            return resp;
                        },
                        // step the preview clock + drive the spawner ONCE at the
                        // fixed virtual dt (RenderD3D9's spawner tick is skipped in
                        // record mode, so this is the only advance per frame).
                        [this](int frames60){
                            StepPreviewFrames(frames60);
                            if (spawnerDriver && particleSystem)
                                spawnerDriver->Tick(frames60 / 60.0f, particleSystem.get(), engine.get());
                        },
                        // ui/cursor host->web push (device px; React divides by DPR).
                        // Timeline coords are CAPTURED-FRAME px, but the PrintWindow
                        // capture includes the window chrome (native title bar +
                        // borders) while RecordCursor positions inside the webview
                        // (client area). Subtract the client-origin offset so the
                        // cursor lands where the author measured in the frame.
                        [this](double x, double y, bool vis, bool press){
                            POINT org = {0, 0};
                            RECT wr = {0, 0, 0, 0};
                            ClientToScreen(hMain, &org);
                            GetWindowRect(hMain, &wr);
                            const double cx = x - (org.x - wr.left);
                            const double cy = y - (org.y - wr.top);
                            nlohmann::json m = {{"type","ui/cursor"},{"x",cx},{"y",cy},
                                                {"visible",vis},{"pressed",press},{"frame",m_recordFrame}};
                            if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(m.dump()).c_str());
                        },
                        // ack: pumped wait for ui/frame-acked >= frameId (bounded).
                        // [record-timing] the whole hook (incl. its inner
                        // RenderD3D9 pumps) accrues to the ACK segment.
                        [this, rf = recordFreq](int frameId, double deadlineMs){
                            const LONGLONG s = PerfQpcNow();
                            bool acked = false;
                            // Headless (message-ack): the web posts the rich ack
                            // SYNCHRONOUSLY (flushSync, no double-rAF), so pump the
                            // queue until it lands — NO RenderD3D9, no rAF/present
                            // dependency (that ~2 s/frame stall is exactly what this
                            // removes). Short deadline fails fast: a missing ack on a
                            // target clip is a loud exit 3, never a silent stale frame.
                            const double dl = m_recordHeadless ? 500.0 : deadlineMs;
                            for (;;)
                            {
                                MSG mw;
                                while (PeekMessage(&mw, nullptr, 0, 0, PM_REMOVE))
                                { TranslateMessage(&mw); DispatchMessage(&mw); }
                                if (m_lastAckedFrame >= frameId) { acked = true; break; }
                                if (rf > 0 && QpcMs(PerfQpcNow() - s, rf) >= dl) break;
                                if (!m_recordHeadless) RenderD3D9();
                                // [R4] Message-aware wait: the ack ARRIVES as a
                                // window message, so wake the instant it posts
                                // instead of sleeping a fixed 1/4 ms past it.
                                // Cap keeps the foreground path's render cadence
                                // (~4 ms) and the old worst-case wait unchanged.
                                MsgWaitForMultipleObjectsEx(
                                    0, nullptr, m_recordHeadless ? 1 : 4,
                                    QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                            }
                            m_recordTiming.curAck += QpcMs(PerfQpcNow() - s, rf);
                            if (m_recordHeadless) m_headlessAckOk = acked;
                            return acked;
                        },
                        // capture: present the latest engine frame, then BLOCK on the
                        // compositor before PrintWindow reads the window. Each
                        // RenderD3D9() Present1's the composed surface to the DXGI
                        // swapchain, but DComp only picks that up on its NEXT
                        // composition cycle (async — see RenderD3D9's CompositeEngineFrame
                        // note). At 30fps the ack/Sleep slack let that cycle land; at
                        // 60fps the grabs outrun it and PrintWindow catches a viewport
                        // with the engine frame not yet composited (static background, no
                        // smoke). DwmFlush() blocks until the DWM/DComp composition pass
                        // completes, so the just-presented frame is on the window before
                        // we grab. Interleaved (not just trailing) so the final present is
                        // always followed by a composited pass.
                        [this, tmpDir, bp = kBarrierPresents, rf = recordFreq,
                         enc = recordEncoder](int idx){
                            wchar_t name[40];
                            swprintf_s(name, L"\\frame_%05d.png", idx);
                            host::AsyncFrameEncoder::Frame f;
                            f.path = tmpDir + name;

                            // Headless (PE_RECORD_HEADLESS): the record window is
                            // moved OFFSCREEN (not minimized — see the SetWindowPos
                            // above), so it composites normally AND can't be occluded
                            // by any other window. That makes the fast foreground
                            // capture path below (barrier + GrabWindowPixels) both
                            // correct and occlusion-immune for headless too. (#510
                            // replaced the old ~50ms/frame CapturePreview + CPU-
                            // composite path with this ~20ms window grab.)
                            if (m_recordHeadless && !m_headlessAckOk)
                            {
                                // A withheld/timed-out ack means the web's flushSync
                                // commit failed — the DOM is STALE. Fail the frame
                                // loudly rather than publish it.
                                Log("[record] headless ack failed at frame %d — DOM not committed\n", idx);
                                return false;
                            }

                            // [record-timing] barrier (present+flush loop) and
                            // png are timed separately. Branch B: png is now
                            // GRAB + ENQUEUE only — the compress+write runs on
                            // the encoder worker (AsyncFrameEncoder.h), so the
                            // png segment no longer contains the zlib cost.
                            //
                            // [R1] Adaptive barrier: the fixed 3x flush existed
                            // because Present1'd engine frames land on DComp's
                            // NEXT composition pass — 3 was a safe worst case.
                            // Probe the global compositor frame counter
                            // (DwmGetCompositionTimingInfo requires a NULL hwnd
                            // on Win8.1+): once composition has advanced ≥2
                            // passes past the pre-present sample (one that may
                            // have missed our present + one that must include
                            // it), the frame is on the window — stop early.
                            // Cap stays bp (= the old fixed count); any probe
                            // failure falls back to fixed-bp and is surfaced
                            // in the summary, never silent.
                            const LONGLONG b0 = PerfQpcNow();
                            DWM_TIMING_INFO ti0 = {};
                            ti0.cbSize = sizeof(ti0);
                            const bool probe0 =
                                SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti0));
                            if (!probe0) m_recordTiming.barrierProbeFailed = true;
                            for (int i = 0; i < bp; ++i)
                            {
                                RenderD3D9(); DwmFlush();
                                ++m_recordTiming.barrierFlushTotal;
                                if (!probe0) continue;   // fixed-bp fallback
                                DWM_TIMING_INFO ti = {};
                                ti.cbSize = sizeof(ti);
                                if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)))
                                {
                                    if (ti.cFrame >= ti0.cFrame + 2) break;
                                }
                                else
                                {
                                    m_recordTiming.barrierProbeFailed = true;
                                }
                            }
                            const LONGLONG b1 = PerfQpcNow();
                            const bool ok = host::GrabWindowPixels(hMain, f.bgra, f.w, f.h)
                                            && enc->Enqueue(std::move(f));
                            m_recordTiming.curBarrier += QpcMs(b1 - b0, rf);
                            m_recordTiming.curPng     += QpcMs(PerfQpcNow() - b1, rf);
                            return ok;
                        },
                        [this](const std::string& s){ Log("[record] %s\n", s.c_str()); },
                        // ui/* passthrough: post {type:kind, ...params} to the webview
                        // verbatim (panel/picker open state). View-only — never the bridge.
                        [this](const std::string& kind, const nlohmann::json& params){
                            nlohmann::json m = params.is_object() ? params : nlohmann::json::object();
                            m["type"] = kind;
                            if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(m.dump()).c_str());
                        },
                        // ackData: read back the SEMANTIC-cursor ack the web side
                        // resolved for `frameId` ({cursor:{x,y,vis,press}, resolved:[...]}).
                        // Returns null if the ack for this frame carried no cursor obj
                        // (literal path / not yet seen) — the runner treats that as no-op.
                        [this](int frameId) -> nlohmann::json {
                            if (m_lastAckCursorFrame != frameId) return nullptr;
                            return nlohmann::json{
                                {"cursor",   m_lastAckCursor},
                                {"resolved", m_lastAckResolved}};
                        });

                    recordBudgetMs = r->FrameCount() *
                                     (host::ClipRunner::kAckDeadlineMs + 1000.0) + 30000.0;
                    m_clipRunner = std::move(r);
                    }  // end if (gateOk)
                }
            }
            else
            {
                m_recordFrame = m_clipRunner->CurrentFrame();
                // [record-timing] stamp setup (branch start -> first Tick) once;
                // time each Tick; the hooks accumulated the four segments.
                if (!m_recordTiming.sawFirstTick)
                {
                    m_recordTiming.sawFirstTick = true;
                    m_recordTiming.setupMs = elapsedMs;
                }
                m_recordTiming.curDispatch = m_recordTiming.curAck = 0.0;
                m_recordTiming.curBarrier  = m_recordTiming.curPng = 0.0;
                const LONGLONG tickStart = PerfQpcNow();
                const auto tickStatus = m_clipRunner->Tick();
                m_recordTiming.frame.push_back(QpcMs(PerfQpcNow() - tickStart, recordFreq));
                m_recordTiming.dispatch.push_back(m_recordTiming.curDispatch);
                m_recordTiming.ack.push_back(m_recordTiming.curAck);
                m_recordTiming.barrier.push_back(m_recordTiming.curBarrier);
                m_recordTiming.png.push_back(m_recordTiming.curPng);
                if (m_recordTimingVerbose)
                    Log("[record-timing] f=%d frame=%.1f dispatch=%.1f ack=%.1f "
                        "barrier=%.1f png=%.1f\n",
                        m_recordFrame, m_recordTiming.frame.back(),
                        m_recordTiming.curDispatch, m_recordTiming.curAck,
                        m_recordTiming.curBarrier, m_recordTiming.curPng);
                if (tickStatus == host::ClipRunner::Status::Done)
                {
                    recordExitCode = m_clipRunner->ExitCode();
                    // Branch B: drain + join the background encoder BEFORE the
                    // publish decision — .tmp is never renamed on a dirty
                    // drain (a queued frame's write can fail after its Tick
                    // already returned success; plan risk 4).
                    if (recordEncoder && !recordEncoder->Finish() && recordExitCode == 0)
                    {
                        Log("[record] async encode failed at %ls\n",
                            recordEncoder->FailedPath().c_str());
                        recordExitCode = 4;
                    }
                    // Move the completed sequence into place on success only.
                    if (recordExitCode == 0)
                    {
                        // an-audit-finding: `out` is validated as relative + traversal-free,
                        // which stops an ESCAPE but not the destruction of an existing
                        // directory under the launch dir — the publish below is an
                        // unconditional remove_all. Refuse unless the target is absent,
                        // empty, or holds nothing but a previous record's own output.
                        // Re-shooting into the same directory (the normal workflow)
                        // still works; deleting a stranger's files does not.
                        std::error_code ecScan;
                        const bool outExists = std::filesystem::exists(recordOutDir, ecScan);
                        std::vector<std::wstring> outEntries;
                        if (outExists)
                        {
                            for (const auto& de : std::filesystem::directory_iterator(recordOutDir, ecScan))
                                outEntries.push_back(de.path().filename().wstring());
                        }
                        std::wstring refuseReason;
                        if (!recordsafety::MayReplaceOutputDir(outExists, outEntries, refuseReason))
                        {
                            Log("[record] REFUSING to replace output dir %ls: %ls\n",
                                recordOutDir.c_str(), refuseReason.c_str());
                            Log("[record] the frames are intact in %ls — move them yourself, "
                                "or point 'out' at a new directory\n", recordTmpDir.c_str());
                            recordExitCode = 4;   // publish refused -> non-zero exit
                        }
                        else
                        {
                        std::error_code ec;
                        std::filesystem::remove_all(recordOutDir, ec);
                        std::error_code ec2;
                        std::filesystem::rename(recordTmpDir, recordOutDir, ec2);
                        if (ec2)
                        {
                            Log("[record] move tmp -> out failed: %s\n", ec2.message().c_str());
                            recordExitCode = 4;   // publish failure -> non-zero exit
                        }
                        // Verify sidecar: a target-bearing run accumulated the
                        // per-frame resolved cursor centers — write them next to the
                        // frames so a downstream script can check the cursor landed on
                        // each authored element. Literal runs skip it (empty array).
                        else if (m_clipRunner->IsTargetCursor())
                        {
                            // One sidecar row per frame on a clean run (step 4a
                            // appends or aborts). A short sidecar means a frame was
                            // captured without resolve validation — fail, don't ship.
                            if ((int)m_clipRunner->Sidecar().size() != m_clipRunner->FrameCount())
                            {
                                Log("[record] cursor sidecar incomplete: %d of %d frames\n",
                                    (int)m_clipRunner->Sidecar().size(), m_clipRunner->FrameCount());
                                recordExitCode = 4;
                            }
                            const std::filesystem::path sidecar =
                                std::filesystem::path(recordOutDir) / L"cursor-sidecar.json";
                            std::ofstream f(sidecar, std::ios::binary | std::ios::trunc);
                            if (f)
                            {
                                f << m_clipRunner->Sidecar().dump(2);
                                Log("[record] wrote cursor sidecar (%d frames)\n",
                                    (int)m_clipRunner->Sidecar().size());
                            }
                            else
                            {
                                Log("[record] cursor sidecar write failed\n");
                                recordExitCode = 4;
                            }
                        }
                        }   // end: output dir was safe to replace
                    }
                    // [R3] Snapshot queue stats before the summary reads them.
                    if (recordEncoder) m_recordEncoderStats = recordEncoder->GetQueueStats();
                    LogRecordTimingSummary(recordFreq > 0
                        ? QpcMs(PerfQpcNow() - recordStart, recordFreq) : 0.0);
                    Log("[record] done: %d frames, exit %d\n",
                        m_clipRunner->FrameCount(), recordExitCode);
                    quit = true;
                }
                else if (elapsedMs >= recordBudgetMs)
                {
                    Log("[record] watchdog: exceeded %.0f ms budget\n", recordBudgetMs);
                    // [record-timing] a timed-out run still yields its numbers.
                    if (recordEncoder) m_recordEncoderStats = recordEncoder->GetQueueStats();
                    LogRecordTimingSummary(elapsedMs);
                    // Branch B: join the encoder before quitting (exit 5 stands
                    // regardless of the drain result).
                    if (recordEncoder) recordEncoder->Finish();
                    recordExitCode = 5; quit = true;
                }
            }
        }
        // --record with a null engine: can't run; exit non-zero.
        else if (m_recordMode && !engine)
        {
            Log("[record] engine unavailable -- aborting record run\n");
            recordExitCode = 5;
            quit = true;
        }
        // Idle: render one frame per budget slot. Cheap enough to always
        // run (Engine has its own paused / IsPreviewPaused gates that skip
        // the simulation step when set; render still presents to keep the
        // surface valid).
        else if (engine && !captureMode)
        {
            const LONGLONG now = PerfQpcNow();
            if (now >= nextFrameQpc)
            {
                RenderD3D9();
                // [B1] Deliver any live-coalesced trailing broadcast once per
                // display frame — the primary flush path (the DispatchSync-top
                // and stats-timer flushes cover pump-starved cases).
                if (dispatcher) dispatcher->FlushPendingEmits();
                // [C4] Service a deferred autosave in the same idle slot —
                // after the present, never mid-gesture (see the latch note).
                ServicePendingAutosave(false);
                // Schedule from "now", not "+= budget": a slow frame must
                // not bank catch-up renders (cap semantics, not vsync).
                nextFrameQpc = now + m_frameBudgetQpc;
            }
            // Sleep until input or the next frame slot, whichever first.
            // Round the wait UP to whole ms so an early wake doesn't spin
            // through sub-ms remainders.
            const LONGLONG remainTicks = nextFrameQpc - PerfQpcNow();
            const LONGLONG f = PerfQpcFreq();
            if (remainTicks > 0 && f > 0)
            {
                const DWORD waitMs =
                    static_cast<DWORD>((remainTicks * 1000 + f - 1) / f);
                if (waitMs > 0)
                {
                    MsgWaitForMultipleObjectsEx(0, nullptr, waitMs,
                                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                }
            }
        }
        else if (engine)
        {
            RenderD3D9();

            // --capture: the runner owns pacing, the layout gate, the
            // frame count, and the RT+composite writes (CaptureRunner.cpp,
            // Phase C split). Done => the one-shot is finished; quit the pump.
            if (captureMode &&
                captureRunner.Tick() == host::CaptureRunner::TickResult::Done)
            {
                quit = true;
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

    // [resize-perf] matching release for the timeBeginPeriod above.
    timeEndPeriod(1);

    g_self = nullptr;
    // Branch B: a WM_QUIT escape from the pump (user closed the record window)
    // bypasses the Done/watchdog joins above — the encoder worker MUST be
    // joined before GdiplusShutdown or it races process-level GDI+ teardown.
    if (recordEncoder) recordEncoder->Finish();
    // [C3] Join the preview-encode worker for the same reason — it encodes
    // via GDI+ and must not race process-level GDI+ teardown.
    if (dispatcher) dispatcher->ShutdownPreviewWorker();
    // Matching shutdown for the GdiplusStartup above. Safe
    // here because the message pump has drained: no dispatcher
    // handlers (CaptureSnapshotPng et al) can run after WM_QUIT.
    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    CloseLog();
    // In --capture mode we break the loop via
    // the `quit` flag (not PostQuitMessage), so m.wParam is stale; return
    // an explicit 0/2 so a script can detect a bad load / failed write.
    if (captureMode) return captureRunner.ExitCode();
    // --drive likewise breaks via `quit`; return the runner's explicit code.
    if (m_ephemeral) return driveExitCode;
    if (m_recordMode) return recordExitCode;
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
                       int captureSkydome,
                       const std::wstring& captureRef,
                       bool hasAmbient, float ambR, float ambG, float ambB,
                       bool hasSun, float sunR, float sunG, float sunB,
                       bool hasSunI, float sunIntensity,
                       const std::wstring& driveScriptPath,
                       const std::wstring& recordScriptPath,
                       const std::wstring& perfWebViewProfile)
    : m_impl(new HostWindowImpl(hInstance, textureManager, shaderManager, fileManager,
                                gameRoots, useDevUi, useTestHost,
                                captureAlo, capturePng, captureFrames, captureSkydome,
                                captureRef,
                                hasAmbient, ambR, ambG, ambB,
                                hasSun, sunR, sunG, sunB,
                                hasSunI, sunIntensity, driveScriptPath, recordScriptPath,
                                perfWebViewProfile))
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
        int captureSkydome,
        const std::wstring& captureRef,
        bool hasAmbient, float ambR, float ambG, float ambB,
        bool hasSun, float sunR, float sunG, float sunB,
        bool hasSunI, float sunIntensity,
        const std::wstring& driveScriptPath,
        const std::wstring& recordScriptPath,
        const std::wstring& perfTracePath,
        const std::wstring& perfTraceMode,
        const std::wstring& perfArtifactDir,
        const std::wstring& perfWebViewProfile)
{
    std::wstring effectiveTracePath = perfTracePath;
    host::perf::SinkMode traceMode = host::perf::SinkMode::Off;
    if (!perfTraceMode.empty())
    {
        if (!IsKnownPerfTraceMode(perfTraceMode))
        {
            fwprintf(stderr, L"--perf-trace-mode: expected off, null, or file; got '%s'\n",
                     perfTraceMode.c_str());
            return 2;
        }
        traceMode = host::perf::ParseSinkMode(perfTraceMode);
    }
    if (!effectiveTracePath.empty())
        traceMode = host::perf::SinkMode::File;
    if (traceMode == host::perf::SinkMode::Off && !perfArtifactDir.empty())
    {
        traceMode = host::perf::SinkMode::File;
        effectiveTracePath = JoinPath(perfArtifactDir, L"perf-trace.ndjson");
    }
    if (!perfArtifactDir.empty())
        SHCreateDirectoryExW(nullptr, perfArtifactDir.c_str(), nullptr);

    bool perfTraceStarted = false;
    if (traceMode != host::perf::SinkMode::Off)
    {
        host::perf::Config cfg;
        cfg.mode = traceMode;
        cfg.tracePath = effectiveTracePath;
        cfg.artifactDir = perfArtifactDir;
        std::string err;
        if (!host::perf::Init(cfg, &err))
        {
            fprintf(stderr, "perf trace init failed: %s\n", err.c_str());
            return 2;
        }
        perfTraceStarted = true;
        host::perf::Emit({
            {"eventName", "host.perf_configuration"},
            {"eventType", "instant"},
            {"traceMode", traceMode == host::perf::SinkMode::File ? "file" : "null"},
            {"tracePath", host::WideToUtf8(effectiveTracePath)},
            {"artifactDir", host::WideToUtf8(perfArtifactDir)},
            {"webViewProfile", host::WideToUtf8(perfWebViewProfile)}
        });
    }

    HostWindow host(hInstance, textureManager, shaderManager, fileManager,
                    gameRoots, useDevUi, useTestHost,
                    captureAlo, capturePng, captureFrames, captureSkydome,
                    captureRef,
                    hasAmbient, ambR, ambG, ambB,
                    hasSun, sunR, sunG, sunB,
                    hasSunI, sunIntensity, driveScriptPath, recordScriptPath,
                    perfWebViewProfile);
    const int result = host.Run(nCmdShow);
    if (perfTraceStarted)
    {
        host::perf::Emit({
            {"eventName", "host.process_exit"},
            {"eventType", "instant"},
            {"exitCode", result}
        });
        host::perf::Shutdown();
    }
    return result;
}

} // namespace host
