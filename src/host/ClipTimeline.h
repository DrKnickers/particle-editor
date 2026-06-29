#ifndef HOST_CLIP_TIMELINE_H
#define HOST_CLIP_TIMELINE_H

// Pure, host-free timeline model + helpers for the --record verb. Depends ONLY
// on nlohmann::json + std (+ DriveScript.h's pure helpers) so
// tests/test_clip_timeline.cpp compiles standalone. The host glue (ClipRunner +
// HostWindow) converts these POD results into engine/Win32 calls.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "third_party/nlohmann/json.hpp"
#include "DriveScript.h"   // drive::ComputeOrbitCamera, IsAllowedBridgeKind, Build/Classify

namespace clip {

constexpr int    kMinWidth        = 800;   // narrower than this and the React chrome reflows/breaks
constexpr int    kMinHeight       = 400;   // sanity floor only (height doesn't reflow the chrome; 720p is fine)
constexpr double kDefaultSettleMs = 500.0; // post-open paint floor

enum class Ease { Linear, InOutSine };

struct Tween {
    std::string    name;            // "camera-orbit" (v1)
    nlohmann::json from, to;        // param objects (e.g. {yaw,pitch,dist[,target]})
    double         t0 = 0, t1 = 0;  // active window in virtual ms
    Ease           ease = Ease::Linear;
};

struct CursorKey { double t = 0, x = 0, y = 0; bool vis = false, press = false; };

struct AtEvent { double t = 0; std::string kind; nlohmann::json params; };

struct Timeline {
    int            fps = 0, width = 0, height = 0;
    double         durationMs = 0;
    std::string    out;                       // output DIRECTORY (utf-8)
    double         openSettleMs = kDefaultSettleMs;
    bool           loop = false;
    std::string    openPath;                  // from `open` sugar (optional)
    std::vector<Tween>     tweens;
    std::vector<CursorKey> cursor;            // sorted by t
    std::vector<AtEvent>   ats;               // sorted by t
};

// frameCount = round(durationMs * fps / 1000); frame N's virtual time = N*1000/fps.
inline int    FrameCount(const Timeline& tl) {
    return static_cast<int>(std::lround(tl.durationMs * tl.fps / 1000.0));
}
inline double FrameTimeMs(const Timeline& tl, int n) { return n * 1000.0 / tl.fps; }

inline double ApplyEase(Ease e, double u) {
    if (u < 0) u = 0; else if (u > 1) u = 1;
    switch (e) {
        case Ease::InOutSine: return -(std::cos(3.14159265358979323846 * u) - 1.0) / 2.0;
        case Ease::Linear:    default: return u;
    }
}

// Eased progress of a tween at virtual time t (0 before t0, 1 after t1).
inline double TweenU(const Tween& tw, double t) {
    if (tw.t1 <= tw.t0) return 1.0;
    return ApplyEase(tw.ease, (t - tw.t0) / (tw.t1 - tw.t0));
}

// Read a numeric param from `to` if present, else from `from`, else `dflt`.
inline double LerpParam(const Tween& tw, const char* key, double u, double dflt) {
    const double a = tw.from.is_object() ? tw.from.value(key, dflt) : dflt;
    const double b = (tw.to.is_object() && tw.to.contains(key)) ? tw.to.value(key, a) : a;
    return a + (b - a) * u;
}

// camera-orbit: interpolate yaw/pitch/dist; target is constant (from `from`).
inline drive::CameraVecs EvalCameraOrbit(const Tween& tw, double t) {
    const double u = TweenU(tw, t);
    const double yaw  = LerpParam(tw, "yaw",   u, 0.0);
    const double pitch= LerpParam(tw, "pitch", u, 0.0);
    const double dist = LerpParam(tw, "dist",  u, 1.0);
    drive::Vec3d target;
    if (tw.from.is_object() && tw.from.contains("target") && tw.from["target"].is_array()
        && tw.from["target"].size() == 3) {
        target.x = tw.from["target"][0].get<double>();
        target.y = tw.from["target"][1].get<double>();
        target.z = tw.from["target"][2].get<double>();
    }
    return drive::ComputeOrbitCamera(yaw, pitch, dist, target);
}

struct CursorState { double x = 0, y = 0; bool vis = false, press = false; };

// Piecewise-linear position; vis/press take the LATEST key at or before t (step).
inline CursorState EvalCursor(const std::vector<CursorKey>& keys, double t) {
    CursorState cs;
    if (keys.empty()) return cs;
    if (t <= keys.front().t) { cs.x = keys.front().x; cs.y = keys.front().y;
                               cs.vis = keys.front().vis; cs.press = keys.front().press; return cs; }
    if (t >= keys.back().t)  { cs.x = keys.back().x; cs.y = keys.back().y;
                               cs.vis = keys.back().vis; cs.press = keys.back().press; return cs; }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].t) {
            const CursorKey& a = keys[i - 1]; const CursorKey& b = keys[i];
            const double span = b.t - a.t;
            const double u = span > 0 ? (t - a.t) / span : 1.0;
            cs.x = a.x + (b.x - a.x) * u;
            cs.y = a.y + (b.y - a.y) * u;
            cs.vis = b.vis; cs.press = b.press;  // step state from the upcoming key
            return cs;
        }
    }
    return cs;
}

