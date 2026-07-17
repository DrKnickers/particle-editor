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
#include <map>
#include "third_party/nlohmann/json.hpp"
#include "DriveScript.h"   // drive::ComputeOrbitCamera, IsAllowedBridgeKind, Build/Classify
#include "ClipPathTokens.h" // ${GAME}-style path-token expansion (pure)

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

struct TrackKeyTween {
    int         id = -1;
    std::string track;            // "red".."alpha","scale","index","rotationSpeed"
    double      keyTime = 0;      // key time in [0,100]; use a border (0 or 100) for a guaranteed-present key
    double      fromValue = 0, toValue = 0;
    double      t0 = 0, t1 = 0;
    Ease        ease = Ease::Linear;
};

struct CursorTarget {
    enum class Kind { None, Element, Point } kind = Kind::None;
    std::string ref;
    double x = 0, y = 0;
};

struct CursorKey {
    double t = 0, x = 0, y = 0;
    bool vis = false, press = false;
    // Opt-in: a press on this key ALSO dispatches a real click (+ input focus)
    // on the resolved element web-side (record-cursor-activate.ts). Default
    // false keeps legacy pointer-events-only presses, so existing clips whose
    // theatrical presses sit on live controls (e.g. Spawn now) are untouched.
    bool activate = false;
    // Optional Ctrl/Shift modifiers for the activate-click, threaded to the web
    // side so a clip can drive a real modifier-click (emitter-tree multi-select).
    // Both default false = no modifiers (the common case); serialized into the
    // ui/cursor-track ONLY when set, so existing clips' output is byte-identical.
    bool modCtrl = false;
    bool modShift = false;
    // Optional mouse button for the activate-press: "right" makes the web side
    // dispatch the real right-click sequence INCLUDING `contextmenu`, the only way
    // to open a context menu (e.g. the emitter tree's "Set Link Group…", which has
    // no menubar/toolbar entry). Empty = left (every existing clip); serialized
    // ONLY when set, so legacy keys stay byte-identical.
    std::string button;
    CursorTarget target;
};

struct AtEvent { double t = 0; std::string kind; nlohmann::json params; };

struct Timeline {
    int            fps = 0, width = 0, height = 0;
    double         scale = 1.0;               // WebView chrome rasterization scale (DPR). >1 renders
                                              // the UI at higher pixel density so a zoomed crop stays
                                              // sharp; CSS layout width = width/scale (drives reflow).
    double         durationMs = 0;
    std::string    out;                       // output DIRECTORY (utf-8)
    double         openSettleMs = kDefaultSettleMs;
    bool           loop = false;
    std::string    openPath;                  // from `open` sugar (optional)
    std::string    saveRoot;                  // confinement root for file/save targets (optional;
                                              // REQUIRED by preflight if any file/save event exists)
    std::vector<Tween>         tweens;
    std::vector<TrackKeyTween> trackKeys;
    std::vector<CursorKey>     cursor;            // sorted by t
    std::vector<AtEvent>       ats;               // sorted by t
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

// Per-track value bounds — match the UI's clamp rules so a script can't insert a
// key the engine/save path would corrupt (RGBA*255 byte cast in ParticleSystem.cpp).
inline bool TrackKeyValueInRange(const std::string& track, double v) {
    if (!std::isfinite(v)) return false;
    if (track == "red" || track == "green" || track == "blue" || track == "alpha")
        return v >= 0.0 && v <= 1.0;
    if (track == "scale" || track == "index") return v >= 0.0;
    if (track == "rotationSpeed")             return true;   // signed
    return false;                                            // unknown track name
}

// track-key: lerp the keyframe value from->to over [t0,t1] with easing.
inline double EvalTrackKeyValue(const TrackKeyTween& tw, double t) {
    double u;
    if (tw.t1 <= tw.t0) u = 1.0;                             // zero-width -> settle at `to`
    else u = ApplyEase(tw.ease, (t - tw.t0) / (tw.t1 - tw.t0));
    return tw.fromValue + (tw.toValue - tw.fromValue) * u;
}

struct CursorState { double x = 0, y = 0; bool vis = false, press = false; };

// Smoothstep-eased position between bracketing keys; vis/press STEP from the
// upcoming key. (Literal-track evaluator — target tracks are evaluated web-side.)
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
            const double ue = u * u * (3.0 - 2.0 * u);
            cs.x = a.x + (b.x - a.x) * ue;
            cs.y = a.y + (b.y - a.y) * ue;
            cs.vis = b.vis; cs.press = b.press;  // step state from the upcoming key
            return cs;
        }
    }
    return cs;
}

