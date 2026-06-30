#ifndef HOST_DRIVE_SCRIPT_H
#define HOST_DRIVE_SCRIPT_H

// Pure, host-free script model + helpers for the --drive verb.
// Depends ONLY on nlohmann::json + std so tests/test_drive_script.cpp can
// compile it standalone (no Win32/WebView2/engine). The host glue
// (DriveRunner) converts these POD results into engine/Win32 calls.

#include <cmath>
#include <cctype>
#include <optional>
#include <string>
#include <vector>
#include "third_party/nlohmann/json.hpp"

namespace drive {

struct Vec3d { double x = 0, y = 0, z = 0; };

enum class StepKind { Bridge, Capture, Settle, Camera, SelectEmitter };

struct Step {
    StepKind kind = StepKind::Bridge;
    // Bridge:
    std::string  bridgeKind;          // e.g. "engine/set/model-shadows"
    nlohmann::json params;            // params object (may be null)
    // Capture:
    std::string  capturePath;         // utf-8; host converts to wide
    // Settle:
    double       settleMs = 0;
    // Camera (orbit):
    double       yaw = 0, pitch = 0, dist = 0;
    Vec3d        target;
    // SelectEmitter (exactly one of these non-empty):
    std::string  selName;
    std::string  selStableId;
};

struct Script {
    double            defaultSettleMs = 150.0;
    std::vector<Step> steps;
};

enum class Outcome { Ok, Failed, Malformed };

struct CameraVecs { Vec3d position, target, up; };

// id is set iff an emitter matched (found == id.has_value()); duplicateName is a
// diagnostic side-channel (true when a name matched >1 node).
struct EmitterResolve { std::optional<int> id; bool duplicateName = false; };

// --- allowlist -------------------------------------------------------------

inline bool IsAllowedBridgeKind(const std::string& kind)
{
    // v1 allowlist: render-state setters + file open + emitter list/select.
    // Everything else (save/save-as/browse/quit/mods/debug/emitter-mutations)
    // is rejected — several open modal native dialogs that would deadlock an
    // unattended run on the single UI thread (spec section 4.6).
    if (kind == "engine/set/background")       return true;
    if (kind == "engine/set/bloom")            return true;
    if (kind == "engine/set/bloom-strength")   return true;
    if (kind == "engine/set/bloom-cutoff")     return true;
    if (kind == "engine/set/bloom-size")       return true;
    if (kind == "engine/set/camera")           return true;
    if (kind == "engine/set/grid-visible")     return true;
    if (kind == "engine/set/grid-spacing")     return true;
    if (kind == "engine/set/ground")           return true;
    if (kind == "engine/set/ground-z")         return true;
    if (kind == "engine/set/ground-texture")   return true;
    if (kind == "engine/set/model-shadows")    return true;
    if (kind == "engine/set/msaa-level")       return true;
    if (kind == "engine/set/overload-guard")   return true;
    if (kind == "engine/set/paused")           return true;
    if (kind == "engine/set/reference-object") return true;
    if (kind == "engine/set/reference-object-visible") return true;
    if (kind == "engine/set/reference-object-lock") return true;   // lock => never selected => no gizmo/selection box
    if (kind == "engine/set/shadow")           return true;
    if (kind == "engine/set/soft-shadows")     return true;
    if (kind == "file/open")               return true;   // path-required (checked separately)
    if (kind == "emitters/list")           return true;
    if (kind == "emitters/select")         return true;
    if (kind == "layout/scene-rect")       return true;   // resize/layout perf probe; no persistence
    if (kind == "textures/get-preview")    return true;   // read-only decode path for perf audit
    return false;
}

inline bool FileOpenHasPath(const nlohmann::json& params)
{
    return params.is_object() && params.contains("path") &&
           params["path"].is_string() && !params["path"].get<std::string>().empty();
}

inline bool IsFiniteMs(double value)
{
    return std::isfinite(value) && value >= 0.0 && value <= 60000.0;
}

inline bool IsSafeArtifactRelativePath(const std::string& path)
{
    if (path.empty()) return false;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':')
        return false;
    if (path[0] == '/' || path[0] == '\\') return false;
    std::string segment;
    for (char ch : path) {
        if (ch == '/' || ch == '\\') {
            if (segment == "..") return false;
            segment.clear();
        } else {
            segment.push_back(ch);
        }
    }
    return segment != "..";
}

// --- request envelope + response classification ----------------------------

inline std::string BuildRequestEnvelope(int id, const std::string& kind, const nlohmann::json& params)
{
    nlohmann::json req = {
        {"type", "req"},
        {"id", std::to_string(id)},
        {"kind", kind},
        {"params", params.is_null() ? nlohmann::json::object() : params},
    };
    return req.dump();
}