// Record allowlist = the --drive render-state allowlist MINUS engine/set/paused
// (the recorder owns the preview clock; an author toggle would corrupt stepping).
// engine/action/step-frames is already outside drive::IsAllowedBridgeKind.
inline bool IsAllowedRecordKind(const std::string& kind) {
    if (!drive::IsAllowedBridgeKind(kind)) return false;
    if (kind == "engine/set/paused") return false;
    return true;
}

inline bool ParseEase(const nlohmann::json& t, Ease& out) {
    const std::string s = t.value("ease", std::string("linear"));
    if (s == "linear")     { out = Ease::Linear;    return true; }
    if (s == "inOutSine")  { out = Ease::InOutSine;  return true; }
    return false;
}

inline bool ParseTimeline(const std::string& json, Timeline& out, std::string& err) {
    out = Timeline{};
    nlohmann::json j;
    try { j = nlohmann::json::parse(json); }
    catch (const std::exception& e) { err = std::string("invalid JSON: ") + e.what(); return false; }
    if (!j.is_object()) { err = "root must be an object"; return false; }

    // Required numeric fields.
    auto reqPos = [&](const char* k, double& dst, bool intOnly) -> bool {
        if (!j.contains(k) || !j[k].is_number()) { err = std::string("missing/invalid '") + k + "'"; return false; }
        double v = j[k].get<double>();
        if (v <= 0) { err = std::string("'") + k + "' must be > 0"; return false; }
        if (intOnly && v != std::floor(v)) { err = std::string("'") + k + "' must be an integer"; return false; }
        dst = v; return true;
    };
    double fpsD = 0, wD = 0, hD = 0, durD = 0;
    if (!reqPos("fps", fpsD, true) || !reqPos("width", wD, true)
        || !reqPos("height", hD, true) || !reqPos("durationMs", durD, false)) return false;
    out.fps = (int)fpsD; out.width = (int)wD; out.height = (int)hD; out.durationMs = durD;

    if (60 % out.fps != 0) { err = "'fps' must divide 60 (e.g. 60, 30, 20, 15)"; return false; }
    if (out.width < kMinWidth)   { err = "'width' below the min-layout floor (chrome reflows)"; return false; }
    if (out.height < kMinHeight) { err = "'height' below the sanity floor"; return false; }
    // Reject a timeline that rounds to zero frames (silent empty output otherwise).
    if (FrameCount(out) < 1) { err = "'durationMs' * 'fps' yields < 1 frame"; return false; }

    if (!j.contains("out") || !j["out"].is_string() || j["out"].get<std::string>().empty()) {
        err = "missing 'out' output directory"; return false;
    }
    out.out = j["out"].get<std::string>();
    // 'out' is used raw for create_directories / remove_all / rename — reject an
    // absolute path or parent-traversal so a typo/hostile timeline can't nuke an
    // arbitrary directory. Must be a relative path under the launch dir.
    if (out.out.find("..") != std::string::npos) { err = "'out' must not contain '..'"; return false; }
    if (out.out.size() >= 2 && out.out[1] == ':') { err = "'out' must be a relative path (no drive letter)"; return false; }
    if (out.out[0] == '/' || out.out[0] == '\\') { err = "'out' must be a relative path (no leading slash)"; return false; }
    if (j.contains("openSettleMs") && j["openSettleMs"].is_number()) out.openSettleMs = j["openSettleMs"].get<double>();
    // Bound the startup settle (it runs before the per-frame watchdog arms).
    if (out.openSettleMs < 0.0)     out.openSettleMs = 0.0;
    if (out.openSettleMs > 10000.0) out.openSettleMs = 10000.0;
    if (j.contains("loop") && j["loop"].is_boolean()) out.loop = j["loop"].get<bool>();

    if (!j.contains("tracks") || !j["tracks"].is_array()) { err = "missing 'tracks' array"; return false; }

    // open sugar: a leading file/open path; mutually exclusive with an explicit
    // at:file/open event.
    bool hasExplicitOpen = false;
    for (const auto& tr : j["tracks"])
        if (tr.is_object() && tr.contains("at") && tr.value("kind", std::string{}) == "file/open")
            hasExplicitOpen = true;
    if (j.contains("open")) {
        if (!j["open"].is_string() || j["open"].get<std::string>().empty()) { err = "'open' must be a non-empty path"; return false; }
        if (hasExplicitOpen) { err = "'open' sugar and an explicit file/open event are mutually exclusive"; return false; }
        out.openPath = j["open"].get<std::string>();
    }

    for (const auto& tr : j["tracks"]) {
        if (!tr.is_object()) { err = "each track must be an object"; return false; }
        const int kinds = (int)tr.contains("tween") + (int)tr.contains("cursor") + (int)tr.contains("at");
        if (kinds != 1) { err = "each track is exactly one of tween|cursor|at"; return false; }

        if (tr.contains("tween")) {
            Tween tw;
            tw.name = tr["tween"].is_string() ? tr["tween"].get<std::string>() : "";
            if (tw.name != "camera-orbit") { err = "unknown tween: " + tw.name; return false; }
            tw.from = tr.contains("from") ? tr["from"] : nlohmann::json::object();
            tw.to   = tr.contains("to")   ? tr["to"]   : nlohmann::json::object();
            tw.t0 = tr.value("t0", 0.0); tw.t1 = tr.value("t1", 0.0);
            if (!ParseEase(tr, tw.ease)) { err = "unknown ease"; return false; }
            out.tweens.push_back(std::move(tw));
        } else if (tr.contains("cursor")) {
            if (!tr["cursor"].is_array()) { err = "'cursor' must be an array"; return false; }
            double lastT = -1e18;
            for (const auto& k : tr["cursor"]) {
                CursorKey ck;
                ck.t = k.value("t", 0.0); ck.x = k.value("x", 0.0); ck.y = k.value("y", 0.0);
                ck.vis = k.value("vis", false); ck.press = k.value("press", false);
                if (ck.t < lastT) { err = "cursor keyframe times must be non-decreasing"; return false; }
                lastT = ck.t; out.cursor.push_back(ck);
            }
        } else {  // at
            AtEvent ev;
            ev.t = tr.value("at", 0.0);
            ev.kind = tr.value("kind", std::string{});
            ev.params = tr.contains("params") ? tr["params"] : nlohmann::json::object();
            if (!IsAllowedRecordKind(ev.kind)) { err = "kind not permitted in --record: " + ev.kind; return false; }
            if (ev.kind == "file/open" && !drive::FileOpenHasPath(ev.params)) {
                err = "file/open requires an explicit non-empty 'path'"; return false;
            }
            out.ats.push_back(std::move(ev));
        }
    }
    // ClipRunner::Tick fires at-events in ascending time via a single forward
    // index, so the list MUST be sorted (out-of-order JSON would otherwise skip).
    std::sort(out.ats.begin(), out.ats.end(), [](const AtEvent& a, const AtEvent& b){ return a.t < b.t; });
    return true;
}

}  // namespace clip
#endif  // HOST_CLIP_TIMELINE_H
