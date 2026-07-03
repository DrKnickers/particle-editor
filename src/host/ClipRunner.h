#ifndef HOST_CLIP_RUNNER_H
#define HOST_CLIP_RUNNER_H

#include <functional>
#include <map>
#include <string>
#include <utility>       // std::move (SetPathTokens setter)
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
    using UiPushFn    = std::function<void(const std::string& kind,const nlohmann::json& params)>; // ui/* -> webview verbatim
    using AckFn       = std::function<bool(int frameId,double deadlineMs)>; // pumped wait; false=timeout
    // Target-bearing cursor only: read back the last frame-acked cursor payload
    // the web side computed — {x,y,vis,press, resolved:[{ref,x,y,ok}]}. The host
    // captures it in OnWebMessage when ui/frame-acked carries a `cursor` object.
    using AckDataFn   = std::function<nlohmann::json(int frameId)>;
    using CaptureFn   = std::function<bool(int frameIndex)>;               // capture frame N (host owns path)
    using LogFn       = std::function<void(const std::string&)>;

    enum class Status { Running, Done };

    // Path tokens expanded in timeline `open` + at-event params at Init (e.g.
    // {"GAME": "<game root>"} lets a timeline say "${GAME}/Mods/X"). Must be set
    // BEFORE Init. Absent tokens are fine unless a timeline actually uses one, in
    // which case Init fails loud (exit 2). See ClipPathTokens.h.
    void SetPathTokens(std::map<std::string, std::string> tokens) { m_pathTokens = std::move(tokens); }

    bool Init(const std::string& timelineJson, std::string& err);  // false + exitCode=2 on bad timeline

    void SetHooks(DispatchFn d, StepFn s, CursorFn c, AckFn a, CaptureFn cap, LogFn log,
                  UiPushFn ui = {}, AckDataFn ackData = {});

    Status Tick();                       // advance exactly one emitted frame
    int    ExitCode() const { return m_exitCode; }
    int    FrameCount() const { return clip::FrameCount(m_tl); }
    int    CurrentFrame() const { return m_frame; }   // next frame Tick() will emit (for the ui/cursor `frame` echo)
    const clip::Timeline& TL() const { return m_tl; }

    // True when the cursor track uses semantic targets (element/point) rather
    // than literal x/y — the host streams the track once + ticks per frame and
    // writes the verify sidecar at the end.
    bool IsTargetCursor() const { return m_targetCursor; }
    // Per-frame verify sidecar (JSON array of {frame,cursor,resolved}); only
    // populated on a target-bearing run, written by the host on success.
    const nlohmann::json& Sidecar() const { return m_sidecar; }

    static constexpr double kAckDeadlineMs = 2000.0;

private:
    clip::Timeline m_tl;
    int    m_frame = 0;
    int    m_exitCode = 0;
    int    m_reqId = 0;
    size_t m_nextAt = 0;         // index of the next un-fired at-event (ats are sorted)
    bool   m_done = false;
    std::map<std::string, std::string> m_pathTokens;   // ${TOKEN} table (host-supplied, pre-Init)

    DispatchFn m_dispatch; StepFn m_step; CursorFn m_cursor;
    AckFn m_ack; CaptureFn m_capture; LogFn m_log; UiPushFn m_uiPush;
    AckDataFn m_ackData;

    bool m_targetCursor = false;          // cursor track uses semantic targets
    nlohmann::json m_sidecar = nlohmann::json::array();  // verify sidecar (target run only)

    bool m_preflighted = false;
    bool Preflight();   // verify every track-key target exists; false + exit 3 on miss

    bool DispatchKind(const std::string& kind, const nlohmann::json& params);  // false + exit 3 on fail
};

}  // namespace host
#endif  // HOST_CLIP_RUNNER_H
