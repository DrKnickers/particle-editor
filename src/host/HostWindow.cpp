// HostWindow — see HostWindow.h for the design overview.
//
// Most of this file is a port of src/host/viewport_poc.cpp, split into
// instance methods on a singleton-style HostWindow + Impl pair. The PoC
// proved the composition pattern (WebView2 surface set transparent, D3D9
// sibling child HWND layered on top, layout/viewport-rect drives
// SetWindowPos). We carry those decisions forward verbatim.
//
// The HostWindowImpl struct itself is declared in HostWindowImpl.h; its
// WebView2, viewport and --record methods live in HostWindow_WebView2.cpp,
// HostWindow_Viewport.cpp and HostWindow_Record.cpp.
//
// IMPORTANT: every host-window TU sets _WIN32_WINNT to 0x0A00 (Windows 10)
// before including windows.h; WebView2 + DPI awareness need a modern target.
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "HostWindowImpl.h"

namespace host {

namespace {

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

// Absolute path of the Evergreen bootstrapper IF one sits beside the exe, or
// empty otherwise. The release no longer bundles it (the shipped build is a
// self-contained exe + d3dx9_43.dll), so in practice this is empty and the
// caller opens the download page; a bootstrapper a user drops in manually is
// still honored.
//
// The grow-until-it-fits GetModuleFileNameW loop that used to live inline here
// now lives in host::ModuleDirectory (ModulePath.h) — the fixed-MAX_PATH form
// it replaced truncated silently under a long extraction path (2026-07 audit).
static std::wstring BundledWebView2SetupPath()
{
    const std::wstring dir = host::ModuleDirectory();
    if (dir.empty()) return std::wstring();
    std::filesystem::path p =
        std::filesystem::path(dir) / L"MicrosoftEdgeWebview2Setup.exe";
    return (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) ? p.wstring()
                                                                     : std::wstring();
}

// The runtime is missing (or unusable). Offer to fix it, in plain language.
//
// The release no longer bundles Microsoft's bootstrapper, so in a shipped build
// `setup` is empty and this opens the download page (https://aka.ms/webview2).
// A bootstrapper manually placed beside the exe is still honored for a one-click
// install.
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

// UTF-8 ↔ UTF-16 conversions now live in StringConv.h (host::Utf8ToWide /
// WideToUtf8), shared with BridgeDispatcher + HostBridgeProxy (DRY audit cpp-host-0).

LRESULT CALLBACK HostMainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
LRESULT CALLBACK HostViewportWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

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

// --drive bridge-selftest: round-trip allowlisted requests through BOTH
// PRODUCTION page->host WebMessage doors. CDP makes WebView2 drop postMessage,
// so this non-CDP --drive path is the only composed oracle for the string and
// JSON-fallback callbacks. It proves the shipped inclusive ingress boundary:
// exactly 16 Mi UTF-16 chars dispatch; one more never reaches OnWebMessage.
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
    // are safe to embed in single-quoted JS. The literal 16777216 is independent
    // of ResourceLimits.h so an accidental shipped-default change cannot move
    // this oracle with the implementation.
    //
    // A faithful inline mini-client of the production wire protocol: the same
    // {type:"req",id,kind,params} envelope NativeBridge sends. Posting the
    // serialized string exercises TryGetWebMessageAsString; posting the object
    // exercises get_WebMessageAsJson. Exact-cap responses prove each transport
    // path is alive. Any correlated cap+1 response is the specific forbidden
    // value; a bounded quiet period is success for that case. setTimeout(0)
    // defers out of ExecuteScript, where postMessage throws 0x80070490.
    const std::string js =
        "setTimeout(function(){"
        "var token='" + token + "',kind='" + kind + "',cap=16777216;"
        "var wv=window.chrome&&window.chrome.webview;"
        "var reported=false,index=0,current=null,timer=0,positiveMs={},forbidden={};"
        "var post=function(ok,why){if(reported)return;reported=true;"
        "try{wv.postMessage(JSON.stringify({kind:'drive/selftest-result',"
        "token:token,ok:!!ok,why:why||''}));}catch(e){}};"
        "if(!wv||typeof wv.postMessage!=='function'){post(false,'no-webview');return;}"
        "var cases=["
        "{door:'string',size:cap,expect:true,label:'string-at-cap'},"
        "{door:'string',size:cap+1,expect:false,label:'string-one-past'},"
        "{door:'json',size:cap,expect:true,label:'json-at-cap'},"
        "{door:'json',size:cap+1,expect:false,label:'json-one-past'}];"
        "var build=function(c,id){"
        "var obj={type:'req',id:id,kind:kind,params:{padding:''}};"
        "var base=JSON.stringify(obj),n=c.size-base.length;"
        "if(n<0)throw new Error('target-too-small');"
        "obj.params.padding='x'.repeat(n);"
        "var raw=JSON.stringify(obj);"
        "if(raw.length!==c.size)throw new Error('wrong-size:'+raw.length);"
        "return{obj:obj,raw:raw};};"
        "var next=function(){"
        "clearTimeout(timer);"
        "if(reported)return;"
        "if(index>=cases.length){post(true,'');return;}"
        "var c=cases[index++];"
        "c.id='selftest-'+token+'-'+c.label;current=c;"
        "var built;try{built=build(c,c.id);}catch(e){post(false,c.label+': '+e.message);return;}"
        "if(!c.expect)forbidden[c.id]=c.label;"
        "c.started=Date.now();"
        "if(c.expect){timer=setTimeout(function(){"
        "post(false,c.label+': response-timeout');},10000);}"
        "else{var wait=Math.min(10000,Math.max(4000,(positiveMs[c.door]||0)*4));"
        "timer=setTimeout(function(){if(current===c)next();},wait);}"
        "try{wv.postMessage(c.door==='string'?built.raw:built.obj);}"
        "catch(e){post(false,c.label+': post-threw: '+(e&&e.message||''));}};"
        "var onMsg=function(e){var m=e.data;"
        "if(typeof m==='string'){try{m=JSON.parse(m);}catch(err){return;}}"
        "if(!m||m.type!=='res'||typeof m.id!=='string')return;"
        "if(forbidden[m.id]){post(false,forbidden[m.id]+': over-cap dispatched');return;}"
        "if(!current||m.id!==current.id)return;"
        "clearTimeout(timer);"
        "positiveMs[current.door]=Date.now()-current.started;"
        "if(!m.ok){post(false,current.label+': res-error: '+(m.error||''));return;}"
        "next();};"
        "wv.addEventListener('message',onMsg);"
        "next();"
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
// or the WebView2 composition controller can't be brought up or kept alive,
// the viewport would be a permanent black window, so we surface a clear modal
// error and exit cleanly instead. Reached from synchronous setup failures and
// the WM_APP_COMPOSITION_FATAL handler. host.log is flushed first so the
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
            L"Particle Editor could not initialize or continue using its "
            L"DirectComposition rendering surface (error 0x%08lX).\n\n"
            L"This build renders the viewport through DirectComposition + WebView2 "
            L"composition hosting and cannot run without it. Make sure your GPU "
            L"drivers are up to date and that the WebView2 runtime is installed. "
            L"Restart the editor to recreate the rendering device.\n\n"
            L"The editor will now close.",
            static_cast<unsigned long>(hr));
        MessageBoxW(nullptr, msg, L"Particle Editor — composition unavailable",
                    MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}

// ---------- WebView2 process failure (2026-10-01 audit HX1) ----------

// ProcessFailed handler. Runs on the UI thread inside a WebView2 callback, so
// it only decides, logs, and either calls Reload() or posts WM_APP_WEB_DEAD —
// the modal and the window teardown happen on the message loop.
void HostWindowImpl::OnWebProcessFailed(ICoreWebView2ProcessFailedEventArgs* args)
{
    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
    if (args) args->get_ProcessFailedKind(&kind);
    int exitCode = 0;
    int reason   = -1;
    ComPtr<ICoreWebView2ProcessFailedEventArgs2> args2;
    if (args && SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) && args2)
    {
        COREWEBVIEW2_PROCESS_FAILED_REASON r = COREWEBVIEW2_PROCESS_FAILED_REASON_UNEXPECTED;
        if (SUCCEEDED(args2->get_Reason(&r))) reason = static_cast<int>(r);
        args2->get_ExitCode(&exitCode);
    }
    Log("[webview] ProcessFailed kind=%d reason=%d exitCode=%d (dead=%d reloads=%d pending=%d)\n",
        static_cast<int>(kind), reason, exitCode,
        m_webDead ? 1 : 0, m_webReloadsUsed, m_webReloadPending ? 1 : 0);
    if (m_webDead) return;   // already handled; the close / exit is under way

