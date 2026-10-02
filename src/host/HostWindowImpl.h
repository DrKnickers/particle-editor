// HostWindowImpl.h — the private implementation struct behind HostWindow,
// shared by the host-window translation units:
//   HostWindow.cpp            logging, the render loop, the main window
//                             procedure, Run() and the HostWindow facade
//   HostWindow_WebView2.cpp   WebView2 creation and per-controller setup,
//                             web-message ingress, resize, mouse forwarding
//   HostWindow_Viewport.cpp   the viewport window procedure
//   HostWindow_Record.cpp     the --record arm setup
// It is not part of the host's public surface: include it only from those
// files, and only after defining _WIN32_WINNT / WINVER as 0x0A00 (WebView2
// and DPI awareness need a Windows 10 target).
#ifndef HOST_HOST_WINDOW_IMPL_H
#define HOST_HOST_WINDOW_IMPL_H

#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0A00
#error "Define _WIN32_WINNT and WINVER as 0x0A00 before including HostWindowImpl.h"
#endif

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
#include <array>
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
#include "StringConv.h"     // host::Utf8ToWide / WideToUtf8 (shared host copy)
#include "generated/EmbeddedWebAssets.h"  // embedded React bundle manifest (RCDATA) served on app.local
#include "CaptureGoldenProfile.h"
#include "LightingSettings.h"
#include "PerfTrace.h"
#include "RestoredSettings.h"
#include "SettingsRegistry.h"
#include "WebViewModalPolicy.h"
#include "WebViewCrashPolicy.h"        // DecideWebFailure / PlanDeadWebClose (ProcessFailed recovery)
#include "WebMessageIngressPolicy.h"   // ShouldAcceptWebMessage (bridge ingress cap)
#include "WebViewOriginPolicy.h"       // IsApprovedWebViewOrigin (navigation + ingress origin gate)
#include "BridgeWire.h"                // SerializeBridgeEnvelope (the one host->UI serializer)
#include "ModulePath.h"               // host::ModuleDirectory (grow-until-it-fits module path)
#include "StartupCallbackAdapter.h"   // guarded one-shot WebView2 creation callbacks
#include "CompositionStartupPolicy.h" // second WebView2 create's synchronous failure channel
#include "ViewportUnavailablePolicy.h" // missing engine pixels: notice or automation exit

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
#include "../ResourceLimits.h"   // kMaxWebMessageChars (bridge ingress cap)
#include "../UI/TexturePalette.h"   // Store::SetEphemeral (automation palette isolation)
#include "../ParticleSystem.h"
#include "../ParticleSystemIO.h"
#include "../ParticleSystemInstance.h"
#include "../SpawnerDriver.h"
#include "../UndoStack.h"
#include "../Autosave.h"  // two-tier autosave timers + clean-exit cleanup
#include "DriveRunner.h"   // --drive: scripted non-CDP composite capture
#include "ClipRunner.h"    // --record: deterministic clip recording (PNG sequence)
#include "RecordTrace.h"   // --record: flag-gated pump-schedule trace
#include "RecordOutputSafety.h"  // --record: refuse to remove_all a non-output dir
#include "CaptureRunner.h" // --capture/--capture-ref: one-shot render + PNG
#include "HostRunUtil.h"   // PerfQpcNow/PerfQpcFreq/QpcMs/DeriveSibling (shared with the runners)
#include "AsyncFrameEncoder.h"   // --record: background PNG encode

using namespace Microsoft::WRL;

namespace host {

// --drive: read a (small) UTF-8/ASCII JSON script file into a std::string.
// Returns empty on any error; DriveRunner::Init reports a bad/empty script.
inline std::string ReadFileUtf8(const std::wstring& path)
{
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0)
    {
        out.resize(static_cast<size_t>(n));
        // A short read (I/O error, file truncated under us) must not hand the
        // runner a zero-padded script — report it as unreadable instead.
        if (fread(&out[0], 1, static_cast<size_t>(n), f) != static_cast<size_t>(n))
            out.clear();
    }
    fclose(f);
    return out;
}