inline Outcome ClassifyResponse(const std::string& envelopeJson)
{
    nlohmann::json j;
    try { j = nlohmann::json::parse(envelopeJson); }
    catch (...) { return Outcome::Malformed; }
    // `ok` must be present AND boolean; anything else is a malformed envelope
    // (the real bridge always sends a boolean ok via sendOk/sendErr).
    if (!j.is_object() || !j.contains("ok") || !j["ok"].is_boolean()) return Outcome::Malformed;
    if (j["ok"].get<bool>() == false) return Outcome::Failed;
    // top-level ok:true -- inspect nested data.ok (file/open et al. report failure there)
    if (j.contains("data") && j["data"].is_object()) {
        const auto& d = j["data"];
        if (d.contains("ok") && d["ok"].is_boolean() && d["ok"].get<bool>() == false)
            return Outcome::Failed;
    }
    return Outcome::Ok;
}

// --- orbit camera (Z-up) ---------------------------------------------------

inline CameraVecs ComputeOrbitCamera(double yaw, double pitch, double dist, Vec3d target)
{
    // Spherical orbit around `target`. THE ENGINE IS Z-UP (D3DXMatrixLookAtRH,
    // default Up=(0,0,1); see engine.cpp ~2522/5659). pitch = elevation above
    // the XY horizon plane, yaw = azimuth about Z. yaw=pitch=0 -> eye on +Y at
    // the horizon (z=0); pitch>0 raises the eye toward +Z (the pole, singular
    // at +/-90deg -- authors avoid the exact pole).
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double cy = std::cos(yaw),   sy = std::sin(yaw);
    Vec3d dir{ cp * sy, cp * cy, sp };  // unit direction from target to eye (Z-up)
    CameraVecs cv;
    cv.target = target;
    cv.position = Vec3d{ target.x + dir.x * dist, target.y + dir.y * dist, target.z + dir.z * dist };
    cv.up = Vec3d{ 0, 0, 1 };           // Z-up
    return cv;
}

// --- emitter resolve -------------------------------------------------------

namespace detail {
// stableId arrives NUMERIC on the wire (BuildEmitterTreeNode emits an int);
// read it type-tolerantly so node.value("stableId", std::string{}) never
// throws type_error.302 on a numeric node.
inline std::string NodeStableIdStr(const nlohmann::json& n)
{
    if (!n.contains("stableId")) return {};
    const auto& v = n["stableId"];
    if (v.is_string())           return v.get<std::string>();
    if (v.is_number_integer())   return std::to_string(v.get<long long>());
    if (v.is_number_unsigned())  return std::to_string(v.get<unsigned long long>());
    return {};
}

inline bool ResolveNode(const nlohmann::json& node, const std::string& name,
                        const std::string& stableId, EmitterResolve& out, int& nameHits)
{
    if (!node.is_object()) return false;
    if (!stableId.empty()) {
        if (NodeStableIdStr(node) == stableId) {
            out.id = node.value("id", -1); return true;
        }
    } else if (!name.empty()) {
        if (node.value("name", std::string{}) == name) {
            ++nameHits;
            if (!out.id.has_value()) out.id = node.value("id", -1);
        }
    }
    if (node.contains("children") && node["children"].is_array())
        for (const auto& c : node["children"])
            if (ResolveNode(c, name, stableId, out, nameHits) && !stableId.empty())
                return true;  // stableId is unique -> stop on first hit
    // The `&& !stableId.empty()` is LOAD-BEARING: in name-mode it makes this
    // return inert (always false) so the caller's loop keeps walking the whole
    // tree to count duplicate name hits. Do NOT simplify to `out.id.has_value()`.
    return out.id.has_value() && !stableId.empty();
}
}  // namespace detail

inline EmitterResolve ResolveEmitterId(const std::string& listEnvelope,
                                       const std::string& name, const std::string& stableId)
{
    // Real shape: sendOk({root: tree}) -> data.root, a synthetic root whose
    // children are the top-level emitters. Walk the root's children (the root
    // itself has stableId:0/name:"" and must not false-match).
    EmitterResolve out;
    nlohmann::json j;
    try { j = nlohmann::json::parse(listEnvelope); } catch (...) { return out; }
    if (!j.contains("data") || !j["data"].is_object()) return out;
    const auto& data = j["data"];
    if (!data.contains("root") || !data["root"].is_object()) return out;
    const auto& root = data["root"];
    int nameHits = 0;
    if (root.contains("children") && root["children"].is_array())
        for (const auto& n : root["children"])
            if (detail::ResolveNode(n, name, stableId, out, nameHits) && !stableId.empty()) break;
    out.duplicateName = (stableId.empty() && nameHits > 1);
    return out;
}

// --- parse + validate ------------------------------------------------------

