#ifndef HOST_CAPTURE_TICKER_H
#define HOST_CAPTURE_TICKER_H
//
// The --capture per-frame state machine: pacing, the layout-determinism gate,
// the counted-frame clock step, the final engine-RT + composite writes, and the
// exit code. Split out of CaptureRunner so it can be driven with stub hooks and
// a fake clock (tests/test_capture_runner.cpp), in the DriveRunner SetHooks
// style. CaptureRunner owns one and wires the hooks to the live host.

#include <functional>
#include <string>
#include <utility>

namespace host {

class CaptureTicker {
public:
    // Outcome of waiting for React's app/ready first paint (plus the settle
    // that follows it) before the composite is captured.
    struct UiWait {
        bool   uiReady  = false;  // app/ready arrived
        bool   timedOut = false;  // gave up waiting (vs. the window closing)
        double waitedMs = 0.0;
    };

    using SleepFn      = std::function<void(int ms)>;
    using NowMsFn      = std::function<double()>;  // monotonic ms; < 0 = no clock
    using ReadyFn      = std::function<bool()>;
    using StepClockFn  = std::function<void()>;    // advance the frozen sim clock one 60 Hz frame
    using WriteFn      = std::function<bool(const std::wstring& path)>;
    using WaitForUiFn  = std::function<UiWait()>;
    using LogFn        = std::function<void(const std::string&)>;

    enum class Result { Running, Done };

    // Hold the frame counter at most this long for the layout gate before
    // proceeding ungated.
    static constexpr double kLayoutGateTimeoutMs = 10000.0;
    // Wall-clock pacing per pump iteration.
    static constexpr int    kPaceMs = 16;

    CaptureTicker(int frames, std::wstring enginePngPath)
        : m_frames(frames), m_enginePng(std::move(enginePngPath)) {}

    // uiReady / layoutReady: React's first paint and first scene-rect.
    void SetHooks(SleepFn sleep, NowMsFn now, ReadyFn uiReady, ReadyFn layoutReady,
                  StepClockFn stepClock, LogFn log);
    // writeEngineRt / writeComposite: the two PNG writers; waitForUi: the
    // app/ready wait + settle that precedes the composite.
    void SetCaptureHooks(WriteFn writeEngineRt, WriteFn writeComposite,
                         WaitForUiFn waitForUi);

    // One pump iteration, called right after the frame renders. Returns Done
    // once the target frame has been written and the pump should quit.
    Result Tick();

    // A setup failure (bad load, unresolved reference object, ...) also
    // fails the run.
    void MarkFailed() { m_failed = true; }
    bool Failed() const { return m_failed; }
    // 2 = bad load / failed engine-RT write, 0 = success. A UI timeout only
    // degrades the composite's name; it never fails the run.
    int  ExitCode() const { return m_failed ? 2 : 0; }
    int  CapturedFrames() const { return m_capturedFrames; }

private:
    void Log(const char* fmt, ...);

    int          m_frames;
    std::wstring m_enginePng;

    SleepFn     m_sleep;
    NowMsFn     m_now;
    ReadyFn     m_uiReady;
    ReadyFn     m_layoutReady;
    StepClockFn m_stepClock;
    LogFn       m_log;
    WriteFn     m_writeEngineRt;
    WriteFn     m_writeComposite;
    WaitForUiFn m_waitForUi;

    bool   m_failed         = false;
    bool   m_done           = false;
    int    m_capturedFrames = 0;
    bool   m_gateStarted    = false;
    double m_gateStartMs    = 0.0;
    bool   m_gateWarned     = false;
};

}  // namespace host

#endif  // HOST_CAPTURE_TICKER_H