// --record: inspect `dir` on disk and ask recordsafety::MayReplaceRecordDir
// whether it may be remove_all'd. Used for the output dir at publish AND the
// `<out>.tmp` staging dir at setup. A listing error is
// reported as such rather than read as "empty".
inline bool MayReplaceRecordDirOnDisk(const std::wstring& dir, std::wstring& reason)
{
    std::error_code ec;
    const std::filesystem::file_status st = std::filesystem::status(dir, ec);
    if (st.type() == std::filesystem::file_type::none)   // status itself failed
    {
        reason = L"could not be inspected";
        return false;
    }
    const bool exists = std::filesystem::exists(st);
    const bool isDirectory = std::filesystem::is_directory(st);
    std::vector<std::wstring> entries;
    bool listed = true;
    if (exists && isDirectory)
    {
        std::filesystem::directory_iterator it(dir, ec);
        const std::filesystem::directory_iterator end;
        while (!ec && it != end)
        {
            entries.push_back(it->path().filename().wstring());
            it.increment(ec);
        }
        listed = !ec;
    }
    return recordsafety::MayReplaceRecordDir(exists, isDirectory, listed, entries, reason);
}

constexpr wchar_t kHostWindowClassName[]     = L"AloHostMain";
constexpr wchar_t kHostViewportClassName[]   = L"AloHostViewport";
constexpr int     kInitialWidth              = 1280;
constexpr int     kInitialHeight             = 800;
constexpr INTERNET_PORT kDevServerPort       = 5174;
constexpr UINT_PTR    kStatsTimerId          = 0x100;  // 4 Hz stats broadcast
// [resize-perf] one-shot safety net: re-armed on every
// size tick while in sizemove; fires 150 ms after the ticks stop and
// re-resets ONLY if a per-tick cheap reset failed mid-gesture (normally
// a no-op — see LayoutBroker::SettleDeferredReset). Covers a lost
// WM_EXITSIZEMOVE too.
constexpr UINT_PTR    kResizeSettleTimerId   = 0x101;
constexpr UINT        kResizeSettleDelayMs   = 150;
// WebView2 crash recovery (WebViewCrashPolicy.h): the one-shot deadline for a
// Reload() to bring the page back, and the retry that moves a dead-web close
// out of a nested modal pump (a file dialog opened by a bridge request).
constexpr UINT_PTR    kWebReloadDeadlineTimerId = 0x102;
constexpr UINT_PTR    kWebDeadCloseRetryTimerId = 0x103;
constexpr UINT        kWebDeadCloseRetryMs      = 250;

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
inline double PerfUsSince(LONGLONG start)
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

// Per-run --record state. The record arm's ~460-line setup+gate
// used to inline in Run(); it now delegates to HostWindowImpl::SetupRecordArm,
// and these shared per-run values (formerly scattered Run() locals) travel in
// one struct. The hot-path capture/ack hooks stay HostWindowImpl-bound (they
// still capture [this]) — only this shared state moved out of Run().
struct RecordSession
{
    int          exitCode = 0;
    std::wstring tmpDir, outDir;
    std::shared_ptr<host::AsyncFrameEncoder> encoder;
    double       budgetMs = 0.0;
    LONGLONG     startQpc = 0;
    LONGLONG     freqQpc  = 0;
};

