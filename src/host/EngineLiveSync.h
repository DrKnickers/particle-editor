// Live engine view: decides which command to send the engine host next, keeping only the newest
// request of each kind so a fast orbit or a burst of edits never builds a backlog. Window-free
// and clock-free (callers pass milliseconds); the dispatcher sends what it returns.
#ifndef HOST_ENGINE_LIVE_SYNC_H
#define HOST_ENGINE_LIVE_SYNC_H

#include "EngineLiveFraming.h"
#include "EnginePreviewPipe.h"
#include "host/third_party/nlohmann/json.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace host::engine_live
{
// Edits and resizes arrive in bursts; one command once they pause.
constexpr std::uint64_t kRefreshDebounceMs = 250;
constexpr std::uint64_t kParkDebounceMs    = 200;
// Camera changes smaller than these are not worth a round trip.
constexpr double kCameraAngleEpsilonDegrees = 0.01;
constexpr double kCameraUnitsEpsilon        = 0.01;
// The frame rate is counted over whole windows of this length, so the figure is steady to read.
constexpr std::uint64_t kFpsWindowMs = 1000;

// Reloads and restarts the effect from the saved copy; the dispatcher writes the copy first.
constexpr const char* kRefreshCommand = "live-refresh";

struct LiveCommand
{
    std::string    command;
    nlohmann::json args = nlohmann::json::object();
};

class EngineLiveSync
{
public:
    // Every input that changes the camera (orbit, pan, zoom, reset, timeline, resize) reaches
    // the engine through the camera itself, so the dispatcher reports it once per frame.
    void NoteCamera(const engine_preview::Orbit& orbit, double fieldDegrees)
    {
        const LiveCamera camera{orbit, fieldDegrees};
        if (m_hasSentCamera && IsSameCamera(camera, m_sentCamera)) return;
        m_pendingCamera = camera;
    }
    void NoteViewportSize(int widthPx, int heightPx, std::uint64_t nowMs)
    {
        if (widthPx == m_requestedWidthPx && heightPx == m_requestedHeightPx) return;
        m_requestedWidthPx  = widthPx;
        m_requestedHeightPx = heightPx;
        m_parkDueMs         = nowMs + kParkDebounceMs;
    }
    // live-start parks at the size it was given; later resizes are measured from it.
    void NoteParked(int widthPx, int heightPx)
    {
        m_requestedWidthPx  = widthPx;
        m_requestedHeightPx = heightPx;
        m_parkDueMs.reset();
    }
    void NoteDocumentEdited(std::uint64_t nowMs) { m_refreshDueMs = nowMs + kRefreshDebounceMs; }
    // True for the one-time read of the host's opening camera (see m_anchor).
    bool IsAnchorRequest(const LiveCommand& command) const
    {
        return command.command == "camera" && command.args.value("action", "") == "get";
    }
    // A host that can set its field of view reports its limits with the opening camera.
    void NoteAnchor(const std::array<double, 3>& openingTarget, const std::optional<FieldLimits>& hostFieldLimits)
    {
        m_anchor            = openingTarget;
        m_hostFieldLimits   = hostFieldLimits;
        m_isAnchorRequested = false;
    }
    // A host that cannot set its field keeps the one maps open with.
    FieldLimits HostFieldLimits() const
    {
        return m_hostFieldLimits.value_or(FieldLimits{kHostDefaultFieldDegrees, kHostDefaultFieldDegrees});
    }
    void NoteAnchorFailed() { m_isAnchorRequested = false; }
    // A still pauses the host's clock and moves its camera; put both back for the live view.
    void NoteStillFinished()
    {
        m_isResumePending = true;
        m_hasSentCamera   = false;
    }
    // Records the time from sending a camera to the first streamed frame that shows it.
    void NoteFrameCameraTag(std::uint64_t frameTag, std::uint64_t nowMs)
    {
        if (m_awaitedTag == 0 || frameTag < m_awaitedTag) return;
        m_cameraLatencyMs = static_cast<double>(nowMs - m_tagSentMs);
        m_awaitedTag      = 0;
    }
    std::optional<double> CameraLatencyMs() const { return m_cameraLatencyMs; }
    void                  NoteFrameShown(std::uint64_t nowMs)
    {
        const std::uint64_t windowMs = nowMs - m_fpsWindowStartMs;
        if (windowMs >= kFpsWindowMs)
        {
            m_framesPerSecond  = static_cast<double>(m_framesInWindow) * kFpsWindowMs / windowMs;
            m_framesInWindow   = 0;
            m_fpsWindowStartMs = nowMs;
        }
        ++m_framesInWindow;
    }
    double FramesPerSecond() const { return m_framesPerSecond; }

    // The next command to send, removed from the pending set. Order: re-park first (frames are
    // letterboxed until it lands), then the refresh an edit asked for, then the clock, then the
    // anchor (once), then the camera, which also runs whenever nothing else waits.
    std::optional<LiveCommand> TakeNextCommand(std::uint64_t nowMs)
    {
        if (m_parkDueMs && nowMs >= *m_parkDueMs) return TakePark();
        if (m_refreshDueMs && nowMs >= *m_refreshDueMs)
        {
            m_refreshDueMs.reset();
            return LiveCommand{kRefreshCommand};
        }
        if (m_isResumePending)
        {
            m_isResumePending = false;
            return LiveCommand{"clock", {{"action", "resume"}}};
        }
        if (!m_anchor) return TakeAnchorRequest();
        if (m_pendingCamera) return TakeCamera(nowMs);
        return std::nullopt;
    }

private:
    struct LiveCamera
    {
        engine_preview::Orbit orbit;
        double                fieldDegrees;
    };
    static bool IsSameCamera(const LiveCamera& a, const LiveCamera& b)
    {
        const bool isSameAngles   = std::abs(a.orbit.pitch - b.orbit.pitch) < kCameraAngleEpsilonDegrees &&
                                    std::abs(a.orbit.yaw - b.orbit.yaw) < kCameraAngleEpsilonDegrees &&
                                    std::abs(a.fieldDegrees - b.fieldDegrees) < kCameraAngleEpsilonDegrees;
        const bool isSameDistance = std::abs(a.orbit.distance - b.orbit.distance) < kCameraUnitsEpsilon;
        for (size_t axis = 0; axis < a.orbit.target.size(); ++axis)
            if (std::abs(a.orbit.target[axis] - b.orbit.target[axis]) >= kCameraUnitsEpsilon) return false;
        return isSameAngles && isSameDistance;
    }
    LiveCommand TakePark()
    {
        m_parkDueMs.reset();
        return LiveCommand{"window",
                           {{"action", "park"}, {"width", m_requestedWidthPx}, {"height", m_requestedHeightPx}}};
    }
    std::optional<LiveCommand> TakeAnchorRequest()
    {
        if (m_isAnchorRequested) return std::nullopt;
        m_isAnchorRequested = true;
        return LiveCommand{"camera", {{"action", "get"}}};
    }
    LiveCommand TakeCamera(std::uint64_t nowMs)
    {
        const LiveCamera            camera = *m_pendingCamera;
        const engine_preview::Orbit orbit  = camera.orbit;
        m_pendingCamera.reset();
        m_sentCamera                    = camera;
        m_hasSentCamera                 = true;
        m_awaitedTag                    = ++m_lastTag;
        m_tagSentMs                     = nowMs;
        std::array<double, 3> mapTarget = orbit.target;
        for (size_t axis = 0; axis < mapTarget.size(); ++axis) mapTarget[axis] += (*m_anchor)[axis];
        LiveCommand command{"camera",
                            {{"action", "set"},
                             {"pitch", orbit.pitch},
                             {"yaw", orbit.yaw},
                             {"distance", orbit.distance},
                             {"target", mapTarget},
                             {"tag", m_lastTag}}};
        // An older host refuses an unknown argument, so the field is sent only to one that can set it.
        if (m_hostFieldLimits) command.args["fov"] = camera.fieldDegrees;
        return command;
    }

    std::optional<LiveCamera> m_pendingCamera;
    // The editor reopens a preview map at its saved camera, which looks at the effect, so the
    // host's opening target is where the effect sits on the map. Our camera is relative to the
    // effect; every camera sent is offset by this anchor.
    std::optional<std::array<double, 3>> m_anchor;
    std::optional<FieldLimits>           m_hostFieldLimits;
    bool                                 m_isAnchorRequested = false;
    LiveCamera                           m_sentCamera{};
    bool                                 m_hasSentCamera = false;
    std::uint64_t                        m_lastTag       = 0;
    std::uint64_t                        m_awaitedTag    = 0;
    std::uint64_t                        m_tagSentMs     = 0;
    std::optional<double>                m_cameraLatencyMs;
    int                                  m_requestedWidthPx  = 0;
    int                                  m_requestedHeightPx = 0;
    std::optional<std::uint64_t>         m_parkDueMs;
    std::optional<std::uint64_t>         m_refreshDueMs;
    bool                                 m_isResumePending  = false;
    std::uint64_t                        m_fpsWindowStartMs = 0;
    std::uint64_t                        m_framesInWindow   = 0;
    double                               m_framesPerSecond  = 0.0;
};
} // namespace host::engine_live
#endif
