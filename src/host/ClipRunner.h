#ifndef HOST_CLIP_RUNNER_H
#define HOST_CLIP_RUNNER_H

#include <functional>
#include <string>
#include "ClipTimeline.h"

namespace host {

// Drives a parsed --record timeline forward, ONE emitted frame per Tick().
// Unlike DriveRunner (which never blocks), each Tick performs a full frame
// including the bounded, pumped per-frame ack wait (delegated to a hook). The
// host pump calls Tick() until Done, then moves the temp sequence to `out`.
class ClipRunner {
public:
    using DispatchFn  = std::function<std::string(const std::string&)>;    // bridge req -> response
    using StepFn      = std::function<void(int frames60)>;                  // StepPreviewFrames(N)
    using CursorFn    = std::function<void(double x,double y,bool vis,bool press)>; // ui/cursor push (device px)
    using AckFn       = std::function<bool(int frameId,double deadlineMs)>; // pumped wait; false=timeout
    using CaptureFn   = std::function<bool(int frameIndex)>;               // capture frame N (host owns path)
    using LogFn       = std::function<void(const std::string&)>;

    enum class Status { Running, Done };

    bool Init(const std::string& timelineJson, std::string& err);  // false + exitCode=2 on bad timeline

    void SetHooks(DispatchFn d, StepFn s, CursorFn c, AckFn a, CaptureFn cap, LogFn log);

    Status Tick();                       // advance exactly one emitted frame
    int    ExitCode() const { return m_exitCode; }
    int    FrameCount() const { return clip::FrameCount(m_tl); }
    int    CurrentFrame() const { return m_frame; }   // next frame Tick() will emit (for the ui/cursor `frame` echo)
    const clip::Timeline& TL() const { return m_tl; }

    static constexpr double kAckDeadlineMs = 2000.0;

private:
    clip::Timeline m_tl;
    int    m_frame = 0;
    int    m_exitCode = 0;
    int    m_reqId = 0;
    size_t m_nextAt = 0;         // index of the next un-fired at-event (ats are sorted)
    bool   m_done = false;

    DispatchFn m_dispatch; StepFn m_step; CursorFn m_cursor;
    AckFn m_ack; CaptureFn m_capture; LogFn m_log;

    bool DispatchKind(const std::string& kind, const nlohmann::json& params);  // false + exit 3 on fail
};

}  // namespace host
#endif  // HOST_CLIP_RUNNER_H
