// HostWindow_Record.cpp — HostWindowImpl::SetupRecordArm, the --record arm's
// one-time setup (timeline parse, mod check, startup gate, hook wiring),
// called from HostWindowImpl::Run in HostWindow.cpp. The per-run state it
// shares with Run travels in RecordSession (HostWindowImpl.h).
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "HostWindowImpl.h"

namespace host {

// ---------- Run: --record setup (extracted from Run) ----------

bool HostWindowImpl::SetupRecordArm(RecordSession& rec)
{
    bool quit = false;
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
                    rec.exitCode = 2;   // Init-fail and mod-miss are both exit 2
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
                    //     in GrabWindowPixels), so the window size IS the emitted
                    //     frame size — set it directly so frames are exactly
                    //     tl.width x tl.height (the engine viewport is the client
                    //     sub-rect, a bit smaller after chrome).
                    SetWindowPos(hMain, nullptr, 0, 0, tl.width, tl.height,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

                    // (b2) render the WebView chrome at tl.scale device-pixel ratio so
                    //      a zoomed crop (e.g. the mod-picker clip) stays sharp: the same
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
                            rec.exitCode = 3; quit = true; gateOk = false;
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
                        while (QpcMs(PerfQpcNow() - s, rec.freqQpc) < tl.openSettleMs)
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
                        webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(hm)).c_str());
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
                        SetWindowPos(hMain, nullptr, -32000, -32000, 0, 0,
                                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                        Log("[record] window moved offscreen for headless capture\n");
                    }

