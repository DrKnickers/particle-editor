#include "ClipRunner.h"
#include "SavePathConfine.h"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace host {

bool ClipRunner::Init(const std::string& timelineJson, std::string& err) {
    if (!clip::ParseTimeline(timelineJson, m_tl, err)) { m_exitCode = 2; return false; }
    // Resolve ${TOKEN}s (e.g. ${GAME}/Mods/X) against the host-supplied table.
    // Fail loud (exit 2) so a moved install / typo'd token can't silently render
    // a broken path — for a mod layer that would quietly drop back to unmodded.
    if (!clip::ExpandTimelineTokens(m_tl, m_pathTokens, err)) { m_exitCode = 2; return false; }
    m_targetCursor = clip::CursorTrackIsTargetBearing(m_tl.cursor);
    return true;
}

void ClipRunner::SetHooks(DispatchFn d, StepFn s, CursorFn c, AckFn a, CaptureFn cap, LogFn log,
                          UiPushFn ui, AckDataFn ackData) {
    m_dispatch = std::move(d); m_step = std::move(s); m_cursor = std::move(c);
    m_ack = std::move(a); m_capture = std::move(cap); m_log = std::move(log);
    m_uiPush = std::move(ui); m_ackData = std::move(ackData);
}

bool ClipRunner::DispatchKind(const std::string& kind, const nlohmann::json& params) {
    const std::string req = drive::BuildRequestEnvelope(++m_reqId, kind, params);
    const std::string resp = m_dispatch ? m_dispatch(req) : std::string();
    if (drive::ClassifyResponse(resp) != drive::Outcome::Ok) {
        if (m_log) m_log("record: step failed: " + kind);
        m_exitCode = 3; return false;
    }
    // Per-kind payload validation (wiki-media pipeline spec §1.4): these kinds
    // report refusals/skips inside a TOP-LEVEL-OK envelope, which
    // ClassifyResponse can't see. A refused add or a skipped property patch
    // recorded as success would silently produce a wrong clip — abort loud.
    const bool isAddKind = kind == "emitters/add-root"
                        || kind == "emitters/add-lifetime-child"
                        || kind == "emitters/add-death-child";
    if (isAddKind || kind == "emitters/set-properties") {
        nlohmann::json j;
        try { j = nlohmann::json::parse(resp); } catch (...) { j = nlohmann::json::object(); }
        const nlohmann::json data = j.contains("data") && j["data"].is_object()
                                      ? j["data"] : nlohmann::json::object();
        if (isAddKind) {
            const long long newId = data.contains("newId") && data["newId"].is_number()
                                      ? data["newId"].get<long long>() : -1;
            if (newId < 0) {
                if (m_log) m_log("record: dispatch: " + kind + " refused (newId="
                                 + std::to_string(newId) + ")");
                m_exitCode = 3; return false;
            }
        } else {
            const nlohmann::json skipped = data.value("skipped", nlohmann::json::array());
            if (skipped.is_array() && !skipped.empty()) {
                if (m_log) m_log("record: dispatch: emitters/set-properties skipped fields: "
                                 + skipped.dump());
                m_exitCode = 3; return false;
            }
        }
    }
    return true;
}

// file/save confinement (spec §1.3): every save event needs an explicit path
// (the dialog fallback would hang an unattended run) resolving under the
// timeline's saveRoot. Runs in preflight so a mis-authored save aborts before
// any frame is captured.
bool ClipRunner::PreflightSaves() {
    for (const auto& ev : m_tl.ats) {
        if (ev.kind != "file/save") continue;
        if (m_tl.saveRoot.empty()) {
            if (m_log) m_log("record: preflight: file/save requires saveRoot");
            m_exitCode = 3; return false;
        }
        std::string pathUtf8;
        if (ev.params.is_object() && ev.params.contains("path") && ev.params["path"].is_string())
            pathUtf8 = ev.params["path"].get<std::string>();
        if (pathUtf8.empty()) {
            if (m_log) m_log("record: preflight: file/save at " + std::to_string(ev.t)
                             + "ms requires params.path (dialog fallback is not record-safe)");
            m_exitCode = 3; return false;
        }
        std::string err;
        if (!clip::ConfineSavePath(clip::ConfineUtf8ToWide(m_tl.saveRoot),
                                   clip::ConfineUtf8ToWide(pathUtf8), nullptr, err)) {
            if (m_log) m_log("record: preflight: file/save at " + std::to_string(ev.t)
                             + "ms rejected: " + err);
            m_exitCode = 3; return false;
        }
    }
    return true;
}

