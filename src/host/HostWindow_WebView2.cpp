// HostWindow_WebView2.cpp — HostWindowImpl's WebView2 side: environment and
// composition-controller creation, the shared per-controller setup
// (navigation and permission policy, the embedded app.local bundle),
// web-message ingress, resize, and mouse forwarding into the composition
// surface. Shared state lives in HostWindowImpl.h.
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "HostWindowImpl.h"

namespace host {

namespace {

// The WebView2 origin allow-list (IsApprovedWebViewOrigin) lives in
// WebViewOriginPolicy.h so its boundary is unit-tested.

// RAII owner for a CoTaskMemAlloc'd string handed out by a WebView2 getter
// (get_Source / TryGetWebMessageAsString / get_WebMessageAsJson), so an early
// return or an exception can never leak it (2026-09-30 audit H1).
struct CoTaskMemString
{
    LPWSTR p = nullptr;
    CoTaskMemString() = default;
    CoTaskMemString(const CoTaskMemString&) = delete;
    CoTaskMemString& operator=(const CoTaskMemString&) = delete;
    ~CoTaskMemString() { if (p) CoTaskMemFree(p); }
};

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
    // "-iso-", not "-capture-": --test-host now takes this path too,
    // and a folder named for capture would misreport which run owns it.
    wchar_t pidSuffix[32] = {};
    if (isolated) swprintf(pidSuffix, 32, L"-iso-%lu", GetCurrentProcessId());

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

std::wstring AppendQueryParam(const std::wstring& url, const wchar_t* param)
{
    if (!param || !param[0]) return url;
    return url + (url.find(L'?') == std::wstring::npos ? L"?" : L"&") + param;
}

} // namespace

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
    // Crash recovery: a message from the page proves the renderer answers, so
    // a hang streak is over; and while a handler runs (it may pump a modal),
    // a dead-web close must wait for the outer loop (CloseAfterWebDeath).
    m_webHangReports = 0;
    struct DepthGuard
    {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depthGuard(m_webMessageDepth);

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
        // A crash-recovery Reload() is complete once the page itself is back.
        if (m_webReloadPending)
        {
            m_webReloadPending = false;
            if (hMain) KillTimer(hMain, kWebReloadDeadlineTimerId);
            Log("[webview] reloaded UI is back (app/ready)\n");
        }
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
        // Same replay for autosave health (2026-07 audit), and for the
        // same reason: it is emitted only on a CHANGE, so a web reload would
        // otherwise drop a live "recovery net is stale" warning and not restore
        // it until the health flipped again — which, once broken, it may never
        // do. Only replay a KNOWN-BAD state: -1 means no autosave has run yet,
        // and asserting health we haven't observed would be a false all-clear.
        if (m_lastAutosaveHealthySent == 0)
        {
            m_lastAutosaveHealthySent = -1;
            EmitAutosaveHealthIfChanged(false);
        }
        return;
    }
    // --drive bridge-selftest result: the page posts {"kind":"drive/selftest-result",
    // "token":…, "ok":…} over the REAL postMessage wire — its arrival here IS the
    // thing under test. Host-lifecycle signal like app/ready (the dispatcher drops
    // non-"req" messages) — intercept + return. Token must match the armed step.
    if (msgKind == L"drive/selftest-result")
    {
        // Field-type guarded: json::value() throws on a non-object message or
        // a field of the wrong type, and this runs inside the WebView2 callback.
        nlohmann::json msg = nlohmann::json::parse(WideToUtf8(json), nullptr, false);
        if (msg.is_object() && !m_selftestToken.empty())
        {
            const auto tok = msg.find("token");
            if (tok != msg.end() && tok->is_string()
                && tok->get_ref<const std::string&>() == m_selftestToken)
            {
                const auto ok = msg.find("ok");
                m_selftestOk = ok != msg.end() && ok->is_boolean() && ok->get<bool>();
                m_selftestDone = true;
                if (!m_selftestOk)
                {
                    const auto why = msg.find("why");
                    Log("drive: selftest page-side failure: %s\n",
                        why != msg.end() && why->is_string()
                            ? why->get_ref<const std::string&>().c_str() : "?");
                }
            }
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
                const auto frame = msg.find("frame");
                m_lastAckCursorFrame = frame != msg.end() && frame->is_number_integer()
                                           ? frame->get<int>() : -1;
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
    // Any session with no human at the keyboard gets a throwaway per-PID
    // profile. This used to list capture and automation but NOT --test-host,
    // which therefore shared the daily driver's stable profile (2026-07
    // audit) — and the comment on ComputeUserDataFolder already explains why
    // that hurts: the runtime LOCKS the user-data folder, so a --test-host run
    // launched beside the live editor fails env-creation outright. That makes it
    // a flake source for the playwright-native GATE lane, not just a nuisance.
    //
    // Expressed via IsFullyInteractiveSession rather than a fourth hand-rolled
    // disjunction: "isolate the profile" and "suppress blocking modals" are the
    // same question — is anyone there? — and they must not drift apart.
    const bool captureIsolation =
        !IsFullyInteractiveSession(!m_captureAlo.empty() || !m_captureRef.empty(),
                                   m_automationMode,
                                   useTestHost);
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

    // Liveness token for the one-shot completion callbacks below. Captured BY
    // VALUE so it shares the flag, not `this`.
    auto startupToken = m_startupGuard.Issue();

    // The production factory checks startupToken before this continuation can
    // touch `this`; the standalone callback test links that exact TU.
    auto environmentHandler =
        MakeEnvironmentStartupCallback(
            startupToken,
            [this, startupToken](HRESULT envHr, ICoreWebView2Environment* env) -> HRESULT
            {
                // The adapter has already proved the owner live before this
                // continuation can log or dereference `this`.
                if (FAILED(envHr) || !env)
                {
                    Log("[host] WebView2 env failed 0x%08lx\n", envHr);
                    // Returning E_FAIL here reported the failure to nobody: the
                    // runtime discards this HRESULT, and Run() already took the
                    // SYNCHRONOUS success from
                    // CreateCoreWebView2EnvironmentWithOptions. The result was a
                    // live window with no UI, no bridge, and exit code 0
                    // (2026-07 audit). Terminal failure instead, the
                    // same treatment the Compositor::Init failure below gets.
                    FailFatalComposition(FAILED(envHr) ? envHr : E_FAIL);   // [[noreturn]]
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
                if (engine)
                    engine->SetCompositionCompositor(m_compositor.get());

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

                auto compositionControllerHandler =
                    MakeCompositionControllerStartupCallback(
                        startupToken,
                        [this](HRESULT cHr,
                               ICoreWebView2CompositionController* ctl) -> HRESULT
                        {
                            // The controller door uses the same production
                            // adapter and can dispatch later still.
                            return OnCompositionControllerReady(cHr, ctl);
                        });

                Log("[host] composition: CreateCoreWebView2CompositionController dispatching\n");
                const HRESULT controllerCreateHr =
                    env3->CreateCoreWebView2CompositionController(
                        hMain, compositionControllerHandler.Get());
                if (ShouldFailCompositionControllerDispatch(controllerCreateHr))
                {
                    // The environment completion handler's return value is
                    // discarded, so merely returning this HRESULT recreates
                    // one call deeper: a live window, no UI, exit 0.
                    // Promote the synchronous dispatch failure to the same
                    // terminal message used by async controller failures.
                    Log("[host] composition: controller create dispatch FAILED hr=0x%08lx\n",
                        controllerCreateHr);
                    PostMessageW(hMain, WM_APP_COMPOSITION_FATAL,
                                 static_cast<WPARAM>(controllerCreateHr), 0);
                }
                return controllerCreateHr;
            });

    HRESULT envCreateHr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder.c_str(), envOptions.Get(),
        environmentHandler.Get());
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
                // (2026-07 audit).
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

    // Production mode: the React app loads from the stable virtual origin
    // https://app.local/, whose requests the WebResourceRequested handler
    // (registered below, in FinishWebView2ControllerSetup) answers from the
    // exe's embedded RCDATA web bundle. No SetVirtualHostNameToFolderMapping,
    // no on-disk web/ folder, and no ?v= cache-bust — the embedded responses
    // carry a real Cache-Control header. Dev mode (--dev-ui) navigates to
    // Vite's localhost:5174 instead and never touches app.local.
    std::wstring prodNavUrl = L"https://app.local/index.html";

    // Subscribe to JS → host messages.
    // Stash the registration token in the member
    // webMessageTok so WM_DESTROY can explicitly unsubscribe.
    if (host::perf::Enabled())
        prodNavUrl = AppendQueryParam(prodNavUrl, L"perfTrace=1");

    webView->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*,
                   ICoreWebView2WebMessageReceivedEventArgs* args) noexcept -> HRESULT
            {
                // No exception may unwind into WebView2's COM dispatcher
                // (2026-09-30 audit H1): the dispatcher's own guard turns a
                // handler throw into an error envelope, and this catch-all is
                // the backstop for everything around it (parse, log, perf).
                // The CoTaskMem strings are RAII-owned, so nothing leaks either.
                try
                {
                    // Reject messages whose originating document
                    // isn't an approved origin. Belt-and-suspenders with the
                    // NavigationStarting cancel — if a frame ever loaded an
                    // off-origin document, its postMessage must not reach the
                    // native bridge.
                    // FAIL CLOSED. This check used to sit entirely inside the
                    // success branch, so a failing/empty get_Source skipped it and
                    // fell straight through to OnWebMessage — the comment above
                    // promised the message "must not reach the native bridge", but
                    // the control flow granted exactly that on a COM error
                    // (2026-07 audit). No confirmed origin, no dispatch.
                    CoTaskMemString src;
                    const HRESULT srcHr = args->get_Source(&src.p);
                    if (FAILED(srcHr) || !src.p)
                    {
                        Log("[host] G11: dropped WebMessage — source unavailable "
                            "(hr=0x%08lx)\n", (unsigned long)srcHr);
                        return S_OK;
                    }
                    if (!IsApprovedWebViewOrigin(src.p, useDevUi))
                    {
                        Log("[host] G11: dropped WebMessage from untrusted "
                            "source %ls\n", src.p);
                        return S_OK;
                    }
                    // Size cap on both ingress paths (2026-07 audit).
                    // OnWebMessage parses the whole string, so without this one
                    // postMessage drives an unbounded UI-thread allocation. Checked
                    // AFTER the origin gate so an untrusted sender never gets even
                    // this far, and applied to the JSON fallback below too -- a cap
                    // on one of two doors is not a cap.
                    const auto oversized = [this](const wchar_t* s, const char* which)
                    {
                        const size_t n = wcslen(s);
                        if (ShouldAcceptWebMessage(n, kMaxWebMessageChars)) return false;
                        Log("[host] dropped %s WebMessage — %zu chars exceeds the "
                            "%zu-char cap\n", which, n, kMaxWebMessageChars);
                        return true;
                    };

                    CoTaskMemString raw;
                    HRESULT hr1 = args->TryGetWebMessageAsString(&raw.p);
                    if (SUCCEEDED(hr1) && raw.p)
                    {
                        if (!oversized(raw.p, "string")) OnWebMessage(raw.p);
                    }
                    else
                    {
                        // Fall back: maybe the page posted a JSON value
                        // (chrome.webview.postMessage(obj) rather than
                        // postMessage(JSON.stringify(obj))). Surface a
                        // dedicated log so we can tell the difference
                        // between "no event" and "event but parse failed".
                        CoTaskMemString json;
                        HRESULT hr2 = args->get_WebMessageAsJson(&json.p);
                        if (SUCCEEDED(hr2) && json.p)
                        {
                            Log("[host] WMR JSON-only (%zu chars), hr1=0x%08lx\n",
                                wcslen(json.p), hr1);
                            if (!oversized(json.p, "json")) OnWebMessage(json.p);
                        }
                        else
                        {
                            Log("[host] WMR empty: hr1=0x%08lx hr2=0x%08lx\n",
                                hr1, hr2);
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    Log("[bridge] WebMessageReceived: swallowed exception: %s\n", e.what());
                }
                catch (...)
                {
                    Log("[bridge] WebMessageReceived: swallowed unknown exception\n");
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
                // FAIL CLOSED, same as the WebMessage source check above: a
                // failing/empty get_Uri used to skip the whole block WITHOUT
                // put_Cancel, so a navigation we could not identify was allowed
                // to proceed (2026-07 audit). An unidentifiable target
                // is exactly the one to refuse — the app's own load reports its
                // URI fine, so cancelling here costs nothing legitimate.
                LPWSTR uri = nullptr;
                const HRESULT uriHr = args->get_Uri(&uri);
                if (FAILED(uriHr) || !uri)
                {
                    Log("[host] G11: cancelled navigation — URI unavailable "
                        "(hr=0x%08lx)\n", (unsigned long)uriHr);
                    args->put_Cancel(TRUE);
                    if (uri) CoTaskMemFree(uri);
                    return S_OK;
                }
                if (!IsApprovedWebViewOrigin(uri, useDevUi))
                {
                    Log("[host] G11: cancelled navigation to %ls\n", uri);
                    args->put_Cancel(TRUE);
                }
                CoTaskMemFree(uri);
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

    // Serve the embedded React bundle. app.local is no longer a
    // SetVirtualHostNameToFolderMapping folder host — the bundle is compiled
    // into this exe as RCDATA (src\generated\EmbeddedWebAssets.rc). Intercept
    // every app.local request and answer it from the matching embedded resource
    // with the Content-Type / Cache-Control the generator baked into the
    // manifest. A folder mapping short-circuits WebResourceRequested — a
    // hard-learned WebView2 behavior — so with no mapping the handler now
    // fires. Dev mode never requests app.local, so the filter is gated on
    // !useDevUi.
    if (!useDevUi)
    {
        const HRESULT filtHr = webView->AddWebResourceRequestedFilter(
            L"https://app.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(filtHr))
        {
            Log("[host] AddWebResourceRequestedFilter(app.local) failed hr=0x%08lx\n",
                filtHr);
            return filtHr;
        }
        const HRESULT wrrHr = webView->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT
                {
                    // Synthesize a no-body response (used for 404/500). Defined
                    // FIRST so every early-out can deliver a real HTTP status:
                    // leaving args->Response null lets the runtime resolve the
                    // request itself, which for app.local is exactly the failed
                    // network load this handler exists to replace.
                    auto makeStatusResponse =
                        [this, args](int code, PCWSTR reason) -> HRESULT
                    {
                        if (!webEnv) return S_OK;
                        ComPtr<ICoreWebView2WebResourceResponse> resp;
                        const HRESULT hr = webEnv->CreateWebResourceResponse(
                            nullptr, code, reason, L"", &resp);
                        if (SUCCEEDED(hr) && resp)
                        {
                            const HRESULT pr = args->put_Response(resp.Get());
                            if (FAILED(pr))
                                Log("[host] app.local: put_Response(%d) failed "
                                    "hr=0x%08lx\n", code, pr);
                        }
                        else
                            // Nothing better to do — the request falls through to the
                            // runtime's default handling — but log it so a mystery
                            // network error is attributable.
                            Log("[host] app.local: could not synthesize %d response "
                                "hr=0x%08lx\n", code, hr);
                        return S_OK;
                    };

                    // Read the request URI. A request we cannot even identify is
                    // not the entry document, so fail it with a 500 rather than
                    // silently serving index.html for an unreadable request.
                    ComPtr<ICoreWebView2WebResourceRequest> req;
                    if (FAILED(args->get_Request(&req)) || !req)
                        return makeStatusResponse(500, L"Internal Server Error");
                    LPWSTR rawUri = nullptr;
                    const HRESULT uriHr = req->get_Uri(&rawUri);
                    if (FAILED(uriHr) || !rawUri)
                    {
                        if (rawUri) CoTaskMemFree(rawUri);
                        Log("[host] app.local: get_Uri failed hr=0x%08lx\n", uriHr);
                        return makeStatusResponse(500, L"Internal Server Error");
                    }
                    std::wstring uri = rawUri;
                    CoTaskMemFree(rawUri);

                    // Reduce the URI to an embedded-asset key: drop the
                    // scheme+host prefix, drop any ?query / #fragment, and map
                    // "" or "/" to the entry document.
                    std::wstring path = uri;
                    const wchar_t* kOrigin = L"https://app.local";
                    if (_wcsnicmp(path.c_str(), kOrigin, wcslen(kOrigin)) == 0)
                        path.erase(0, wcslen(kOrigin));
                    const size_t q = path.find_first_of(L"?#");
                    if (q != std::wstring::npos) path.erase(q);
                    if (path.empty() || path == L"/") path = L"/index.html";

                    const host::EmbeddedAsset* assets = host::EmbeddedWebAssets();
                    const size_t count = host::EmbeddedWebAssetCount();
                    const host::EmbeddedAsset* hit = nullptr;
                    for (size_t i = 0; i < count; ++i)
                        if (path == assets[i].urlPath) { hit = &assets[i]; break; }
                    if (!hit)
                    {
                        Log("[host] app.local embedded 404: %ls\n", path.c_str());
                        return makeStatusResponse(404, L"Not Found");
                    }

                    HMODULE hMod = GetModuleHandle(nullptr);
                    HRSRC hRes = FindResource(
                        hMod, MAKEINTRESOURCE(hit->resourceId), RT_RCDATA);
                    HGLOBAL hData = hRes ? LoadResource(hMod, hRes) : nullptr;
                    const DWORD size = hRes ? SizeofResource(hMod, hRes) : 0;
                    void* data = hData ? LockResource(hData) : nullptr;
                    if (!data || !size)
                    {
                        // The manifest named a resource the exe doesn't carry —
                        // a generator/rc mismatch, not a normal miss.
                        Log("[host] app.local embedded resource %d unreadable (%ls)\n",
                            hit->resourceId, path.c_str());
                        return makeStatusResponse(500, L"Internal Server Error");
                    }

                    // SHCreateMemStream COPIES the bytes into a ref-counted
                    // stream; the RCDATA block stays valid for the process
                    // lifetime, so no lifetime coupling to worry about.
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(
                        static_cast<const BYTE*>(data), size));
                    if (!stream) return makeStatusResponse(500, L"Internal Server Error");

                    if (!webEnv) return S_OK;
                    ComPtr<ICoreWebView2WebResourceResponse> resp;
                    const HRESULT rHr = webEnv->CreateWebResourceResponse(
                        stream.Get(), 200, L"OK", hit->responseHeaders, &resp);
                    if (FAILED(rHr) || !resp)
                    {
                        Log("[host] CreateWebResourceResponse failed hr=0x%08lx (%ls)\n",
                            rHr, path.c_str());
                        return makeStatusResponse(500, L"Internal Server Error");
                    }
                    const HRESULT pr = args->put_Response(resp.Get());
                    if (FAILED(pr))
                        Log("[host] app.local: put_Response failed hr=0x%08lx (%ls)\n",
                            pr, path.c_str());
                    return S_OK;
                }).Get(), &webResourceTok);
        // This handler is load-bearing: without it app.local has no responder and
        // the first navigation reproduces the blank-window / ERR_NAME_NOT_RESOLVED
        // failure this change eliminates. A failed registration must therefore be
        // terminal, not swallowed — same fail-closed treatment the removed folder
        // mapping got (2026-07 audit).
        if (FAILED(wrrHr))
        {
            Log("[host] add_WebResourceRequested(app.local) failed hr=0x%08lx\n", wrrHr);
            return wrrHr;
        }
    }

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
                // A crash-recovery Reload() ends at the page's app/ready (or
                // its deadline), not here: a navigation can complete while
                // React never boots.
                if (m_webReloadPending)
                    Log("[webview] reload navigation completed (success=%d)\n", ok ? 1 : 0);
                return S_OK;
            }).Get(), &navCompletedTok);