inline bool CursorTrackIsTargetBearing(const std::vector<CursorKey>& keys) {
    for (const auto& k : keys)
        if (k.target.kind != CursorTarget::Kind::None) return true;
    return false;
}

inline nlohmann::json BuildCursorTrackJson(const std::vector<CursorKey>& keys) {
    nlohmann::json out = { {"type", "ui/cursor-track"}, {"keys", nlohmann::json::array()} };
    for (const auto& k : keys) {
        nlohmann::json target;
        if (k.target.kind == CursorTarget::Kind::Element) {
            target = { {"kind", "element"}, {"ref", k.target.ref} };
        } else if (k.target.kind == CursorTarget::Kind::Point) {
            target = { {"kind", "point"}, {"x", k.target.x}, {"y", k.target.y} };
        } else {
            target = nlohmann::json::object();
        }
        nlohmann::json keyJson = { {"t", k.t}, {"vis", k.vis}, {"press", k.press}, {"activate", k.activate}, {"target", target} };
        if (k.modCtrl || k.modShift) {
            keyJson["mods"] = { {"ctrl", k.modCtrl}, {"shift", k.modShift} };
        }
        if (!k.button.empty()) {
            keyJson["button"] = k.button;
        }
        out["keys"].push_back(keyJson);
    }
    return out;
}