inline bool ParseScript(const std::string& json, Script& out, std::string& err)
{
    out = Script{};
    try {
    nlohmann::json j;
    try { j = nlohmann::json::parse(json); }
    catch (const std::exception& e) { err = std::string("invalid JSON: ") + e.what(); return false; }

    if (!j.is_object()) { err = "root must be an object"; return false; }

    if (j.contains("settleMs") && j["settleMs"].is_number()) {
        out.defaultSettleMs = j["settleMs"].get<double>();
        if (!IsFiniteMs(out.defaultSettleMs)) {
            err = "'settleMs' must be finite and between 0 and 60000"; return false;
        }
    }

    if (!j.contains("steps") || !j["steps"].is_array()) { err = "missing 'steps' array"; return false; }

    // `open` sugar: a leading file/open. Mutually exclusive with an explicit
    // file/open step (reject the ambiguity rather than silently double-open).
    bool hasExplicitFileOpen = false;
    for (const auto& s : j["steps"])
        if (s.is_object() && s.value("kind", std::string{}) == "file/open")
            hasExplicitFileOpen = true;

    if (j.contains("open")) {
        if (!j["open"].is_string() || j["open"].get<std::string>().empty()) {
            err = "'open' must be a non-empty path string"; return false;
        }
        if (hasExplicitFileOpen) {
            err = "'open' sugar and an explicit file/open step are mutually exclusive"; return false;
        }
        Step openStep;
        openStep.kind = StepKind::Bridge;
        openStep.bridgeKind = "file/open";
        openStep.params = nlohmann::json{{"path", j["open"].get<std::string>()}};
        out.steps.push_back(std::move(openStep));
    }

    for (const auto& s : j["steps"]) {
        if (!s.is_object()) { err = "each step must be an object"; return false; }
        Step step;
        if (s.contains("kind")) {
            step.kind = StepKind::Bridge;
            step.bridgeKind = s["kind"].get<std::string>();
            step.params = s.contains("params") ? s["params"] : nlohmann::json::object();
            if (!IsAllowedBridgeKind(step.bridgeKind)) {
                err = "kind not permitted in --drive: " + step.bridgeKind; return false;
            }
            if (step.bridgeKind == "file/open" && !FileOpenHasPath(step.params)) {
                err = "file/open requires an explicit non-empty 'path'"; return false;
            }
        } else if (s.contains("capture")) {
            if (!s["capture"].is_string() || s["capture"].get<std::string>().empty()) {
                err = "'capture' must be a non-empty path string"; return false;
            }
            step.kind = StepKind::Capture;
            step.capturePath = s["capture"].get<std::string>();
            if (!IsSafeArtifactRelativePath(step.capturePath)) {
                err = "'capture' must be an artifact-relative path"; return false;
            }
        } else if (s.contains("settle")) {
            if (!s["settle"].is_number()) { err = "'settle' must be a number (ms)"; return false; }
            step.kind = StepKind::Settle;
            step.settleMs = s["settle"].get<double>();
            if (!IsFiniteMs(step.settleMs)) {
                err = "'settle' must be finite and between 0 and 60000"; return false;
            }
        } else if (s.contains("camera")) {
            const auto& c = s["camera"];
            if (!c.is_object()) { err = "'camera' must be an object"; return false; }
            step.kind = StepKind::Camera;
            step.yaw   = c.value("yaw", 0.0);
            step.pitch = c.value("pitch", 0.0);
            step.dist  = c.value("dist", 0.0);
            if (c.contains("target") && c["target"].is_array() && c["target"].size() == 3) {
                step.target.x = c["target"][0].get<double>();
                step.target.y = c["target"][1].get<double>();
                step.target.z = c["target"][2].get<double>();
            }
            // NOTE: "fit" is intentionally ignored in v1 (descoped -- spec section 1).
        } else if (s.contains("select-emitter")) {
            const auto& e = s["select-emitter"];
            if (!e.is_object()) { err = "'select-emitter' must be an object"; return false; }
            step.kind = StepKind::SelectEmitter;
            step.selName     = e.value("name", std::string{});
            step.selStableId = e.value("stableId", std::string{});
            if (step.selName.empty() && step.selStableId.empty()) {
                err = "'select-emitter' needs 'name' or 'stableId'"; return false;
            }
            // Exactly one — reject both (else stableId silently wins, dropping
            // name). Mirrors the open-sugar-vs-file/open exclusivity above.
            if (!step.selName.empty() && !step.selStableId.empty()) {
                err = "'select-emitter' takes 'name' OR 'stableId', not both"; return false;
            }
        } else {
            err = "unknown step shape"; return false;
        }
        out.steps.push_back(std::move(step));
    }
    return true;
    } catch (const std::exception& e) {
        out = Script{};
        err = std::string("bad script: ") + e.what();
        return false;
    } catch (...) {
        out = Script{};
        err = "bad script";
        return false;
    }
}

}  // namespace drive

#endif  // HOST_DRIVE_SCRIPT_H