                    // (e1) hide the right-dock (Spawner/Lighting/Atlas) panel so the
                    //      recorded clip shows a clean layout + more curve editor.
                    {
                        nlohmann::json hp = {{"type","ui/hide-panel"}};
                        if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(hp)).c_str());
                    }

                    // (e2) focus the curve panel on each track-key tween's channel so
                    //      a scripted scrub shows the channel it edits (the panel focus
                    //      is React-local + defaults to red). Posted once after settle;
                    //      focusChannel persists independent of the emitter selection.
                    for (const auto& tk : tl.trackKeys)
                    {
                        nlohmann::json fm = {{"type","ui/focus-channel"},{"channel", tk.track}};
                        if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(fm)).c_str());
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
                        while (QpcMs(PerfQpcNow() - s, rec.freqQpc) < 500 ||
                               (m_recordHeadless && !m_sceneRectSeen &&
                                QpcMs(PerfQpcNow() - s, rec.freqQpc) < 3000))
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
                            webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(trackMsg)).c_str());
                    }

                    // (f) output dirs: render to <out>.tmp, move on success.
                    rec.outDir = host::Utf8ToWide(tl.out);
                    rec.tmpDir = rec.outDir + L".tmp";
                    // Clearing <out>.tmp is the same remove_all as the
                    // publish, so it gets the same check — a directory (or a
                    // file) that happens to sit at that name and isn't a
                    // previous run's staging output is refused, not deleted.
                    {
                        std::wstring refuseReason;
                        if (!MayReplaceRecordDirOnDisk(rec.tmpDir, refuseReason))
                        {
                            Log("[record] REFUSING to clear staging dir %ls: %ls\n",
                                rec.tmpDir.c_str(), refuseReason.c_str());
                            Log("[record] move it aside, or point 'out' at a new directory\n");
                            rec.exitCode = 4;   // output-dir safety refusal, same code as publish
                            return true;
                        }
                    }
                    std::error_code ec;
                    std::filesystem::remove_all(rec.tmpDir, ec);
                    std::filesystem::create_directories(rec.tmpDir, ec);

                    // (g) hooks.
                    const std::wstring tmpDir = rec.tmpDir;
                    // Flag-gated pump-schedule trace (RecordTrace.h). Written INTO the
                    // output dir (allowlisted in RecordOutputSafety so the next
                    // run doesn't treat it as a foreign file); off by default so
                    // 60fps token logging never perturbs a normal record. The
                    // runner and the capture lambda both emit into this one sink.
                    {
                        wchar_t tb[8] = {};
                        const bool traceOn =
                            GetEnvironmentVariableW(L"PE_RECORD_TRACE", tb, 8) > 0
                            && tb[0] != L'0';
                        if (traceOn)
                        {
                            auto tr = std::make_unique<host::RecordTrace>(
                                tmpDir + L"\\pump-trace.txt");
                            if (tr->Ok())
                            {
                                m_recordTrace = std::move(tr);
                                r->SetTrace(m_recordTrace.get());
                            }
                            else
                            {
                                Log("[record] WARNING PE_RECORD_TRACE set but trace "
                                    "file could not be opened — continuing untraced\n");
                            }
                        }
                    }
                    // Start the background encoder BEFORE the hooks
                    // capture it. Ctor pre-warms the PNG CLSID on this (UI)
                    // thread (GdiplusEncode.h's cache is not first-call
                    // thread-safe) and spawns the single worker.
                    rec.encoder = std::make_shared<host::AsyncFrameEncoder>(
                        128ull * 1024 * 1024,
                        [this](const std::string& s){ Log("[record] %s\n", s.c_str()); });
                    r->SetHooks(
                        // dispatch (allowlisted bridge req -> response)
                        [this, rf = rec.freqQpc](const std::string& req){
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
                            if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(m)).c_str());
                        },
                        // ack: pumped wait for ui/frame-acked >= frameId (bounded).
                        // [record-timing] the whole hook (incl. its inner
                        // RenderD3D9 pumps) accrues to the ACK segment.
                        [this, rf = rec.freqQpc](int frameId, double deadlineMs){
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
                                // A dead web will never ack; stop now and let the
                                // pump end the run instead of waiting out
                                // the deadline frame after frame.
                                if (m_webDead) break;
                                if (rf > 0 && QpcMs(PerfQpcNow() - s, rf) >= dl) break;
                                if (!m_recordHeadless) RenderD3D9();
                                // Message-aware wait: the ack ARRIVES as a
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
                        [this, tmpDir, bp = kBarrierPresents,
                         adv = kBarrierCompositorAdvance, rf = rec.freqQpc,
                         enc = rec.encoder](int idx){
                            wchar_t name[40];
                            swprintf_s(name, L"\\frame_%05d.png", idx);
                            host::AsyncFrameEncoder::Frame f;
                            f.path = tmpDir + name;

                            // Headless (PE_RECORD_HEADLESS): the record window is
                            // moved OFFSCREEN (not minimized — see the SetWindowPos
                            // above), so it composites normally AND can't be occluded
                            // by any other window. That makes the fast foreground
                            // capture path below (barrier + GrabWindowPixels) both
                            // correct and occlusion-immune for headless too. (This
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
                            // png are timed separately. png is now
                            // GRAB + ENQUEUE only — the compress+write runs on
                            // the encoder worker (AsyncFrameEncoder.h), so the
                            // png segment no longer contains the zlib cost.
                            //
                            // Adaptive barrier: the fixed 3x flush existed
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
                            // Pump-trace barrier phase token — the CONFIGURED
                            // policy (cap + compositor-advance target), emitted at
                            // the loop's implementation site. Runtime flush count
                            // and probe outcome stay OUT (timing-dependent → they
                            // live in m_recordTiming); the compare needs the
                            // deterministic policy a weakening extraction changes.
                            if (m_recordTrace)
                                m_recordTrace->Emit("BARRIER:cap=" + std::to_string(bp)
                                                    + ",adv=" + std::to_string(adv));
                            for (int i = 0; i < bp; ++i)
                            {
                                RenderD3D9(); DwmFlush();
                                ++m_recordTiming.barrierFlushTotal;
                                if (!probe0) continue;   // fixed-bp fallback
                                DWM_TIMING_INFO ti = {};
                                ti.cbSize = sizeof(ti);
                                if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)))
                                {
                                    if (ti.cFrame >= ti0.cFrame + adv) break;
                                }
                                else
                                {
                                    m_recordTiming.barrierProbeFailed = true;
                                }
                            }
                            const LONGLONG b1 = PerfQpcNow();
                            // Pump-trace grab token, adjacent to the real
                            // GrabWindowPixels — so a refactor that moves the grab
                            // above the barrier within this lambda reorders the
                            // trace line (a token at the pump's callback entry
                            // would not move with it).
                            const bool ok = host::GrabWindowPixels(hMain, f.bgra, f.w, f.h)
                                            && enc->Enqueue(std::move(f));
                            if (m_recordTrace)
                                m_recordTrace->Emit(ok ? "GRAB:ok" : "GRAB:fail");
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
                            if (webView) webView->PostWebMessageAsJson(host::Utf8ToWide(SerializeBridgeEnvelope(m)).c_str());
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

                    rec.budgetMs = r->FrameCount() *
                                     (host::ClipRunner::kAckDeadlineMs + 1000.0) + 30000.0;
                    m_clipRunner = std::move(r);
                    }  // end if (gateOk)
                }
    return quit;
}

} // namespace host