// DComp-present barrier policy (was Run()-scope; hoisted to file scope so the
// capture hook inside SetupRecordArm still sees it). Recorded verbatim in the
// pump trace (RecordTrace.h) as the CONFIGURED policy the adaptive loop must honor —
// weakening either value (cap 3->1, advance 2->1) grabs a composition cycle
// early and MUST change the trace, not just the runtime flush count.
constexpr int kBarrierPresents          = 3;   // DComp-present barrier / adaptive ceiling
constexpr int kBarrierCompositorAdvance = 2;   // early-exit: compositor passes past pre-present sample

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
    // WM_DESTROY (mirroring the webMessageTok lifecycle). The handlers
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
    // WebResourceRequested handler that serves app.local from the embedded
    // RCDATA bundle (prod only). Registered in FinishWebView2ControllerSetup,
    // removed in WM_DESTROY like the tokens above.
    EventRegistrationToken          webResourceTok = {};
    // ProcessFailed → OnWebProcessFailed (renderer/browser crash recovery).
    // Registered in FinishWebView2ControllerSetup, removed in
    // ReleaseHostComObjects like the tokens above.
    EventRegistrationToken          processFailedTok = {};
    // Every token above exists so WM_DESTROY can UNSUBSCRIBE a handler that
    // captures `this`. The two WebView2 CREATION callbacks have no token to
    // unsubscribe — they are one-shot completions the runtime owns — so they
    // get the liveness guard instead, retired at the top of WM_DESTROY and
    // checked before either callback touches `this`.
    host::StartupCallbackGuard      m_startupGuard;
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

    // host-state plumbing — the host owns the live
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

    // Undo / redo stack used by BridgeDispatcher to service `undo/perform`
    // requests and record emitter edits.
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
    // `g_spawnerLastFrameTime` flow in the legacy editor. First frame
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
    // editor's drag-state. On WM_LBUTTONDOWN /
    // WM_RBUTTONDOWN we snapshot the camera + cursor XY, then
    // WM_MOUSEMOVE deltas are applied relative to the snapshot
    // (matches legacy "drag relative to start" feel — releasing and
    // re-pressing resets the reference frame). NONE means no drag in
    // progress; the wheel handler only fires when dragMode == NONE.
    // OBJECT_Z: cursor-bound preview is being dragged for placement.
    // Only Z (height) tracks the drag delta; X/Y stay frozen at the
    // click position. WM_LBUTTONUP detaches the preview (place it).
    // Matches the legacy editor.
    // MANIPULATE: a manipulator handle (translate arrow or rotate
    // ring) was grabbed; LMB drag moves/rotates the object (wins over camera orbit
    // only when a handle is actually under the cursor at press).
    enum class DragMode { NONE, MOVE, ROTATE, ZOOM, OBJECT_Z, MANIPULATE };
    struct ViewportInteraction
    {
        DragMode        dragMode      = DragMode::NONE;
        Engine::Camera  dragStartCam  = {};
        int             dragStartX    = 0;
        int             dragStartY    = 0;
        // Manipulator drag state: the grabbed handle (kind + axis),
        // the transform snapshot at grab, and the no-jump anchors. TRANSLATE 
        // accumulates precision-scaled per-move axis-param deltas: each WM_MOUSEMOVE adds
        // (tNow - manipPrevT) * factor to manipAccumT (factor = 0.2 while Shift held,
        // else 1.0) and applies newPos = startPos + axis*manipAccumT. manipGrabT0 is
        // the axis param at press (seeds manipPrevT so the first move's delta is 0 -> no
        // jump). With factor==1 throughout, manipAccumT telescopes to (tNow - grabT0),
        // matching the old absolute-from-grab formula; a mid-drag Shift toggle only rescales
        // subsequent deltas (no jump, since manipPrevT tracks the raw param). ROTATE
        // accumulates wrapped, precision-scaled per-move ring angle deltas
        // (manipGrabAngle/Prev/Accum) onto the snapshot rotation.
        Engine::ManipHandle::Kind manipKind = Engine::ManipHandle::NONE;
        int             manipAxis        = -1;
        D3DXVECTOR3     manipStartPos    = D3DXVECTOR3(0, 0, 0);
        D3DXVECTOR3     manipStartRot    = D3DXVECTOR3(0, 0, 0);
        float           manipGrabT0      = 0.0f;
        float           manipPrevT       = 0.0f;   // translate accumulate-per-move: last raw axis param
        float           manipAccumT      = 0.0f;   // accumulated (precision-scaled) translate offset from grab
        float           manipPrevU       = 0.0f;   // plane drag: last raw in-plane U
        float           manipPrevV       = 0.0f;   //                     last raw in-plane V
        float           manipAccumU      = 0.0f;   //   accumulated (precision-scaled) U offset from grab
        float           manipAccumV      = 0.0f;   //   accumulated V offset from grab
        float           manipGrabAngle   = 0.0f;   // ring angle at grab (rad)
        float           manipPrevAngle   = 0.0f;   // previous-move ring angle (rad)
        float           manipAccumAngle  = 0.0f;   // accumulated rotation (rad)
        // Per-gesture latch: false until the FIRST per-move mutation
        // of a manipulator drag pushes its (one) pre-mutation undo point. A grab
        // that never moves the object captures nothing — no phantom undo step.
        bool            manipUndoCaptured = false;
        // ~30 Hz throttle for the per-move engine/state/changed emit (the snapshot is
        // heavy; the gizmo render still moves every frame via SetReferenceObjectTransform).
        DWORD           lastManipEmitTick = 0;

        // Readout pill scratch: each MANIPULATE branch fills these post-snap;
        // the throttle gate projects the gizmo origin and emits one event.
        std::string readoutKind;                 // "translate" | "plane" | "rotate"
        std::string readoutLabels[2];            // axis / euler names
        float       readoutValues[2] = {0,0};    // absolute values
        int         readoutN = 0;                // 1 or 2
        int         readoutDecimals = 1;         // 1 for units, 0 for degrees

        // lastCursorX/Y: cache of the most recent (x,y) seen by
        // WM_MOUSEMOVE. Used as the spawn coords on WM_KEYDOWN VK_SHIFT
        // because WM_KEYDOWN's lParam is NOT cursor coords (a legacy
        // main.cpp bug passed garbage). Fallback if the cache is
        // stale: GetCursorPos + ScreenToClient.
        int             lastCursorX = 0;
        int             lastCursorY = 0;
        // last GetTickCount() at which we pushed a
        // `cursor/position-3d` event. Throttled to ~30 Hz so the
        // WebView2 message channel isn't saturated by WM_MOUSEMOVE
        // (which fires per-pixel). The legacy status bar updates per
        // WM_MOUSEMOVE since SendMessage is free in-process; over the
        // bridge a 33 ms minimum interval is a good compromise.
        DWORD           lastCursorEmitTick = 0;
    };
    ViewportInteraction m_viewport;

    // Re-validate the cursor-bound Shift-preview borrow before ANY use. See the
    // m_attachedParticleSystem note below: Engine::Clear() frees the pointee
    // behind our back on three paths that never reset this slot. Returns nullptr
    // and self-heals the slot when the borrow has gone stale, so a caller can
    // neither act on a freed instance nor be blocked forever by a non-empty
    // handle that will never clear on its own. Factored out for the same
    // reason as ResetManipDragState below -- so a new use site can't forget it.
    ParticleSystemInstance* LiveAttachedSystem()
    {
        if (!m_attachedParticleSystem || !engine) return nullptr;
        ParticleSystemInstance* live =
            engine->ResolveInstance(m_attachedParticleSystem);
        if (!live)
        {
            m_attachedParticleSystem.Reset();   // stale borrow: drop it
            return nullptr;
        }
        return live;
    }

    // The invariant tail every MANIPULATE drag-end shares: drop the grabbed handle, zero the
    // accumulators, and clear the engine's active-drag (guide/sweep/dim) state. Per-site Commit /
    // ReleaseCapture / m_viewport.dragMode handling stays at the call site -- only this shared tail is factored
    // out so a new end-site can't forget the active-drag clear (the bug WM_KILLFOCUS originally had).
    void ResetManipDragState()
    {
        m_viewport.manipAxis = -1;
        m_viewport.manipKind = Engine::ManipHandle::NONE;
        m_viewport.manipAccumT = 0.0f;
        m_viewport.manipAccumAngle = 0.0f;
        m_viewport.manipAccumU = 0.0f;
        m_viewport.manipAccumV = 0.0f;
        m_viewport.manipUndoCaptured = false;   // next grab starts a fresh gesture
        if (engine) engine->SetManipulatorActiveDrag(Engine::ManipHandle(), 0.0f, 0.0f);
        // hide the readout pill (ResetManipDragState is called from the 4
        // capture-drag-end sites: LBUTTONUP, RBUTTONDOWN, CAPTURECHANGED, KILLFOCUS).
        if (dispatcher) dispatcher->EmitManipulatorDrag({ {"active", false} });
    }

    // shift-click-to-spawn. Mirror of the legacy editor's
    // `info->mouseCursor` + `info->attachedParticleSystem`.
    //
    // m_mouseCursor: Object3D whose position is set from screen-space
    // mouse moves (WM_MOUSEMOVE → GetCursorPos3D unproject) and whose
    // velocity is derived from QueryPerformanceCounter deltas in
    // UpdateVelocity() (called once per RenderD3D9).
    //
    // m_attachedParticleSystem: non-empty between Shift-press (spawn) and
    // Shift-release (kill). Its pointer + immutable token are retained until
    // KillParticleSystem consumes the live identity.
    //
    // It is a tokenized BORROW of an Engine::m_instances entry. Engine::Clear()
    // frees every instance without telling us. file/new + file/open + recover
    // + undo-apply null this slot themselves (BridgeDispatch_File.cpp,
    // BridgeDispatcher.cpp), but three paths reach Clear() without doing so:
    // engine/action/clear, the SetEstimatedLoad overload hard-guard, and a
    // gate-refused SpawnParticleSystem. Read it through LiveAttachedSystem()
    // rather than directly — a stale pointer otherwise blocks every future
    // Shift-spawn (the non-null precondition never clears) and puts LMB-down
    // into a placement drag for an instance that no longer exists.
    //
    // Shared with RenderD3D9 and the dispatcher; keep these at host scope.
    MouseCursor             m_mouseCursor;
    ParticleSystemInstanceHandle m_attachedParticleSystem;

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
    // spike's Shutdown sequence in spike/dxgi_spike.cpp).
    std::unique_ptr<host::Compositor>          m_compositor;
    // Latches a posted runtime composition fatal so no later render tick can
    // submit more D3D11 work before the message-loop handler exits.
    bool                                      m_compositionFatalPending = false;
    // Persistent for this host session; replayed after every page reload.
    std::string                               m_viewportUnavailableReason;
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
    enum class RunMode { Interactive, Capture, Drive, Record };
    const RunMode m_runMode;
    // --capture mode: load m_captureAlo,
    // render m_captureFrames frames, write engine RT to m_capturePng,
    // then quit. These are capture inputs; m_runMode selects the pump branch.
    std::wstring m_captureAlo;
    // --capture-ref <objectName>: render a game reference object (with its
    // shadow) headlessly instead of a particle system. Mutually exclusive
    // with m_captureAlo in practice; the capture branch checks it first.
    std::wstring m_captureRef;
    std::wstring m_capturePng;
    int          m_captureFrames = 180;
    // --skydome <slot>: apply this skydome slot in --capture mode before
    // rendering (0 = Off / solid colour, the default).
    int          m_captureSkydomeSlot = 0;
    // Internal render-oracle profile: ignore persisted view/camera state and
    // require CaptureRunner to establish the canonical capture view.
    bool         m_captureGoldenProfile = false;
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
    // --drive and --record share persistence/profile/log isolation, but have
    // separate run-loop branches. --test-host remains an orthogonal flag.
    std::wstring m_driveScriptPath;
    std::wstring m_recordScriptPath;
    bool IsAutomationMode() const
    {
        return m_runMode == RunMode::Drive || m_runMode == RunMode::Record;
    }
    // True only when a human is present to dismiss a blocking modal — i.e. NOT in
    // any headless mode (capture / drive-or-record / test-host). Every fatal or
    // preflight MessageBoxW is gated on this so a headless run never hangs on a
    // dialog nobody can click (the log line + non-zero exit still carry the error).
    bool IsFullyInteractive() const
    {
        return IsFullyInteractiveSession(
            m_runMode == RunMode::Capture, IsAutomationMode(), useTestHost);
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
    // (permanent instrumentation). The four segments
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
        // adaptive-barrier accounting: total DwmFlush presents across
        // the run (avg = total/frames in the summary) and whether the
        // composition-timing probe ever failed (permanent fixed-3 fallback
        // must be VISIBLE, not silent).
        unsigned barrierFlushTotal   = 0;
        bool     barrierProbeFailed  = false;
    };
    RecordTiming m_recordTiming;
    // --record pump-schedule trace: non-null only under PE_RECORD_TRACE.
    // Owned here (outlives m_clipRunner within Run); the capture lambda emits its
    // barrier/grab tokens and the runner emits step/tick/ack + EndFrame, both
    // into this one sink. See RecordTrace.h.
    std::unique_ptr<host::RecordTrace> m_recordTrace;
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
    // (This replaced the old CapturePreview + CPU-composite path.) Env gate
    // PE_RECORD_HEADLESS=1. Probed once; see the ack + capture hooks.
    bool m_recordHeadless = [] {
        wchar_t b[8] = {};
        return GetEnvironmentVariableW(L"PE_RECORD_HEADLESS", b, 8) > 0
            && b[0] != L'0';
    }();
    // --record-minimized: run the record window OUT OF SIGHT during a headless
    // render so the machine is free. This moves the window OFFSCREEN
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
    // withheld ack is never silently captured.
    bool m_headlessAckOk = true;
    // End-of-run summary (also emitted on the record watchdog so a timed-out
    // run still yields its measurement). p99/max reported beside p95 because
    // the 2000 ms ack deadline is fatal at p100, not p95.
    // Defined in-class: the enclosing region around the file's later helpers
    // is an anonymous namespace, where a host::HostWindowImpl member can't be
    // defined (C2888). (std::min) parenthesized against windows.h's min macro.
    // Snapshot of the encoder's back-pressure stats, taken just before
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
        // Adaptive-barrier proof line: avg flushes/frame (fixed-3 was
        // the old behavior; ~2.0 = adaptive working) + loud fallback flag.
        if (!rt.barrier.empty() || rt.barrierFlushTotal > 0)
            Log("[record-timing] barrier-adaptive avg=%.2f flushes/frame%s\n",
                rt.frame.empty() ? 0.0
                    : static_cast<double>(rt.barrierFlushTotal)
                      / static_cast<double>(rt.frame.size()),
                rt.barrierProbeFailed ? "  (PROBE FAILED — fixed-3 fallback)" : "");
        // Encoder back-pressure: time the grab thread spent BLOCKED on
        // the 128 MB queue cap (silently folded into png/capture before) +
        // queue high-water marks — the second-worker/faster-encoder
        // decision datum.
        if (m_recordEncoderStats.waitCount > 0 || m_recordEncoderStats.depthHighWater > 0)
            Log("[record-timing] queue    blocked=%8.0fms x%u  hw=%zuMB depth=%zu\n",
                m_recordEncoderStats.waitMsTotal, m_recordEncoderStats.waitCount,
                m_recordEncoderStats.bytesHighWater / (1024 * 1024),
                m_recordEncoderStats.depthHighWater);
    }

    // Frame-pacing budget from the monitor the window actually sits on
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

    // Deferred-autosave latch (see the WM_TIMER autosave note): the
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
        // Tell the UI. `wrote` previously fed nothing but
        // the format string above, so a failing autosave was invisible outside a
        // debug log: the user kept editing believing the recovery net was live
        // when the newest recoverable state was silently falling behind.
        EmitAutosaveHealthIfChanged(wrote);
    }

    // Autosave health is a persistent CONDITION, not an event — it stays broken
    // until a write succeeds — so emit only on a CHANGE and let the web hold it
    // as durable state. Mirrors EmitWindowStateIfChanged, including the
    // not-yet-wired case that app/ready replays. -1 = not yet sent.
    int m_lastAutosaveHealthySent = -1;
    void EmitAutosaveHealthIfChanged(bool healthy)
    {
        if (!dispatcher) return;
        const int cur = healthy ? 1 : 0;
        if (cur == m_lastAutosaveHealthySent) return;
        if (dispatcher->EmitAutosaveHealth(healthy)) m_lastAutosaveHealthySent = cur;
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
                   const HostLaunchOptions& options)
        : hInstance(inst)
        , textureManager(tex)
        , shaderManager(shd)
        , fileManager(fil)
        , useDevUi(options.useDevUi)
        , useTestHost(options.useTestHost)
        // main.cpp rejects capture/drive/record combinations. For direct callers,
        // preserve the old pump precedence: Drive, then Record, then Capture.
        // Both capture inputs are allowed; CaptureRunner still prefers captureRef.
        , m_runMode(!options.driveScriptPath.empty() ? RunMode::Drive
                    : !options.recordScriptPath.empty() ? RunMode::Record
                    : (!options.captureAlo.empty() || !options.captureRef.empty())
                        ? RunMode::Capture : RunMode::Interactive)
        , m_captureAlo(options.captureAlo)
        , m_captureRef(options.captureRef)
        , m_capturePng(options.capturePng)
        , m_captureFrames(options.captureFrames)
        , m_captureSkydomeSlot(options.captureSkydome)
        , m_captureGoldenProfile(options.captureGoldenProfile)
        , m_captureHasAmbient(options.hasAmbient)
        , m_captureHasSun(options.hasSun)
        , m_captureHasSunI(options.hasSunIntensity)
        , m_captureSunIntensity(options.sunIntensity)
        , m_driveScriptPath(options.driveScriptPath)
        , m_recordScriptPath(options.recordScriptPath)
        , m_perfWebViewProfile(options.perfWebViewProfile)
        , layout(nullptr)
        , accelerator()
        // Persistence is suppressed in every headless mode — the same set
        // IsFullyInteractive() excludes (capture / drive-or-record / test-host),
        // except that ALO_SETTINGS_LIVE lifts the test-host gate exactly as it
        // does for the dispatcher's settings and mods/set-layers writes. None of
        // these runs may rewrite the daily driver's persisted mod stack: with a
        // mod folder temporarily unavailable (unmounted drive), a capture run
        // would otherwise ghost-drop those layers and PERSIST the reduced stack.
        // Startup RestoreLastLayerStack never writes
        // regardless. Computed from the ctor arguments, not the m_* flags: this
        // member is declared (so initialized) before them.
        , modManager(std::make_unique<ModManager>(&fil, gameRoots_,
              /*suppressPersistence=*/!IsFullyInteractiveSession(
                  !options.captureAlo.empty() || !options.captureRef.empty(),
                  !options.driveScriptPath.empty() || !options.recordScriptPath.empty(),
                  options.useTestHost && !ReadSettingsLiveEnv())))
    {
        // [world-lit] capture lighting colours (arrays can't init in list).
        m_captureAmbient[0] = options.ambient[0]; m_captureAmbient[1] = options.ambient[1]; m_captureAmbient[2] = options.ambient[2];
        m_captureSun[0] = options.sun[0]; m_captureSun[1] = options.sun[1]; m_captureSun[2] = options.sun[2];
        // Automation must isolate the palette BEFORE the saved mod stack is
        // restored: RestoreLastLayerStack activates a mod, which otherwise
        // loads the user's persisted pins/recents before OpenLog/Run begins.
        if (IsAutomationMode())
            TexturePalette::Store::Instance().SetEphemeral(true);
        // discover installed mods and restore the
        // previously-active one from the registry before any UI shows.
        // Both calls are quick; they don't touch GPU / WebView2 state.
        // Engine pointer is bound later via SetEngine() in WM_CREATE.
        modManager->DiscoverMods();
        // A golden capture resolves content unmodded — see
        // ShouldRestorePersistedModLayers. Skipping the restore (rather than
        // clearing and re-writing) means the daily driver's stack is never
        // touched, so a killed or timed-out capture can't strand it.
        if (ShouldRestorePersistedModLayers(m_captureGoldenProfile))
            modManager->RestoreLastLayerStack();
    }

    void Log(const char* fmt, ...);
    void OpenLog();
    bool RunDriveSelftest(const std::string& kind, int timeoutMs);

    // The --record arm's one-time setup — timeline parse + mod
    // check + startup gate (resize/pause/open/catalog/settles) + hook wiring.
    // Moved out of Run() (was a ~460-line inline block). Returns true if the
    // run should quit (bad timeline exit 2 / failed open exit 3 / unsafe
    // <out>.tmp exit 4); on success builds m_clipRunner. Shared per-run state
    // travels in `rec`.
    bool SetupRecordArm(RecordSession& rec);
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
    void MarkViewportUnavailable(const char* reason, HRESULT hr);
    void EmitViewportUnavailable();

    // WebView2 process-failure recovery. The decisions
    // live in WebViewCrashPolicy.h; this is the wiring. m_webDead is the one
    // flag the rest of the host reads: WM_CLOSE stops vetoing (the page that
    // would answer the save prompt is gone), the pump ends a headless run with
    // kWebProcessFailedExitCode, and an interactive session closes through
    // CloseAfterWebDeath.
    bool m_webDead              = false;
    int  m_webReloadsUsed       = 0;      // capped at kMaxWebReloads per session
    bool m_webReloadPending     = false;  // Reload() issued, the page's app/ready not yet seen
    int  m_webHangReports       = 0;      // consecutive hang reports with no web message between
    int  m_webMessageDepth      = 0;      // OnWebMessage frames on the stack (a handler's modal pumps)
    std::wstring m_appNavUrl;             // the app URL the first Navigate used; recovery navigates here
    bool m_webDeadCloseStarted  = false;  // CloseAfterWebDeath runs once (its modal pumps)
    bool m_keepAutosaveSession  = false;  // WM_DESTROY skips DeleteOurSession
    void OnWebProcessFailed(ICoreWebView2ProcessFailedEventArgs* args);
    // Sets m_webDead and hands the close (interactive) or the abort (headless)
    // to the message loop via WM_APP_WEB_DEAD.
    void MarkWebDead(const char* why);
    // Interactive close with a dead web: write the recovery copy if dirty,
    // keep the autosave session for the next launch, tell the user once, and
    // destroy the window.
    void CloseAfterWebDeath(HWND hwnd);

    // One idempotent release of every WebView2 / composition / engine COM
    // object. Called from WM_DESTROY and again at the end of Run()
    // so automation exits — which leave the pump without destroying hMain —
    // also release everything before CoUninitialize.
    void ReleaseHostComObjects();

    LRESULT MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT ViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    int Run(int nCmdShow);
};

} // namespace host

#endif // HOST_HOST_WINDOW_IMPL_H