    // Renderer / browser process failure (2026-10-01 audit HX1). Without this a
    // crashed renderer left a dead page — and with a dirty document a window
    // whose close was vetoed forever, waiting on a prompt the dead page could
    // never show. Registered before Navigate so a crash during the first load
    // is seen too; token removed in ReleaseHostComObjects.
    {
        const HRESULT pfHr = webView->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this](ICoreWebView2*,
                       ICoreWebView2ProcessFailedEventArgs* args) noexcept -> HRESULT
                {
                    OnWebProcessFailed(args);
                    return S_OK;
                }).Get(), &processFailedTok);
        if (FAILED(pfHr))
            Log("[webview] add_ProcessFailed failed hr=0x%08lx (crash recovery off)\n",
                static_cast<unsigned long>(pfHr));
    }

    // Navigate to the React app. Navigate returns SYNCHRONOUSLY on whether the
    // request was accepted at all, and that HRESULT was dropped — a rejected
    // URL left the window blank with nothing in the log and a zero exit code
    // (2026-07 audit).
    if (useDevUi)
    {
        Log("[host] dev-ui: Navigate to Vite dev server\n");
        m_appNavUrl = host::perf::Enabled()
            ? L"http://localhost:5174/?perfTrace=1"
            : L"http://localhost:5174/";
    }
    else
    {
        m_appNavUrl = prodNavUrl;
    }
    const HRESULT navHr = webView->Navigate(m_appNavUrl.c_str());
    if (FAILED(navHr))
    {
        Log("[host] Navigate REJECTED hr=0x%08lx\n", navHr);
        return navHr;
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
        PostMessageW(hMain, WM_APP_COMPOSITION_FATAL, static_cast<WPARAM>(failHr), 0);
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
        PostMessageW(hMain, WM_APP_COMPOSITION_FATAL, static_cast<WPARAM>(qihr), 0);
        return qihr;
    }

    HRESULT setupHr = FinishWebView2ControllerSetup(baseController.Get());
    if (FAILED(setupHr))
    {
        Log("[host] composition: shared controller setup failed hr=0x%08lx\n", setupHr);
        // Composition is required (no HWND fallback): signal a fatal error.
        PostMessageW(hMain, WM_APP_COMPOSITION_FATAL, static_cast<WPARAM>(setupHr), 0);
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
            PostMessageW(hMain, WM_APP_COMPOSITION_FATAL, static_cast<WPARAM>(bhr), 0);
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
        // no-chain-into-HWND-mode). The per-frame render loop composites a
        // successful attachment; an unsuccessful one leaves the viewport empty.
        if (engine && engine->GetSharedTextureHandle())
        {
            HANDLE sharedTex = engine->GetSharedTextureHandle();
            LUID   engineLuid = engine->GetAdapterLuid();
            HRESULT ehr = m_compositor->AttachEngineVisual(sharedTex, clientW, clientH, engineLuid);
            if (FAILED(ehr))
            {
                Log("[host] composition: AttachEngineVisual hr=0x%08lx — composition mode continues with engine visual NOT attached (viewport area will be empty)\n", ehr);
                // Do NOT PostMessage(WM_APP_COMPOSITION_FATAL) — that
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

} // namespace host