// Record allowlist = the --drive render-state allowlist MINUS engine/set/paused
// (the recorder owns the preview clock; an author toggle would corrupt stepping),
// PLUS the non-modal spawner commands. Loading a particle system creates NO
// preview instance, so without the spawner the recorded viewport stays empty
// (0 emitters / 0 particles). spawner/start {manual,enabled} + spawner/trigger
// spawn one instance whose emitters then run against the stepped clock. These are
// record-only — deliberately NOT added to the shared --drive allowlist (which
// stays a non-mutating snapshot/perf path). engine/action/step-frames is already
// outside drive::IsAllowedBridgeKind.
inline bool IsAllowedRecordKind(const std::string& kind) {
    if (kind == "spawner/start")   return true;
    if (kind == "spawner/trigger") return true;
    if (kind == "spawner/stop")    return true;
    // mods/set-layers lets a timeline clear the mod stack (paths:[]) so a
    // reference-object resolves the BASE-GAME asset, not a mod override. Safe in
    // record: ModManager is constructed ephemeral (automationMode) so SetLayerStack
    // does NOT persist — the daily-driver's mod stack is untouched.
    if (kind == "mods/set-layers") return true;
    // emitters/delete drops an emitter subtree from the in-memory effect (no
    // native dialog). The handler markDirty()s, but --record NEVER writes the
    // .alo back, so the source asset is untouched — this lets a timeline remove
    // an emitter for a render (e.g. the heat-haze passes) without editing the
    // file. `id` is the 0-based emitter-array index; deletion shifts the array,
    // so a timeline removing several must delete highest-index-first.
    if (kind == "emitters/delete") return true;
    // linkGroups/set-membership {ids, groupId} sets (groupId) or clears (groupId:
    // null) a link group for a batch of emitters. In-memory + dialog-free. A BUILDER
    // needs this to hand a clip a clean starting state: the Tutorial-5 example ships
    // its four Debris already in a link group, so the §6 clip — which teaches
    // CREATING the group via the UI (multi-select -> Set Link Group) — must start
    // from an UNLINKED Debris, which only an explicit unlink can produce.
    //
    // NOTE (do NOT repeat the neighbouring "record never writes the .alo back"
    // claim here — it is not true in general): this mutates the in-memory document
    // and markDirty()s it, and `file/save` IS on this list, so a timeline that
    // combines them writes the result to disk. That is exactly what the teardown
    // builders do — but they open a STAGED COPY and save to a DIFFERENT staged path
    // under `saveRoot`. Preflight only confines a save under saveRoot; it does not
    // reject params.path == the opened path, so a careless timeline could still
    // overwrite the asset it opened (a pre-existing hazard shared by emitters/delete
    // + file/save, not introduced here). Never point a mutating builder at an
    // original you care about.
    if (kind == "linkGroups/set-membership") return true;
    // emitters/move {id, direction} reorders the in-memory emitter array (no
    // dialog; .alo not written back). `id` is the 0-based array index and a move
    // shifts the array, so sequential moves in one timeline must account for the
    // shift. The reorder only changes a LIVE preview's draw order after a respawn
    // (spawner/stop+start+trigger) — see tasks/clips/README.md.
    if (kind == "emitters/move") return true;
    // emitters/set-track-key is also drivable as a discrete at-event (not just by
    // the continuous track-key tween) — a clip uses it for distinct atlas-frame
    // "clicks" (set the index track to a chosen frame, with dwells) rather than a
    // per-frame scrub that makes the picker grid jump. In-memory, dialog-free.
    if (kind == "emitters/set-track-key") return true;
    // emitters/add-track-key inserts a NEW key (click-to-add commit) — needed by
    // walkthrough clips whose target curve has mid-track keys (e.g. the tutorial-3
    // 0→peak@10%→0 muzzle pulse), which set-track-key alone can't create (it only
    // moves the two default border keys). In-memory, dialog-free; bad id/track
    // returns sendErr (envelope-checked), unknown-track cannot silent-OK.
    if (kind == "emitters/add-track-key") return true;
    // engine/set/skydome-environment {context, primaryName, secondaryName} is the
    // name-based game-dome selector the Background picker actually uses (NOT the
    // legacy slot-index API). Persist is gated by !m_ephemeral, so record-safe.
    if (kind == "engine/set/skydome-environment") return true;
    // ui/* events are view-only host->webview pushes (panel/picker open state);
    // the ClipRunner routes any ui/-prefixed kind to PostWebMessageAsJson rather
    // than the bridge dispatcher. Allow the two we drive so the parser passes them
    // through to the runner; they never reach IsAllowedBridgeKind.
    if (kind == "ui/show-panel")    return true;
    if (kind == "ui/open-picker")   return true;
    if (kind == "ui/focus-channel") return true;  // focus a curve channel mid-clip
    if (kind == "ui/select-key")    return true;  // select a curve key (lights the atlas preview/highlight)
    if (kind == "ui/pose-drag")     return true;  // pose a frozen reorder drag (chip+gap) for a clip still
    if (kind == "ui/set-picker-search") return true;  // drive a picker's search box (the cursor can't type)
    if (kind == "ui/picker-collapse")   return true;  // force-collapse picker sections (e.g. Heroes) under a search
    // Wiki-media tutorial kinds (pipeline spec §1). Each is guarded, not trusted:
    // file/save is dialog-free ONLY with an explicit params.path, and record
    // additionally confines the target under the timeline's `saveRoot`
    // (ClipRunner::Preflight, SavePathConfine.h) — a pathless save would pop
    // GetSaveFileNameW and hang the unattended run, so preflight rejects it.
    if (kind == "file/save") return true;
    // The add-* handlers are direct engine wrappers (no dialog). Their refusal
    // shape is sendOk({newId:-1}) — top-level success — so ClipRunner validates
    // the response payload and aborts (exit 3) on newId < 0.
    if (kind == "emitters/add-root")           return true;
    if (kind == "emitters/add-lifetime-child") return true;
    if (kind == "emitters/add-death-child")    return true;
    // set-properties silently skips unknown/mistyped patch fields; the handler
    // reports {applied,skipped} and ClipRunner aborts on a non-empty skipped so
    // a typo'd tutorial patch can't record a no-op as success.
    if (kind == "emitters/set-properties")     return true;
    // set-track-lock re-points a channel's track alias (the curve editor's
    // "Lock to" control). REQUIRED before per-channel color keys on a NEW
    // emitter: the default ctor aliases G/B/A onto Red (ParticleSystem.cpp
    // "Point Green, Blue and Alpha tracks to Red"), so without an unlock every
    // per-channel set-track-key funnels into one shared track (renders white).
    // Dialog-free; bad id/channel returns sendErr (envelope-checked).
    if (kind == "emitters/set-track-lock")     return true;
    // preview/* — the scripted mirror of the native Shift-hover spawn
    // (BridgeDispatch_Spawner.cpp), added for the guide's ref-shift-preview
    // clip: attach spawns a cursor-bound instance at an unprojected client
    // (x,y), move re-positions it along the cursor path, place detaches it
    // (Shift-click), kill removes it (Shift release). Dialog-free, in-memory,
    // engine-only; a misordered call (move/place/kill with nothing attached)
    // is SendErr, which aborts the run — record-only, never on the drive list.
    if (kind == "preview/attach") return true;
    if (kind == "preview/move")   return true;
    if (kind == "preview/place")  return true;
    if (kind == "preview/kill")   return true;
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

inline std::vector<std::string> SplitCursorRef(const std::string& ref) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t pos = ref.find(':', start);
        parts.push_back(ref.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return parts;
}

inline bool IsValidCursorElementRef(const std::string& ref) {
    // testid: the id is a free-form data-testid value that may itself contain ':',
    // so accept by prefix (non-empty remainder) before the per-segment non-empty
    // check the structured refs rely on. Mirrors the web parseElementRef.
    if (ref.rfind("testid:", 0) == 0) return ref.size() > 7;  // "testid:".size() == 7
    const auto parts = SplitCursorRef(ref);
    for (const auto& part : parts)
        if (part.empty()) return false;
    if (parts.size() == 3 && parts[0] == "curve-key") return true;
    if (parts.size() == 2 && parts[0] == "atlas-tile") return true;
    if (parts.size() == 2 && parts[0] == "channel-row") return true;
    return false;
}

inline bool ParseCursorTarget(const nlohmann::json& value, CursorTarget& out, std::string& err) {
    out = CursorTarget{};
    if (!value.is_object()) { err = "cursor target must be an object"; return false; }
    const std::string kind = value.value("kind", std::string{});
    if (kind == "element") {
        if (!value.contains("ref") || !value["ref"].is_string()) {
            err = "cursor element target needs string 'ref'"; return false;
        }
        const std::string ref = value["ref"].get<std::string>();
        if (!IsValidCursorElementRef(ref)) {
            err = "cursor element target has malformed 'ref'"; return false;
        }
        out.kind = CursorTarget::Kind::Element;
        out.ref = ref;
        return true;
    }
    if (kind == "point") {
        if (!value.contains("x") || !value["x"].is_number()
            || !value.contains("y") || !value["y"].is_number()) {
            err = "cursor point target needs numeric 'x' and 'y'"; return false;
        }
        const double x = value["x"].get<double>();
        const double y = value["y"].get<double>();
        if (!std::isfinite(x) || !std::isfinite(y)) {
            err = "cursor point target 'x' and 'y' must be finite"; return false;
        }
        out.kind = CursorTarget::Kind::Point;
        out.x = x; out.y = y;
        return true;
    }
    err = "cursor target has unknown kind"; return false;
}

// Expand ${TOKEN}s across a parsed timeline's PATH-BEARING fields only: the `open`
// sugar, the `saveRoot` confinement root, an explicit file/open OR file/save event's
// `path`, and each mods/set-layers `paths` entry. Deliberately NOT every param string
// — a value like a ui/set-picker-search `text` is arbitrary user text that could
// legitimately contain "${...}" and must not be treated as a token (which would
// wrongly fail-loud). A timeline with no ${...} in these fields is unchanged even when
// `tokens` is empty; a ${...} with no matching token FAILS LOUD (err set, false
// returned) rather than resolving to a broken path. Save fields are expanded here so
// the preflight save-confinement check (ClipRunner::PreflightSaves) sees the RESOLVED
// paths, not the literal ${...} — otherwise a tokenized save always fails confinement.
// Separate from ParseTimeline so both stay independently testable.
inline bool ExpandTimelineTokens(Timeline& tl,
                                 const std::map<std::string, std::string>& tokens,
                                 std::string& err) {
    if (!tl.openPath.empty()) {
        std::string e;
        std::string v = ExpandPathTokens(tl.openPath, tokens, e);
        if (!e.empty()) { err = "open: " + e; return false; }
        tl.openPath = v;
    }
    if (!tl.saveRoot.empty()) {
        std::string e;
        std::string v = ExpandPathTokens(tl.saveRoot, tokens, e);
        if (!e.empty()) { err = "saveRoot: " + e; return false; }
        tl.saveRoot = v;
    }
    for (auto& ev : tl.ats) {
        std::string e;
        if (ev.kind == "file/open" || ev.kind == "file/save") {
            auto it = ev.params.find("path");
            if (it != ev.params.end() && it->is_string()) {
                std::string v = ExpandPathTokens(it->get<std::string>(), tokens, e);
                if (!e.empty()) { err = ev.kind + ": " + e; return false; }
                *it = v;
            }
        } else if (ev.kind == "mods/set-layers") {
            auto it = ev.params.find("paths");
            if (it != ev.params.end() && it->is_array()) {
                for (auto& pe : *it) {
                    if (!pe.is_string()) continue;
                    std::string v = ExpandPathTokens(pe.get<std::string>(), tokens, e);
                    if (!e.empty()) { err = ev.kind + ": " + e; return false; }
                    pe = v;
                }
            }
        }
    }
    return true;
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

    // Optional render scale (WebView chrome DPR). >1 renders the UI at higher pixel
    // density so a zoomed crop (e.g. the F4 mod picker) stays sharp. Cursor point
    // coords are device px, so author them at width/height scale; element refs
    // auto-scale (getBoundingClientRect × devicePixelRatio). Default 1.0.
    if (j.contains("scale")) {
        if (!j["scale"].is_number()) { err = "'scale' must be a number"; return false; }
        out.scale = j["scale"].get<double>();
        if (out.scale < 1.0 || out.scale > 4.0) { err = "'scale' must be in [1.0, 4.0]"; return false; }
    }

    if (60 % out.fps != 0) { err = "'fps' must divide 60 (e.g. 60, 30, 20, 15)"; return false; }
    // The chrome reflows on CSS width, which is width/scale (scale renders the same
    // CSS layout at higher device px), so the min-layout floor applies there.
    if (out.width / out.scale < kMinWidth) { err = "'width'/'scale' (CSS layout width) below the min-layout floor (chrome reflows)"; return false; }
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

    // saveRoot: shape-only here (non-empty string). Whether it is present when a
    // file/save event needs it — and whether each save target resolves under it —
    // is a PREFLIGHT concern (exit 3), keeping parse errors purely structural.
    if (j.contains("saveRoot")) {
        if (!j["saveRoot"].is_string() || j["saveRoot"].get<std::string>().empty()) {
            err = "'saveRoot' must be a non-empty string"; return false;
        }
        out.saveRoot = j["saveRoot"].get<std::string>();
    }

    for (const auto& tr : j["tracks"]) {
        if (!tr.is_object()) { err = "each track must be an object"; return false; }
        const int kinds = (int)tr.contains("tween") + (int)tr.contains("cursor") + (int)tr.contains("at");
        if (kinds != 1) { err = "each track is exactly one of tween|cursor|at"; return false; }

        if (tr.contains("tween")) {
            const std::string tname = tr["tween"].is_string() ? tr["tween"].get<std::string>() : "";
            if (tname == "camera-orbit") {
                Tween tw;
                tw.name = tname;
                tw.from = tr.contains("from") ? tr["from"] : nlohmann::json::object();
                tw.to   = tr.contains("to")   ? tr["to"]   : nlohmann::json::object();
                tw.t0 = tr.value("t0", 0.0); tw.t1 = tr.value("t1", 0.0);
                if (!ParseEase(tr, tw.ease)) { err = "unknown ease"; return false; }
                out.tweens.push_back(std::move(tw));
            } else if (tname == "track-key") {
                TrackKeyTween tk;
                if (!tr.contains("id") || !tr["id"].is_number_integer()) { err = "track-key needs integer 'id'"; return false; }
                tk.id = tr["id"].get<int>();
                if (tk.id < 0) { err = "track-key 'id' must be >= 0"; return false; }
                tk.track = tr.value("track", std::string{});
                if (tk.track.empty()) { err = "track-key needs non-empty 'track'"; return false; }
                tk.keyTime = tr.value("keyTime", 0.0);
                if (!(tk.keyTime >= 0.0 && tk.keyTime <= 100.0)) { err = "track-key 'keyTime' must be 0..100"; return false; }
                if (!tr.contains("from") || !tr["from"].is_object() || !tr["from"].contains("value")
                    || !tr["from"]["value"].is_number()) { err = "track-key needs numeric 'from.value'"; return false; }
                if (!tr.contains("to") || !tr["to"].is_object() || !tr["to"].contains("value")
                    || !tr["to"]["value"].is_number()) { err = "track-key needs numeric 'to.value'"; return false; }
                tk.fromValue = tr["from"]["value"].get<double>();
                tk.toValue   = tr["to"]["value"].get<double>();
                if (!TrackKeyValueInRange(tk.track, tk.fromValue)
                    || !TrackKeyValueInRange(tk.track, tk.toValue)) {
                    err = "track-key value out of range for track '" + tk.track + "'"; return false;
                }
                tk.t0 = tr.value("t0", 0.0); tk.t1 = tr.value("t1", 0.0);
                if (!ParseEase(tr, tk.ease)) { err = "unknown ease"; return false; }
                out.trackKeys.push_back(std::move(tk));
            } else {
                err = "unknown tween: " + tname; return false;
            }
        } else if (tr.contains("cursor")) {
            // Exactly one cursor track per timeline. The literal-vs-target mixing
            // guard below is per-track; a SECOND cursor track would merge into the
            // shared `out.cursor` and could mix modes across tracks (a literal key
            // would then serialize as target:{} and the web parser would reject the
            // whole stream). out.cursor non-empty here ⇒ a prior cursor track ran.
            if (!out.cursor.empty()) { err = "only one cursor track is allowed per timeline"; return false; }
            if (!tr["cursor"].is_array()) { err = "'cursor' must be an array"; return false; }
            double lastT = -1e18;
            bool trackHasTarget = false;
            bool trackHasLiteral = false;
            for (const auto& k : tr["cursor"]) {
                if (!k.is_object()) { err = "cursor key must be an object"; return false; }
                CursorKey ck;
                ck.t = k.value("t", 0.0);
                ck.vis = k.value("vis", false); ck.press = k.value("press", false);
                if (k.contains("activate")) {
                    if (!k["activate"].is_boolean()) { err = "cursor key 'activate' must be a boolean"; return false; }
                    ck.activate = k["activate"].get<bool>();
                }
                if (k.contains("button")) {
                    if (!k["button"].is_string()) { err = "cursor key 'button' must be a string"; return false; }
                    const std::string b = k["button"].get<std::string>();
                    // Fail loud on a typo: silently degrading to a left-click would
                    // open no context menu and the clip would record a no-op.
                    if (b != "left" && b != "right") { err = "cursor key 'button' must be \"left\" or \"right\""; return false; }
                    if (b == "right") ck.button = b;   // left is the default; keep it unserialized
                }
                if (k.contains("mods")) {
                    const auto& m = k["mods"];
                    if (!m.is_object()) { err = "cursor key 'mods' must be an object"; return false; }
                    if (m.contains("ctrl")) {
                        if (!m["ctrl"].is_boolean()) { err = "cursor key 'mods.ctrl' must be a boolean"; return false; }
                        ck.modCtrl = m["ctrl"].get<bool>();
                    }
                    if (m.contains("shift")) {
                        if (!m["shift"].is_boolean()) { err = "cursor key 'mods.shift' must be a boolean"; return false; }
                        ck.modShift = m["shift"].get<bool>();
                    }
                }
                const bool hasTarget = k.contains("target");
                const bool hasX = k.contains("x");
                const bool hasY = k.contains("y");
                if (hasTarget) {
                    if (hasX || hasY) { err = "cursor target keys must not include x/y"; return false; }
                    if (!k.contains("t") || !k["t"].is_number()) { err = "cursor target key needs numeric 't'"; return false; }
                    if (!k.contains("vis") || !k["vis"].is_boolean()
                        || !k.contains("press") || !k["press"].is_boolean()) {
                        err = "cursor target key needs boolean 'vis' and 'press'"; return false;
                    }
                    if (!ParseCursorTarget(k["target"], ck.target, err)) return false;
                    trackHasTarget = true;
                } else {
                    if (!hasX && !hasY) { err = "cursor key needs x/y or target"; return false; }
                    if (!hasX || !hasY || !k["x"].is_number() || !k["y"].is_number()) {
                        err = "cursor literal key needs numeric 'x' and 'y'"; return false;
                    }
                    ck.x = k.value("x", 0.0); ck.y = k.value("y", 0.0);
                    trackHasLiteral = true;
                }
                if (ck.t < lastT) { err = "cursor keyframe times must be non-decreasing"; return false; }
                lastT = ck.t; out.cursor.push_back(ck);
            }
            if (trackHasTarget && trackHasLiteral) { err = "cursor track cannot mix target and literal keys"; return false; }
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