bool ClipRunner::Preflight() {
    if (!PreflightSaves()) return false;
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
        if (m_log)
            m_log("[record-cursor] track=" + std::to_string(m_tl.cursor.size())
                  + " keys, " + (m_targetCursor ? "target-bearing" : "literal"));
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
    // A tween is INACTIVE before its t0: skip the dispatch so the key keeps its
    // authored (or a prior tween's) value until this tween's window opens. Without
    // this, every tween writes its clamped `from` value every frame from t=0, so two
    // tweens on the same key (e.g. a down-scrub then a later restore-to-original)
    // fight — the restore's held `from` would clobber the resting value during the
    // lead-in. After t1 the tween still dispatches (holds `to`), so later tweens in
    // the list win the overlap; that's how a high→low→high arc is expressed.
    for (const auto& tk : m_tl.trackKeys) {
        if (t < tk.t0) continue;
        const double v = clip::EvalTrackKeyValue(tk, t);
        nlohmann::json p = {
            {"id", tk.id}, {"track", tk.track},
            {"oldTime", tk.keyTime}, {"newTime", tk.keyTime}, {"newValue", v},
        };
        if (!DispatchKind("emitters/set-track-key", p)) { m_done = true; return Status::Done; }
    }
    // Cursor: two paths. LITERAL tracks keep posting the resolved (x,y) per frame
    // via the cursor lambda (host owns the device-px geometry). TARGET-BEARING
    // tracks instead post a per-frame {type:"ui/cursor-tick", t, frame}; the web
    // side resolved the selectors against its own live DOM (it streamed the track
    // once during post-settle) and returns the cursor + resolved set in the ack.
    if (!m_tl.cursor.empty()) {
        if (m_targetCursor) {
            if (m_uiPush)
                m_uiPush("ui/cursor-tick",
                         nlohmann::json{{"t", t}, {"frame", m_frame}});
        } else if (m_cursor) {
            const clip::CursorState cs = clip::EvalCursor(m_tl.cursor, t);
            m_cursor(cs.x, cs.y, cs.vis, cs.press);
        }
    }

    // 3. Discrete at-events at or before t, fired once each via the forward index (ats sorted).
    while (m_nextAt < m_tl.ats.size() && m_tl.ats[m_nextAt].t <= t) {
        const clip::AtEvent& ev = m_tl.ats[m_nextAt];
        // ui/* events are view-only: post verbatim to the webview (panel/picker
        // open state, etc.), never the bridge dispatcher. Fire-and-forget — a
        // missing listener is a no-op, so no exit-3 gating like a bridge call.
        if (ev.kind.rfind("ui/", 0) == 0) {
            if (m_uiPush) m_uiPush(ev.kind, ev.params);
        } else if (!DispatchKind(ev.kind, ev.params)) {
            m_done = true; return Status::Done;
        }
        ++m_nextAt;
    }

    // 4. Commit-ack + grab. For a LITERAL track an ack timeout is best-effort
    // (log + continue). For a TARGET track the ack carries the resolve set that
    // step 4a validates — a timeout means we'd capture an UNVALIDATED frame (a
    // press miss could slip through silently), so it's fatal.
    if (m_ack && !m_ack(m_frame, kAckDeadlineMs)) {
        if (m_log) m_log("record: ack timeout frame " + std::to_string(m_frame));
        if (m_targetCursor) { m_exitCode = 3; m_done = true; return Status::Done; }
    }

    // 4a. Target-bearing cursor: read back what the web side resolved this frame.
    // A PRESS frame whose target matched nothing is a mis-authored clip (a click /
    // drag landing on empty space) — abort LOUD (exit 3). A NON-press TRANSIT frame
    // may legitimately pass over a not-yet-mounted target (e.g. the atlas dock mid-
    // open, or a curve key whose channel just defocused); the web hides the cursor
    // for those, so they're benign. Accumulate the resolved set into the sidecar.
    if (m_targetCursor && m_ackData) {
        const nlohmann::json ack = m_ackData(m_frame);
        // Strict shape: a target frame MUST come back with a well-formed cursor +
        // resolved array. A malformed/missing ack is treated as a hard failure
        // rather than silently captured unvalidated.
        if (!ack.is_object() || !ack.contains("cursor") || !ack["cursor"].is_object()
            || !ack.contains("resolved") || !ack["resolved"].is_array()) {
            if (m_log) m_log("[record-cursor] malformed/missing ack at frame " + std::to_string(m_frame));
            m_exitCode = 3; m_done = true; return Status::Done;
        }
        const auto& resolved = ack["resolved"];
        const bool pressed = ack["cursor"].value("press", false);
        if (pressed) {
            for (const auto& r : resolved) {
                if (!r.value("ok", false)) {   // missing/false ok ⇒ unresolved ⇒ fatal on a press
                    const std::string ref = r.value("ref", std::string{});
                    if (m_log)
                        m_log("[record-cursor] press target '" + ref
                              + "' did not resolve at frame " + std::to_string(m_frame));
                    m_exitCode = 3; m_done = true; return Status::Done;
                }
            }
        }
        m_sidecar.push_back({
            {"frame", m_frame},
            {"cursor", ack["cursor"]},
            {"resolved", resolved},
        });
    }

    if (m_capture && !m_capture(m_frame)) { m_exitCode = 4; m_done = true; return Status::Done; }

    ++m_frame;
    if (m_frame >= N) { m_done = true; return Status::Done; }
    return Status::Running;
}

}  // namespace host
