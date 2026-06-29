#include "ClipRunner.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace host {

bool ClipRunner::Init(const std::string& timelineJson, std::string& err) {
    if (!clip::ParseTimeline(timelineJson, m_tl, err)) { m_exitCode = 2; return false; }
    return true;
}

void ClipRunner::SetHooks(DispatchFn d, StepFn s, CursorFn c, AckFn a, CaptureFn cap, LogFn log) {
    m_dispatch = std::move(d); m_step = std::move(s); m_cursor = std::move(c);
    m_ack = std::move(a); m_capture = std::move(cap); m_log = std::move(log);
}

bool ClipRunner::DispatchKind(const std::string& kind, const nlohmann::json& params) {
    const std::string req = drive::BuildRequestEnvelope(++m_reqId, kind, params);
    const std::string resp = m_dispatch ? m_dispatch(req) : std::string();
    if (drive::ClassifyResponse(resp) != drive::Outcome::Ok) {
        if (m_log) m_log("record: step failed: " + kind);
        m_exitCode = 3; return false;
    }
    return true;
}

bool ClipRunner::Preflight() {
    if (m_tl.trackKeys.empty()) return true;
    // distinct emitter ids referenced by track-key tweens
    std::vector<int> ids;
    for (const auto& tk : m_tl.trackKeys)
        if (std::find(ids.begin(), ids.end(), tk.id) == ids.end()) ids.push_back(tk.id);
    for (int id : ids) {
        const std::string req = drive::BuildRequestEnvelope(++m_reqId, "emitters/get-tracks",
                                                            nlohmann::json{{"id", id}});
        const std::string resp = m_dispatch ? m_dispatch(req) : std::string();
        nlohmann::json j;
        try { j = nlohmann::json::parse(resp); } catch (...) { j = nlohmann::json::object(); }
        const auto tracks = j.contains("data") && j["data"].contains("tracks")
                              ? j["data"]["tracks"] : nlohmann::json::array();
        for (const auto& tk : m_tl.trackKeys) {
            if (tk.id != id) continue;
            bool found = false;
            for (const auto& tr : tracks) {
                if (tr.value("name", std::string{}) != tk.track) continue;
                for (const auto& k : tr.value("keys", nlohmann::json::array()))
                    if (std::fabs(k.value("time", -1.0) - tk.keyTime) < 1e-6) { found = true; break; }
                break;
            }
            if (!found) {
                if (m_log) m_log("record: preflight: no key at time "
                                 + std::to_string(tk.keyTime) + " on track " + tk.track
                                 + " of emitter " + std::to_string(tk.id));
                m_exitCode = 3; return false;
            }
        }
    }
    return true;
}

ClipRunner::Status ClipRunner::Tick() {
    if (m_done) return Status::Done;
    const int N = clip::FrameCount(m_tl);
    if (m_frame >= N) { m_done = true; return Status::Done; }

    if (m_frame == 0 && !m_preflighted) {
        m_preflighted = true;
        if (!Preflight()) { m_done = true; return Status::Done; }
    }

    const double t = clip::FrameTimeMs(m_tl, m_frame);

    // 1. Advance the sim clock one frame (fps divides 60 -> integer).
    if (m_step) m_step(60 / m_tl.fps);

    // 2. Continuous tracks: camera tweens -> engine/set/camera; cursor -> ui/cursor push.
    for (const auto& tw : m_tl.tweens) {
        if (tw.name != "camera-orbit") continue;
        const drive::CameraVecs cv = clip::EvalCameraOrbit(tw, t);
        nlohmann::json p = {
            {"position", {cv.position.x, cv.position.y, cv.position.z}},
            {"target",   {cv.target.x,   cv.target.y,   cv.target.z}},
            {"up",       {cv.up.x,       cv.up.y,       cv.up.z}},
        };
        if (!DispatchKind("engine/set/camera", p)) { m_done = true; return Status::Done; }
    }
    // track-key tweens: scrub one keyframe's value, pinned in time (oldTime==newTime).
    for (const auto& tk : m_tl.trackKeys) {
        const double v = clip::EvalTrackKeyValue(tk, t);
        nlohmann::json p = {
            {"id", tk.id}, {"track", tk.track},
            {"oldTime", tk.keyTime}, {"newTime", tk.keyTime}, {"newValue", v},
        };
        if (!DispatchKind("emitters/set-track-key", p)) { m_done = true; return Status::Done; }
    }
    if (!m_tl.cursor.empty() && m_cursor) {
        const clip::CursorState cs = clip::EvalCursor(m_tl.cursor, t);
        m_cursor(cs.x, cs.y, cs.vis, cs.press);
    }

    // 3. Discrete at-events at or before t, fired once each via the forward index (ats sorted).
    while (m_nextAt < m_tl.ats.size() && m_tl.ats[m_nextAt].t <= t) {
        const clip::AtEvent& ev = m_tl.ats[m_nextAt];
        if (!DispatchKind(ev.kind, ev.params)) { m_done = true; return Status::Done; }
        ++m_nextAt;
    }

    // 4. Commit-ack + grab. Ack timeout is best-effort (log + continue), not fatal.
    if (m_ack && !m_ack(m_frame, kAckDeadlineMs) && m_log)
        m_log("record: ack timeout frame " + std::to_string(m_frame));
    if (m_capture && !m_capture(m_frame)) { m_exitCode = 4; m_done = true; return Status::Done; }

    ++m_frame;
    if (m_frame >= N) { m_done = true; return Status::Done; }
    return Status::Running;
}

}  // namespace host
