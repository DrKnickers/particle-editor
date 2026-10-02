// CaptureTicker.cpp — the --capture per-frame state machine (see the header).
// Moved from CaptureRunner::Tick; the live-host pieces (Sleep, QPC clock,
// StepPreviewFrames, the PNG writers, the app/ready message-pump wait) arrive
// through hooks so this file has no host, engine or window dependency.

#include "CaptureTicker.h"

#include "HostRunUtil.h"   // DeriveSibling

#include <cstdarg>
#include <cstdio>

namespace host {

void CaptureTicker::SetHooks(SleepFn sleep, NowMsFn now, ReadyFn uiReady,
                             ReadyFn layoutReady, StepClockFn stepClock, LogFn log)
{
    m_sleep       = std::move(sleep);
    m_now         = std::move(now);
    m_uiReady     = std::move(uiReady);
    m_layoutReady = std::move(layoutReady);
    m_stepClock   = std::move(stepClock);
    m_log         = std::move(log);
}

void CaptureTicker::SetCaptureHooks(WriteFn writeEngineRt, WriteFn writeComposite,
                                    WaitForUiFn waitForUi)
{
    m_writeEngineRt  = std::move(writeEngineRt);
    m_writeComposite = std::move(writeComposite);
    m_waitForUi      = std::move(waitForUi);
}

void CaptureTicker::Log(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m_log) m_log(buf);
}

CaptureTicker::Result CaptureTicker::Tick()
{
    if (m_done) return Result::Done;

    // Pace the sim with a fixed ~16 ms wall-clock step so the render's
    // real-time dt advances particles a useful amount per frame (the uncapped
    // pump would otherwise run dozens of frames in a few ms, leaving particles
    // bunched at the spawn point and never overlapping — which is exactly the
    // additive-over-smoke case we need to see).
    if (m_sleep) m_sleep(kPaceMs);

    // Layout-determinism gate: hold the frame counter until React's first
    // paint AND first layout/scene-rect have landed — the scene-rect resizes
    // the engine RT, so counting from process start raced it and captures came
    // out at the pre- OR post-layout size depending on system load. 10 s cap so
    // a changed UI degrades to the old (ungated) behavior, loudly. With no
    // clock at all the gate is treated as already timed out.
    const bool uiReady     = m_uiReady && m_uiReady();
    const bool layoutReady = m_layoutReady && m_layoutReady();
    if (!(uiReady && layoutReady))
    {
        const double now = m_now ? m_now() : -1.0;
        double heldMs = kLayoutGateTimeoutMs;
        if (now >= 0.0)
        {
            if (!m_gateStarted) { m_gateStarted = true; m_gateStartMs = now; }
            heldMs = now - m_gateStartMs;
        }
        if (heldMs < kLayoutGateTimeoutMs)
            return Result::Running;
        if (!m_gateWarned)
        {
            m_gateWarned = true;
            Log("[capture] layout gate timed out (uiReady=%d sceneRect=%d) — proceeding ungated\n",
                (int)uiReady, (int)layoutReady);
            // Also on stdout: an ungated capture is racy-sized, so golden
            // consumers (scripts/render-goldens.mjs) must be able to SEE the
            // degradation and fail the scene rather than flake against a
            // fixed-size golden.
            printf("[capture] layout-gate-timeout — capture size may be pre-layout\n");
            fflush(stdout);
        }
    }

    // Advance the frozen sim clock by exactly one 60 Hz frame per COUNTED
    // frame (a no-op unless the capture spawn path paused the preview clock).
    // Placed after the layout gate so gate-held pump frames render the frozen
    // scene without advancing sim time — the captured frame is then always at
    // sim time capturedFrames/60, independent of UI cold-start duration.
    // Consumed by the NEXT render at the top of the pump.
    if (m_stepClock) m_stepClock();
    if (++m_capturedFrames < m_frames)
        return Result::Running;

    // (1) engine RT: the engine's own pre-composite pixels, captured at the
    // exact frame target. Only the composite below is gated on the UI.
    const bool ok = m_writeEngineRt && m_writeEngineRt(m_enginePng);
    if (!ok) m_failed = true;

    // (2) composite: the final composited window (engine viewport framed by
    // React chrome), after waiting for app/ready so it shows real chrome, not
    // a blank WebView surface.
    const UiWait wait = m_waitForUi ? m_waitForUi() : UiWait();

    // Success name only when React actually signalled first paint; a timeout
    // OR an external WM_QUIT before the signal yields a degraded image under a
    // DISTINCT name so it can never be mistaken for a good one (the harness
    // greps for the non-TIMEOUT name + requires ui-ready=1).
    const wchar_t* suffix = wait.uiReady ? L"-composite" : L"-composite-TIMEOUT";
    const char*    state  = wait.uiReady ? "" : (wait.timedOut ? " TIMEOUT" : " ABORTED");
    const std::wstring compPath = DeriveSibling(m_enginePng, suffix);
    // Composite is UNCONDITIONAL (attempted even if engine-RT failed) — the
    // diagnostic composite is most valuable exactly when a render broke.
    const bool okc = m_writeComposite && m_writeComposite(compPath);
    Log("[capture] frame %d: engine-RT %ls -> %s; composite %ls -> %s "
        "(ui-ready=%d waited=%.0fms%s)\n",
        m_capturedFrames, m_enginePng.c_str(), ok ? "ok" : "FAILED",
        compPath.c_str(), okc ? "ok" : "FAILED",
        wait.uiReady ? 1 : 0, wait.waitedMs, state);
    if (!wait.uiReady)
        Log("[capture] WARNING: app/ready not received (%s) — composite "
            "may show an unpainted React surface\n",
            wait.timedOut ? "30s timeout" : "window closed mid-wait");
    // The exit code stays engine-RT-driven; a UI timeout is a host.log
    // WARNING, not a process failure.
    m_done = true;
    return Result::Done;
}

}  // namespace host
