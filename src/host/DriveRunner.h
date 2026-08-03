#ifndef HOST_DRIVE_RUNNER_H
#define HOST_DRIVE_RUNNER_H

#include <functional>
#include <memory>
#include <string>
#include "DriveScript.h"

namespace host {

// Drives a parsed --drive script forward, one non-blocking Tick() per render-pump
// iteration. The pump renders every frame regardless; the runner never blocks it.
class DriveRunner {
public:
    using DispatchFn = std::function<std::string(const std::string&)>;   // -> response envelope
    using CaptureFn  = std::function<bool(const std::string& utf8Path)>; // PrintWindow capture
    using NowMsFn    = std::function<double()>;                          // QPC ms clock
    using LogFn      = std::function<void(const std::string&)>;
    using ProbeFn    = std::function<int(double x0, double y0, double x1, double y1)>;
    using SelftestFn = std::function<bool(const std::string& kind, int timeoutMs)>;

    enum class Status { Running, Done };

    // Parse the script file's contents. Returns false + sets exitCode=2 on a bad script.
    bool Init(const std::string& scriptJson, std::string& err);

    void SetHooks(DispatchFn dispatch, CaptureFn capture, NowMsFn now, LogFn log);
    void SetProbeHook(ProbeFn probe);
    void SetSelftestHook(SelftestFn selftest);

    // Advance one step if ready; never blocks. Call once per pump iteration AFTER
    // RenderD3D9(). Returns Done when finished (read ExitCode()).
    Status Tick();

    int  ExitCode() const { return m_exitCode; }

    // Sum of all settle ms in the script (for the watchdog render-budget cap).
    double TotalSettleMs() const;

private:
    drive::Script m_script;
    size_t m_index = 0;
    int    m_exitCode = 0;
    int    m_reqId = 0;

    // settle bookkeeping
    bool   m_settleActive = false;
    double m_settleDeadline = 0.0;
    double m_lastVisualChangeMs = 0.0;   // when the last visual-changing step landed
    double m_preCaptureFloorMs = 0.0;    // required wait before the next capture (per last step)

    DispatchFn m_dispatch;
    CaptureFn  m_capture;
    NowMsFn    m_now;
    LogFn      m_log;
    ProbeFn    m_probe;
    SelftestFn m_selftest;

    // Dispatch one bridge kind + params; classify the response; on success mark
    // the visual-change time and set the pre-capture floor (larger after file/open,
    // which reloads textures). Returns false (and sets m_exitCode=3) on failure.
    bool DispatchKind(const std::string& kind, const nlohmann::json& params);

    void SetAssertFailed(const std::string& message);

    bool DoBridge(const drive::Step& s);
    bool DoCamera(const drive::Step& s);
    bool DoSelectEmitter(const drive::Step& s);
    bool DoCapture(const drive::Step& s);
    bool DoAssertState(const drive::Step& s);
    bool DoAssertViewportNonblack(const drive::Step& s);
    bool DoBridgeSelftest(const drive::Step& s);
};

}  // namespace host

#endif  // HOST_DRIVE_RUNNER_H