    webviewcrash::WebFailure failure = webviewcrash::WebFailure::Other;
    switch (kind)
    {
    case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
        failure = webviewcrash::WebFailure::BrowserExited; break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
        failure = webviewcrash::WebFailure::RenderExited; break;
    case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE:
        failure = webviewcrash::WebFailure::RenderUnresponsive; break;
    default:
        break;
    }

    // Hang reports repeat every few seconds while a hang lasts; OnWebMessage
    // resets the count, since any message proves the renderer is answering.
    if (failure == webviewcrash::WebFailure::RenderUnresponsive)
        ++m_webHangReports;

    webviewcrash::WebFailureAction action = webviewcrash::DecideWebFailure(
        failure, IsFullyInteractive(), m_webReloadsUsed, m_webReloadPending,
        m_webHangReports);

    if (action == webviewcrash::WebFailureAction::Reload)
    {
        // Navigate to the app's own URL rather than Reload(): a renderer that
        // dies before the first navigation commits leaves about:blank as the
        // current document, and reloading THAT never boots the app (seen in
        // a manual crash test: navigation "succeeds", no app/ready).
        const HRESULT hr = !webView           ? E_POINTER
                         : m_appNavUrl.empty() ? webView->Reload()
                                               : webView->Navigate(m_appNavUrl.c_str());
        if (SUCCEEDED(hr))
        {
            ++m_webReloadsUsed;
            m_webReloadPending = true;
            m_webHangReports = 0;
            // The page must report in (app/ready) before the deadline, or
            // the web is declared dead — a still-hung renderer can't commit
            // the reload, and its later hang reports are skipped as pending.
            if (hMain) SetTimer(hMain, kWebReloadDeadlineTimerId,
                                webviewcrash::kWebReloadDeadlineMs, nullptr);
            Log("[webview] reloading the UI (%d of %d)\n",
                m_webReloadsUsed, webviewcrash::kMaxWebReloads);
            return;
        }
        Log("[webview] Reload failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
        action = webviewcrash::WebFailureAction::MarkDead;
    }

    if (action == webviewcrash::WebFailureAction::LogOnly)
    {
        Log("[webview] no action (WebView2 recovers this kind itself, or a run "
            "watchdog bounds it)\n");
        return;
    }

    MarkWebDead("process failure");
}

void HostWindowImpl::MarkWebDead(const char* why)
{
    if (m_webDead) return;
    m_webDead = true;
    m_webReloadPending = false;
    if (hMain) KillTimer(hMain, kWebReloadDeadlineTimerId);
    Log("[webview] web layer DEAD (%s) — %s\n", why,
        IsFullyInteractive() ? "closing through the native recovery path"
                             : "ending the headless run");
    if (hMain) PostMessageW(hMain, WM_APP_WEB_DEAD, 0, 0);
}

// Interactive close once the web is dead. Nothing here may wait on the page:
// it is gone, so its Save/Discard/Cancel prompt is replaced by a recovery copy
// plus one native notice. Runs once — the MessageBoxW below pumps messages, and
// a second WM_CLOSE (Alt-F4 again) must not re-enter.
void HostWindowImpl::CloseAfterWebDeath(HWND hwnd)
{
    if (m_webDeadCloseStarted) return;
    // A bridge handler is still on the stack, pumping a modal (a file dialog
    // opened by the page before it died). Destroying the window from inside
    // that pump would release the engine under the handler, so wait for the
    // outermost loop. Polled by timer: re-posting the message would spin.
    if (m_webMessageDepth > 0)
    {
        SetTimer(hwnd, kWebDeadCloseRetryTimerId, kWebDeadCloseRetryMs, nullptr);
        return;
    }
    KillTimer(hwnd, kWebDeadCloseRetryTimerId);
    m_webDeadCloseStarted = true;

    // The modal below pumps WM_TIMER: stop the autosave ticks first so an
    // unverified timer write can't replace the verified recovery copy.
    KillTimer(hwnd, Autosave::RECENT_TIMER_ID);
    KillTimer(hwnd, Autosave::STABLE_TIMER_ID);
    m_autosavePending = false;

    const bool dirty = dispatcher && dispatcher->GetDirty();
    const webviewcrash::DeadWebClosePlan plan =
        webviewcrash::PlanDeadWebClose(dirty, IsFullyInteractive());

    bool handoffOk = false;
    if (plan.writeRecoveryHandoff && dispatcher && particleSystem)
    {
        handoffOk = Autosave::WriteRecoveryHandoff(*particleSystem,
                                                   dispatcher->GetCurrentFilePath());
        Log("[webview] dead-web close: recovery copy %s\n",
            handoffOk ? "written" : "write FAILED (older autosaves kept)");
    }
    // Keep the session even when the handoff failed: the timer-written tiers
    // are older, but still better than nothing.
    m_keepAutosaveSession = plan.keepAutosaveSession;

    if (plan.notifyUser)
    {
        const wchar_t* detail = !dirty
            ? L"There were no unsaved changes."
            : handoffOk
                ? L"Your unsaved changes were saved for recovery. The next time you "
                  L"start Particle Editor it will offer to restore them."
                : L"Your unsaved changes could not be saved for recovery. The next "
                  L"time you start Particle Editor it will offer the most recent "
                  L"autosave, if there is one.";
        wchar_t msg[640];
        _snwprintf_s(msg, _TRUNCATE,
            L"The editor's interface (WebView2) stopped working and could not be "
            L"restarted.\n\n%s\n\nThe editor will now close.", detail);
        MessageBoxW(hwnd, msg, L"Particle Editor — interface stopped",
                    MB_OK | MB_ICONERROR);
    }
    DestroyWindow(hwnd);
}

// One idempotent teardown for every COM object the host holds (2026-09-30 audit MH1).
// WM_DESTROY calls it on an interactive close; Run() calls it again after the
// pump so an automation exit (which never destroys hMain) also releases
// everything BEFORE CoUninitialize — a COM Release after CoUninitialize is
// undefined. Every step null-checks, so the second call is a no-op.
void HostWindowImpl::ReleaseHostComObjects()
{
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
        if (webResourceTok.value != 0)
        {
            webView->remove_WebResourceRequested(webResourceTok);
            webResourceTok = {};
        }
        if (docTitleTok.value != 0)
        {
            webView->remove_DocumentTitleChanged(docTitleTok);
            docTitleTok = {};
        }
        if (processFailedTok.value != 0)
        {
            webView->remove_ProcessFailed(processFailedTok);
            processFailedTok = {};
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
    // Release the WebView2 environment here too, while COM is still live.
    // It is a member ComPtr used by the WebResourceRequested handler; if left
    // to HostWindowImpl's destructor its final Release() would run AFTER the
    // CoUninitialize() at the end of Run() — a COM call past teardown.
    webEnv.Reset();
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
    // The Controller4 QI is a second reference to the same controller; left
    // alone it kept the controller alive past the Compositor release below
    // (and past CoUninitialize), defeating the order above.
    m_ncRegionEnabled = false;
    m_compositionController4.Reset();
    m_compositionController.Reset();
    // Clear LayoutBroker's pointer BEFORE
    // releasing the Compositor so any late SetSceneRect dispatch
    // (e.g. an in-flight BridgeDispatcher message that's already
    // past the WM_DESTROY barrier in the message-pump shutdown
    // sequence) doesn't dereference a freed Compositor.
    layout.SetCompositor(nullptr);
    if (engine) engine->SetCompositionCompositor(nullptr);
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
    if (!engine || m_compositionFatalPending) return;
    // The composed editor has no D3D9 Present result. This admission door
    // consumes any suspect state raised by the previous frame's event-query
    // failure before SpawnerDriver, Update, or Render can touch D3D resources.
    // A healthy frame performs no CheckDeviceState probe.
    if (!engine->PrepareComposedFrame()) return;

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
    const bool rendered = engine->Render();
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
    if (rendered && m_compositor && m_compositor->IsReady())
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
        const ComposedFrameResult compositeResult =
            m_compositor->CompositeEngineFrame(engine->GetSharedTextureHandle());
        // Present1 belongs to the D3D11 composition device, not the engine's
        // D3D9Ex device. Exact DXGI device-removal states therefore enter the
        // composition fatal/restart path with zero D3D9 recovery attempts.
        // Shared-handle failures remain retryable even if they carry the same
        // numeric HRESULT; CompositeEngineFrame clears its cached handle.
        if (ClassifyComposedFrameResult(compositeResult) ==
            ComposedFrameAction::RestartRequired)
        {
            m_compositionFatalPending = true;
            Log("[host] composition: Present1 requires restart hr=0x%08lx\n",
                static_cast<unsigned long>(compositeResult.hr));
            if (!PostMessageW(hMain, WM_APP_COMPOSITION_FATAL,
                              static_cast<WPARAM>(compositeResult.hr), 0))
            {
                FailFatalComposition(compositeResult.hr);
            }
        }
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

static void ApplyRestoredSettings(Engine* engine, const host::RestoredSettings& s)
{
    if (s.bloomEnabled)  engine->SetBloom(*s.bloomEnabled);
    if (s.bloomStrength) engine->SetBloomStrength(*s.bloomStrength);
    if (s.bloomCutoff)   engine->SetBloomCutoff(*s.bloomCutoff);
    if (s.bloomSize)     engine->SetBloomSize(*s.bloomSize);

    if (s.backgroundColor) engine->SetBackground(*s.backgroundColor);
    if (s.showGround)      engine->SetGround(*s.showGround);
    engine->SetGroundZ(0.0f);

    // Ground texture: per-slot custom paths BEFORE the selected index, so
    // SetGroundTexture can find the right source for a custom slot (ordering is
    // load-bearing).
    for (const auto& slot : s.groundSlotPaths)
        engine->SetGroundSlotCustomPath(slot.first, slot.second);
    if (s.groundSolidColor) engine->SetGroundSolidColor(*s.groundSolidColor);
    if (s.groundTexture)    engine->SetGroundTexture(*s.groundTexture);

    // Skydome: custom paths first so SetSkydomeSlot can reload a previously-active
    // custom slot.
    for (const auto& slot : s.skydomeCustomPaths)
        engine->SetSkydomeCustomPath(slot.first, slot.second);
    if (s.skydomeSlot) engine->SetSkydomeSlot(*s.skydomeSlot);

    // Game-dome environment: battle context + the two chosen GameObject Names.
    // This restore block runs after the device is up, so SetSkydomeEnvironment
    // resolves + uploads the meshes now (the only place the new UI re-resolves a
    // name-based selection).
    if (s.hasSkydomeEnv)
    {
        engine->SetSkydomeEnvironment(
            s.skydomeContextRaw == 0 ? SkydomeContext::Land : SkydomeContext::Space,
            WideToAnsi(s.skydomePrimaryName), WideToAnsi(s.skydomeSecondaryName));
    }

    // Imported reference object + unit grid. At startup the catalog isn't built
    // yet, so SetReferenceObject DEFERS: it kicks the off-thread catalog build
    // and the mesh resolves/uploads a frame or more later, once Update() harvests
    // the catalog and reruns the deferred rebuild (so a restored object isn't on
    // the first frame). Transform / grid spacing are REG_BINARY floats;
    // visibility / grid toggle / snap toggle are REG_DWORD.
    if (s.refTransform)
    {
        const std::array<float, 6>& xform = *s.refTransform;
        engine->SetReferenceObjectTransform(
            D3DXVECTOR3(xform[0], xform[1], xform[2]),
            D3DXVECTOR3(xform[3], xform[4], xform[5]));
    }
    if (s.refVisible)   engine->SetReferenceObjectVisible(*s.refVisible);
    if (s.gridVisible)  engine->SetGridVisible(*s.gridVisible);
    if (s.gridSpacing)  engine->SetGridSpacing(*s.gridSpacing);
    // Persistent gizmo snap toggle (REG_DWORD, like GridVisible).
    if (s.snapEnabled)  engine->SetSnapEnabled(*s.snapEnabled);
    // Restore the persisted lock so a frozen object comes back frozen. (Ordering
    // vs. the Name read isn't load-bearing: the silent restore force-deselects
    // below regardless, so the object lands deselected either way — the lock flag
    // just needs to be set before the user can interact.)
    if (s.refLocked) engine->SetReferenceLocked(*s.refLocked);
    // Name LAST so the mesh loads once with the transform in place; guard on
    // non-empty so an unset selection doesn't clobber a debug
    // ALO_LT7_TEST_OBJECT env-hook mesh.
    //
    // In headless --capture mode NEVER restore the persisted reference object:
    // the capture supplies its own object (the ALO_LT7_TEST_OBJECT env hook, or
    // --capture-ref via SetReferenceObject below), and restoring here would both
    // clobber that mesh AND force the capture script to mutate the registry to
    // suppress it — which, if the script is interrupted, wipes the user's saved
    // selection. Skipping makes captures registry-inert and crash-safe by
    // construction.
    if (s.refName)
    {
        engine->SetReferenceObject(WideToAnsi(*s.refName));
        // A silent startup restore is NOT a pick -- load the object inert so the
        // gizmo + selection box appear only when the user clicks its body
        // (honours the selection-gating).
        engine->SetReferenceObjectSelected(false);
    }

    // [lighting-restore, session 12] Restore the persisted lighting (sun /
    // fill1 / fill2 angles + colours + intensities, ambient, shadow) so the
    // new-UI viewport opens with the user's saved lights instead of engine ctor
    // defaults. Mirrors the legacy `PushLightingToEngine` (native Win32 UI,
    // since removed) field-for-field, including the Force-Align fill-angle
    // computation: when the LightingForceFillAlignment flag is ON the fill
    // azimuths are derived from the sun (sun.z + 120° / + 210°, both at -10°
    // tilt); when OFF the persisted free-edit angles feed the engine directly.
    // Floats are REG_BINARY (readF), colours + the flag are REG_DWORD. Same
    // !useTestHost gate as the rest of this block (the engine snapshot the
    // dialog-lighting a11y golden seeds from must show ctor defaults under
    // --test-host). Intensity is folded into the diffuse/specular channels
    // exactly as the legacy `MakeLight` (native Win32 UI, since removed) did;
    // fills pass specular=black.
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
    // Ambient pushes alpha w=1 — game-faithful. The engine folds scene ambient
    // into its SPH lighting as ambient.xyz * ambient.w
    // (src/SphericalHarmonics.cpp:76), so w gates the per-vertex mesh ambient
    // floor; per Petroglyph's shaders (reference/foc-shaders/AlamoEngine.fxh)
    // production Mesh*/RSkin* light ambient ONLY via that SPH path, so w=1
    // reproduces the game's mesh brightness. This and the React
    // `ambientToVec4` (LightingPane.tsx) push the same w=1, so load == Reset
    // (keep both in lockstep).
    auto ambientToVec4 = [](COLORREF c) -> D3DXVECTOR4 {
        return D3DXVECTOR4(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                           GetBValue(c) / 255.0f, 1.0f);
    };
    // Shadow keeps w=0: m_shadow.xyz drives the reference-model shadow darken
    // tint (Engine::RenderReferenceShadows), so it IS sampled at render time.
    // The alpha (w) is unused by the darken — only .xyz is read.
    auto colorToVec4 = [](COLORREF c) -> D3DXVECTOR4 {
        return D3DXVECTOR4(GetRValue(c) / 255.0f, GetGValue(c) / 255.0f,
                           GetBValue(c) / 255.0f, 0.0f);
    };

    // Force-align fill angles (verbatim from the legacy dialog).
    const host::FillAngles fills = host::ForceAlignFillAngles(
        s.forceAlign, s.sunZ, s.fill1Zp, s.fill1Tiltp, s.fill2Zp, s.fill2Tiltp);
    engine->SetLight(Engine::LT_SUN,
        makeLight(s.sunZ, s.sunTilt, s.sunDiffuse, s.sunSpecular, s.sunIntensity));
    engine->SetLight(Engine::LT_FILL1,
        makeLight(fills.fill1Z, fills.fill1Tilt, s.fill1Diffuse, RGB(0, 0, 0),
                  s.fill1Intensity));
    engine->SetLight(Engine::LT_FILL2,
        makeLight(fills.fill2Z, fills.fill2Tilt, s.fill2Diffuse, RGB(0, 0, 0),
                  s.fill2Intensity));
    engine->SetAmbient(ambientToVec4(s.sunAmbient));
    engine->SetShadow (colorToVec4(s.sunShadow));

    // The standing no-user verification channel for the lighting restore —
    // distinct from [view-restore] above. Prints the inputs that drove the
    // engine writes.
    g_self->Log("[lighting-restore] sunZ=%.1f sunTilt=%.1f forceAlign=%d "
                "fill1Z=%.1f fill2Z=%.1f sunDiffuse=0x%06X\n",
                s.sunZ, s.sunTilt, s.forceAlign ? 1 : 0, fills.fill1Z, fills.fill2Z,
                static_cast<unsigned>(s.sunDiffuse));

    // Dump restored view-settings to host.log. This is the ONLY no-user
    // verification channel for this restore: the --test-host CDP bridge can't
    // observe it (the whole block is gated off under --test-host), so a faithful
    // non-test-host launch + this log line is how parity is confirmed (host.log
    // is trusted under this architecture; agent screenshots are not).
    g_self->Log("[view-restore] bg=0x%06X showGround=%d groundTex=%d "
                "groundSolid=0x%06X skydome=%d\n",
                static_cast<unsigned>(engine->GetBackground()),
                engine->GetGround() ? 1 : 0,
                engine->GetGroundTexture(),
                static_cast<unsigned>(engine->GetGroundSolidColor()),
                engine->GetSkydomeSlot());
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
            if (ShouldRestorePersistedViewSettings(useTestHost, m_captureGoldenProfile))
            {
                // Open MAY fail on the very first launch (key absent). That must NOT skip
                // the restore: ReadRestoredSettings fail-softs every read to its default on
                // a null key, and the apply phase's UNCONDITIONAL lighting pushes
                // (SetLight/SetAmbient(w=1)/SetShadow) are load-bearing — gating on a
                // successful open once left a true first run unlit (ambient w=0 → black
                // viewport). Caught by scripts/cold-launch-check.ps1 on a clean profile.
                const bool inCaptureMode = !m_captureAlo.empty() || !m_captureRef.empty();
                HKEY hKey = host::OpenSettingsKeyForRead();
                const host::RestoredSettings restored =
                    host::ReadRestoredSettings(hKey, inCaptureMode);
                if (hKey) RegCloseKey(hKey);
                ApplyRestoredSettings(engine.get(), restored);
            }
            else if (m_captureGoldenProfile)
            {
                // Report the LIVE stack size rather than a constant: if the
                // restore gate above ever regresses, the count moves off zero,
                // the golden runner's exact-line match misses, and the capture
                // fails loudly instead of quietly comparing modded pixels.
                const size_t layers = modManager ? modManager->GetLayerStack().size() : 0;
                fputs("[capture-profile] golden persisted-view-restore=skipped\n", stdout);
                printf("[capture-profile] golden persisted-mod-layer-restore=skipped layers=%zu\n",
                       layers);
                fflush(stdout);
                Log("[capture-profile] golden persisted-view-restore=skipped\n");
                Log("[capture-profile] golden persisted-mod-layer-restore=skipped layers=%zu\n",
                    layers);
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
                engine->SetCompositionCompositor(m_compositor.get());
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
        // Crash recovery: a Reload() the page never answered (WebViewCrashPolicy.h).
        else if (wp == kWebReloadDeadlineTimerId)
        {
            KillTimer(hwnd, kWebReloadDeadlineTimerId);
            if (webviewcrash::DecideReloadDeadline(m_webReloadPending)
                    == webviewcrash::WebFailureAction::MarkDead)
                MarkWebDead("reload deadline passed");
        }
        // A dead-web close deferred out of a bridge handler's modal pump.
        else if (wp == kWebDeadCloseRetryTimerId)
        {
            CloseAfterWebDeath(hwnd);
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
    case WM_APP_COMPOSITION_FATAL:
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
        // A dead web can't show that prompt or send app/quit, so it never
        // vetoes (HX1); an interactive session closes through the native
        // recovery path instead of an unanswerable veto.
        if (ShouldVetoClose(dispatcher && dispatcher->GetDirty(), m_automationMode, useTestHost,
                            /*webAlive*/!m_webDead))
        {
            if (dispatcher) dispatcher->EmitCloseRequested();
            return 0;
        }
        if (m_webDead && IsFullyInteractive())
        {
            CloseAfterWebDeath(hwnd);   // once; a repeat close during its modal is swallowed
            return 0;
        }
        break;   // not dirty (or headless) → DefWindowProc → WM_DESTROY

    case WM_APP_WEB_DEAD:
        // OnWebProcessFailed declared the web dead. The window is unusable
        // (title bar, menus and viewport all live in the page), so an
        // interactive session closes now rather than waiting for an Alt-F4 the
        // user may not think of. A headless run ends from the pump instead,
        // with kWebProcessFailedExitCode.
        if (IsFullyInteractive())
            CloseAfterWebDeath(hwnd);
        return 0;

    case WM_APP_QUIT_CONFIRMED:
        // React confirmed the close (saved or discarded). DestroyWindow
        // → WM_DESTROY (NOT WM_CLOSE), so a confirmed quit never re-enters the
        // veto above.
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // FIRST, before anything is torn down. PostQuitMessage below only POSTS
        // WM_QUIT — the pump keeps dispatching whatever is already queued — so a
        // WebView2 creation completion can still arrive after this point and
        // would otherwise build a Compositor on a destroyed HWND. The token the
        // creation callbacks hold reads dead from here on.
        m_startupGuard.Retire();
        KillTimer(hwnd, kStatsTimerId);
        // Stop autosave + delete THIS session's autosave files on a
        // clean exit so no orphan prompts on the next launch. A crash skips
        // WM_DESTROY, leaving the orphan for recovery — exactly the point.
        // A dead-web close with unsaved work is the same case by another road:
        // CloseAfterWebDeath wrote the recovery copy and set
        // m_keepAutosaveSession so the next launch offers it.
        if (!useTestHost && !m_automationMode)
        {
            KillTimer(hwnd, Autosave::RECENT_TIMER_ID);
            KillTimer(hwnd, Autosave::STABLE_TIMER_ID);
            if (m_keepAutosaveSession)
                Log("[webview] keeping this session's autosave files for recovery\n");
            else
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
        // Every WebView2 / composition / engine COM object, in the documented
        // order — shared with the end of Run() so automation exits release the
        // same set (2026-09-30 audit MH1).
        ReleaseHostComObjects();
        PostQuitMessage(0);
        return 0;
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
        if (SUCCEEDED(coHr)) CoUninitialize();
        CloseLog();
        return 1;
    }
    Log("[host] WebView2 runtime detected — proceeding\n");

    // GDI+ init for AlphaCompositor::CaptureSnapshotJpegBase64 (the
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
        if (SUCCEEDED(coHr)) CoUninitialize();
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
    // Every event and async response reaches the UI through here, serialized
    // by host::SerializeBridgeEnvelope (BridgeWire.h): invalid UTF-8 (raw
    // .alo/.meg name bytes) becomes U+FFFD instead of throwing (audit H1).
    auto emitFn = [this](const nlohmann::json& env)
    {
        if (!webView) return;
        std::wstring w = Utf8ToWide(SerializeBridgeEnvelope(env));
        webView->PostWebMessageAsJson(w.c_str());
    };
    dispatcher = std::make_unique<BridgeDispatcher>(/*engine*/nullptr, layout, accelerator, emitFn,
                                                    /*useTestHost*/useTestHost,
                                                    /*automationMode*/m_automationMode);
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
        // folder, corrupt install, policy). Reuse the same offer: it opens the
        // WebView2 download page (or a side-by-side bootstrapper if present).
        wchar_t detail[128];
        swprintf(detail, 128, L"(WebView2 initialisation failed, 0x%08lx.)", hr);
        if (IsFullyInteractive())
            OfferWebView2Install(hMain, detail);
        else
            Log("[host] WebView2 init failed (0x%08lx) — bailing headlessly\n", hr);
        DestroyWindow(hMain);
        g_self = nullptr;
        if (SUCCEEDED(coHr)) CoUninitialize();
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
            m_captureSkydomeSlot, m_captureGoldenProfile,
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
    RecordSession rec;
    rec.startQpc = PerfQpcNow();
    rec.freqQpc  = PerfQpcFreq();

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
        // HX1: a headless run whose web process died can't make progress — no
        // app/ready, no frame acks, no composite — so end it now with
        // kWebProcessFailedExitCode instead of waiting out a watchdog. An
        // interactive session closes through WM_APP_WEB_DEAD instead.
        if (m_webDead && !IsFullyInteractive())
        {
            Log("[webview] headless run aborted: web process failed (exit %d)\n",
                webviewcrash::kWebProcessFailedExitCode);
            break;
        }
        // Load/capture failure → bail to cleanup
        // without rendering (exit code set below).
        if (captureRunner.Failed()) break;

        // --drive: own top-level branch, FIRST (captureMode is false in drive
        // mode, so this must precede the !captureMode idle branch). Renders
        // every iteration; never blocks. States: wait app/ready -> build runner
        // -> one-shot DComp settle -> Tick per frame.
        if (engine && m_driveMode)
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
        else if (m_driveMode && !engine)
        {
            Log("[drive] engine unavailable -- aborting drive run\n");
            driveExitCode = 5;
            quit = true;
        }
        // --record: own top-level branch (captureMode/m_driveMode are false in
        // record mode, so this precedes the !captureMode idle branch). States:
        // wait app/ready -> parse timeline + one-time startup gate (seed/resize/
        // pause/open/catalog) + build runner -> Tick per emitted frame.
        else if (engine && m_recordMode)
        {
            RenderD3D9();
            const double elapsedMs = rec.freqQpc > 0
                ? QpcMs(PerfQpcNow() - rec.startQpc, rec.freqQpc) : 0.0;

            if (rec.freqQpc <= 0)
            {
                Log("[record] no high-resolution timer available\n");
                rec.exitCode = 5; quit = true;
            }
            else if (!m_uiReady)
            {
                if (elapsedMs >= 30000.0)
                {
                    Log("[record] startup watchdog: app/ready never arrived\n");
                    rec.exitCode = 5; quit = true;
                }
            }
            else if (!m_clipRunner)
            {
                if (SetupRecordArm(rec)) quit = true;
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
                m_recordTiming.frame.push_back(QpcMs(PerfQpcNow() - tickStart, rec.freqQpc));
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
                    rec.exitCode = m_clipRunner->ExitCode();
                    // Branch B: drain + join the background encoder BEFORE the
                    // publish decision — .tmp is never renamed on a dirty
                    // drain (a queued frame's write can fail after its Tick
                    // already returned success; plan risk 4).
                    if (rec.encoder && !rec.encoder->Finish() && rec.exitCode == 0)
                    {
                        Log("[record] async encode failed at %ls\n",
                            rec.encoder->FailedPath().c_str());
                        rec.exitCode = 4;
                    }
                    // [PR 12] close the trace before the publish rename — an open
                    // file handle inside rec.tmpDir would fail the tmp -> out
                    // move on Windows (sharing violation), same reason the encoder
                    // is drained first. The per-frame flushes already persisted it.
                    m_recordTrace.reset();
                    // HX1: a web process that died during the run leaves frames
                    // nobody can vouch for (the pump then ends the run with
                    // kWebProcessFailedExitCode) — never publish them.
                    if (rec.exitCode == 0 && m_webDead)
                    {
                        Log("[record] web process failed during the run -- not publishing\n");
                        rec.exitCode = webviewcrash::kWebProcessFailedExitCode;
                    }
                    // Validate, THEN replace (HX3). Everything that can still fail
                    // the run is checked against <out>.tmp first; the previous good
                    // output in <out> is touched only once the new one is complete.
                    // On any failure both stay as they are.
                    //
                    // Verify sidecar: a target-bearing run accumulated the
                    // per-frame resolved cursor centers — write them next to the
                    // frames so a downstream script can check the cursor landed on
                    // each authored element. Literal runs skip it (empty array).
                    // Written into <out>.tmp before the length check, so a clean
                    // run that comes up short still leaves the sidecar to inspect.
                    if (rec.exitCode == 0 && m_clipRunner->IsTargetCursor())
                    {
                        const std::filesystem::path sidecar =
                            std::filesystem::path(rec.tmpDir) / L"cursor-sidecar.json";
                        bool wrote = false;
                        {
                            std::ofstream f(sidecar, std::ios::binary | std::ios::trunc);
                            if (f)
                            {
                                f << m_clipRunner->Sidecar().dump(2);
                                f.close();
                                wrote = !f.fail();
                            }
                        }
                        if (wrote)
                        {
                            Log("[record] wrote cursor sidecar (%d frames)\n",
                                (int)m_clipRunner->Sidecar().size());
                        }
                        else
                        {
                            Log("[record] cursor sidecar write failed\n");
                            rec.exitCode = 4;
                        }
                        // One sidecar row per frame on a clean run (step 4a
                        // appends or aborts). A short sidecar means a frame was
                        // captured without resolve validation — fail, don't ship.
                        if ((int)m_clipRunner->Sidecar().size() != m_clipRunner->FrameCount())
                        {
                            Log("[record] cursor sidecar incomplete: %d of %d frames\n",
                                (int)m_clipRunner->Sidecar().size(), m_clipRunner->FrameCount());
                            rec.exitCode = 4;
                        }
                    }
                    // Move the completed, validated sequence into place.
                    if (rec.exitCode == 0)
                    {
                        // `out` is validated as relative + traversal-free,
                        // which stops an ESCAPE but not the destruction of an existing
                        // directory under the launch dir — the publish below is an
                        // unconditional remove_all. Refuse unless the target is absent,
                        // empty, or holds nothing but a previous record's own output.
                        // Re-shooting into the same directory (the normal workflow)
                        // still works; deleting a stranger's files does not.
                        std::wstring refuseReason;
                        if (!MayReplaceRecordDirOnDisk(rec.outDir, refuseReason))
                        {
                            Log("[record] REFUSING to replace output dir %ls: %ls\n",
                                rec.outDir.c_str(), refuseReason.c_str());
                            Log("[record] the frames are intact in %ls — move them yourself, "
                                "or point 'out' at a new directory\n", rec.tmpDir.c_str());
                            rec.exitCode = 4;   // publish refused -> non-zero exit
                        }
                        else
                        {
                            std::error_code ec;
                            std::filesystem::remove_all(rec.outDir, ec);
                            std::error_code ec2;
                            std::filesystem::rename(rec.tmpDir, rec.outDir, ec2);
                            if (ec2)
                            {
                                Log("[record] move tmp -> out failed: %s (the validated frames "
                                    "are intact in %ls)\n", ec2.message().c_str(), rec.tmpDir.c_str());
                                rec.exitCode = 4;   // publish failure -> non-zero exit
                            }
                        }
                    }
                    // [R3] Snapshot queue stats before the summary reads them.
                    if (rec.encoder) m_recordEncoderStats = rec.encoder->GetQueueStats();
                    LogRecordTimingSummary(rec.freqQpc > 0
                        ? QpcMs(PerfQpcNow() - rec.startQpc, rec.freqQpc) : 0.0);
                    Log("[record] done: %d frames, exit %d\n",
                        m_clipRunner->FrameCount(), rec.exitCode);
                    quit = true;
                }
                else if (elapsedMs >= rec.budgetMs)
                {
                    Log("[record] watchdog: exceeded %.0f ms budget\n", rec.budgetMs);
                    // [record-timing] a timed-out run still yields its numbers.
                    if (rec.encoder) m_recordEncoderStats = rec.encoder->GetQueueStats();
                    LogRecordTimingSummary(elapsedMs);
                    // Branch B: join the encoder before quitting (exit 5 stands
                    // regardless of the drain result).
                    if (rec.encoder) rec.encoder->Finish();
                    rec.exitCode = 5; quit = true;
                }
            }
        }
        // --record with a null engine: can't run; exit non-zero.
        else if (m_recordMode && !engine)
        {
            Log("[record] engine unavailable -- aborting record run\n");
            rec.exitCode = 5;
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

    // Did a headless run lose its web process? Covers both the pump-top abort
    // and a death delivered by the message pumping inside a run's final Tick.
    const bool webDeadAbort = m_webDead && !IsFullyInteractive();

    // Automation exits (--capture/--drive/--record, and a headless run ended by
    // a dead web) break the pump via `quit` WITHOUT destroying hMain, so
    // WM_DESTROY never ran and every WebView2 / composition / engine object is
    // still held. Release them through the SAME path WM_DESTROY uses, at the
    // same point in the sequence (pump done, before the worker joins and GDI+
    // shutdown), so their final Release() precedes CoUninitialize on EVERY exit
    // path (2026-09-30 audit MH1). Idempotent: after an interactive teardown it is a no-op.
    ReleaseHostComObjects();

    g_self = nullptr;
    // Branch B: a WM_QUIT escape from the pump (user closed the record window)
    // bypasses the Done/watchdog joins above — the encoder worker MUST be
    // joined before GdiplusShutdown or it races process-level GDI+ teardown.
    if (rec.encoder) rec.encoder->Finish();
    // [C3] Join the preview-encode worker for the same reason — it encodes
    // via GDI+ and must not race process-level GDI+ teardown.
    if (dispatcher) dispatcher->ShutdownPreviewWorker();
    // Matching shutdown for the GdiplusStartup above. Safe
    // here because the message pump has drained: no dispatcher
    // handlers (CaptureSnapshotJpegBase64 et al) can run after WM_QUIT.
    if (gdiplusToken) Gdiplus::GdiplusShutdown(gdiplusToken);
    // Balance CoInitializeEx only if it succeeded (S_OK or S_FALSE); after a
    // failure (e.g. RPC_E_CHANGED_MODE) there is nothing of ours to undo.
    if (SUCCEEDED(coHr)) CoUninitialize();
    CloseLog();
    // A headless run whose web process died (HX1) — whatever stage it reached,
    // its output can't be trusted, and a --record run never published.
    if (webDeadAbort) return webviewcrash::kWebProcessFailedExitCode;
    // In --capture mode we break the loop via
    // the `quit` flag (not PostQuitMessage), so m.wParam is stale; return
    // an explicit 0/2 so a script can detect a bad load / failed write.
    if (captureMode) return captureRunner.ExitCode();
    // --drive likewise breaks via `quit`; return the runner's explicit code.
    if (m_driveMode) return driveExitCode;
    if (m_recordMode) return rec.exitCode;
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
                       const HostLaunchOptions& options)
    : m_impl(std::make_unique<HostWindowImpl>(hInstance, textureManager, shaderManager,
                                              fileManager, gameRoots, options))
{
}

HostWindow::~HostWindow() = default;

int HostWindow::Run(int nCmdShow)
{
    return m_impl->Run(nCmdShow);
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
        const HostLaunchOptions& options)
{
    const std::wstring& perfTracePath = options.perfTracePath;
    const std::wstring& perfTraceMode = options.perfTraceMode;
    const std::wstring& perfArtifactDir = options.perfArtifactDir;
    const std::wstring& perfWebViewProfile = options.perfWebViewProfile;
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
                    gameRoots, options);
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
