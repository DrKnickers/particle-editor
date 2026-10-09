// `--tool patch`. See ToolPatch.h for the request and the plan (§3.5) for the
// verification model.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "ToolPatch.h"
#include "ToolIO.h"
#include "ToolSelector.h"
#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "effect/ParticleSystemIO.h"
#include "effect/ParticleSystemJson.h"
#include "effect/PersistedProjection.h"
#include "effect/Rescale.h"

using nlohmann::json;
typedef ParticleSystem::Emitter        Emitter;
typedef ParticleSystem::Emitter::Track Track;

namespace host
{
namespace
{
const size_t kMaxMutations = 256;

// Why a patch stopped: its exit code, a report category, the reason, and the
// mutation it stopped at (-1 when none).
struct Stop
{
    int         code = kExitOk;
    std::string category;
    std::string reason;
    int         op = -1;
};

bool Fail(Stop* stop, int code, const std::string& category, const std::string& reason, int op = -1)
{
    stop->code     = code;
    stop->category = category;
    stop->reason   = reason;
    stop->op       = op;
    return false;
}

// ---- Inspect paths ----------------------------------------------------------
// "emitters[3].groups[1].minX": object keys separated by dots, array indices
// in brackets.

const json* At(const json& root, const std::string& path)
{
    const json* cur = &root;
    size_t i = 0;
    while (i < path.size())
    {
        if (path[i] == '.') { i++; continue; }
        if (path[i] == '[')
        {
            const size_t close = path.find(']', i);
            if (close == std::string::npos) return nullptr;
            const size_t index = std::stoul(path.substr(i + 1, close - i - 1));
            if (!cur->is_array() || index >= cur->size()) return nullptr;
            cur = &(*cur)[index];
            i = close + 1;
            continue;
        }
        const size_t end = path.find_first_of(".[", i);
        const std::string key = path.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!cur->is_object() || !cur->contains(key)) return nullptr;
        cur = &(*cur)[key];
        i = end == std::string::npos ? path.size() : end;
    }
    return cur;
}

void Blank(json& root, const std::string& path)
{
    const json* found = At(root, path);
    if (found) *const_cast<json*>(found) = nullptr;
}

// The first path at which `a` and `b` differ (numbers compare by value).
bool FirstDifference(const json& a, const json& b, const std::string& path, std::string* where)
{
    if (a.is_object() && b.is_object())
    {
        for (auto it = a.begin(); it != a.end(); ++it)
        {
            const std::string p = path.empty() ? it.key() : path + "." + it.key();
            if (!b.contains(it.key())) { *where = p; return true; }
            if (FirstDifference(it.value(), b[it.key()], p, where)) return true;
        }
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key())) { *where = path.empty() ? it.key() : path + "." + it.key(); return true; }
        return false;
    }
    if (a.is_array() && b.is_array())
    {
        if (a.size() != b.size()) { *where = path; return true; }
        for (size_t i = 0; i < a.size(); i++)
            if (FirstDifference(a[i], b[i], path + "[" + std::to_string(i) + "]", where)) return true;
        return false;
    }
    if (a != b) { *where = path.empty() ? "(root)" : path; return true; }
    return false;
}

std::string Shown(const json* value) { return value ? value->dump() : "(absent)"; }

// ---- Request shape (exit 2) -------------------------------------------------

enum class Kind { String, Number, Integer, Scalar, Keys, Dependents };

struct Field
{
    const char* name;
    Kind        kind;
    bool        required;
};

struct OpShape
{
    const char*        name;
    std::vector<Field> fields;
};

const std::vector<OpShape>& OpShapes()
{
    static const std::vector<OpShape> shapes = {
        {"setProperty",      {{"field", Kind::String, true}, {"value", Kind::Scalar, true}}},
        {"setBlendMode",     {{"value", Kind::Number, true}}},
        {"setColorTexture",  {{"name", Kind::String, true}}},
        {"setNormalTexture", {{"name", Kind::String, true}}},
        {"rename",           {{"name", Kind::String, true}}},
        {"rescale",          {{"timeScale", Kind::Number, true}, {"sizeScale", Kind::Number, true}}},
        {"setInterpolation", {{"track", Kind::Integer, true}, {"interpolation", Kind::String, true},
                              {"dependents", Kind::Dependents, false}}},
        {"replaceTrack",     {{"track", Kind::Integer, true}, {"keys", Kind::Keys, true},
                              {"dependents", Kind::Dependents, false}}},
        {"setTrackKey",      {{"track", Kind::Integer, true}, {"time", Kind::Number, true}, {"value", Kind::Number, true},
                              {"newTime", Kind::Number, false}, {"dependents", Kind::Dependents, false}}},
        {"addTrackKey",      {{"track", Kind::Integer, true}, {"time", Kind::Number, true}, {"value", Kind::Number, true},
                              {"dependents", Kind::Dependents, false}}},
    };
    return shapes;
}

bool IsIntegral(const json& v)
{
    return v.is_number_integer() || v.is_number_unsigned() ||
           (v.is_number_float() && v.get<double>() == std::floor(v.get<double>()) && std::fabs(v.get<double>()) < 2147483648.0);
}

bool HasKind(const json& v, Kind kind)
{
    switch (kind)
    {
        case Kind::String:  return v.is_string();
        case Kind::Number:  return v.is_number();
        case Kind::Integer: return v.is_number() && IsIntegral(v);
        case Kind::Scalar:  return v.is_number() || v.is_boolean();
        case Kind::Keys:
            if (!v.is_array()) return false;
            for (const json& key : v)
                if (!key.is_array() || key.size() != 2 || !key[0].is_number() || !key[1].is_number()) return false;
            return true;
        case Kind::Dependents:
            if (!v.is_array()) return false;
            for (const json& d : v)
                if (!d.is_number() || !IsIntegral(d)) return false;
            return true;
    }
    return false;
}

const char* KindName(Kind kind)
{
    switch (kind)
    {
        case Kind::String:     return "a string";
        case Kind::Number:     return "a number";
        case Kind::Integer:    return "an integer";
        case Kind::Scalar:     return "a number or a boolean";
        case Kind::Keys:       return "an array of [time, value] pairs";
        case Kind::Dependents: return "an array of track numbers";
    }
    return "?";
}

bool CheckRequest(const json& request, Stop* stop)
{
    if (!request.is_object()) return Fail(stop, kExitUsage, "request-malformed", "the request must be a JSON object");
    for (auto it = request.begin(); it != request.end(); ++it)
        if (it.key() != "schema" && it.key() != "selector" && it.key() != "mutations")
            return Fail(stop, kExitUsage, "request-malformed", "unknown request key '" + it.key() + "'");
    if (!request.contains("schema") || request["schema"] != 1)
        return Fail(stop, kExitUsage, "request-malformed", "the request needs \"schema\": 1");
    if (!request.contains("selector"))
        return Fail(stop, kExitUsage, "request-malformed", "the request needs a selector");
    if (!request.contains("mutations") || !request["mutations"].is_array())
        return Fail(stop, kExitUsage, "request-malformed", "the request needs a mutations array");
    const json& ops = request["mutations"];
    if (ops.size() > kMaxMutations)
        return Fail(stop, kExitUsage, "request-malformed", "the request has more than 256 mutations");
    for (size_t i = 0; i < ops.size(); i++)
    {
        const json& op = ops[i];
        const int at = static_cast<int>(i);
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string())
            return Fail(stop, kExitUsage, "request-malformed", "each mutation must be an object with an \"op\" string", at);
        const std::string name = op["op"].get<std::string>();
        const OpShape* shape = nullptr;
        for (const OpShape& s : OpShapes())
            if (name == s.name) shape = &s;
        if (!shape) return Fail(stop, kExitUsage, "request-malformed", "unknown op '" + name + "'", at);
        for (auto it = op.begin(); it != op.end(); ++it)
        {
            if (it.key() == "op") continue;
            const Field* field = nullptr;
            for (const Field& f : shape->fields)
                if (it.key() == f.name) field = &f;
            if (!field) return Fail(stop, kExitUsage, "request-malformed", name + " has no key '" + it.key() + "'", at);
            if (!HasKind(it.value(), field->kind))
                return Fail(stop, kExitUsage, "request-malformed", name + "." + it.key() + " must be " + KindName(field->kind), at);
        }
        for (const Field& f : shape->fields)
            if (f.required && !op.contains(f.name))
                return Fail(stop, kExitUsage, "request-malformed", name + " needs '" + f.name + "'", at);
    }
    return true;
}

// ---- Ops ---------------------------------------------------------------------

// What one applied op touched, as inspect paths relative to the emitter:
// `requested` are the fields it asked for; `footprint` adds what a save
// derives from them.
struct Touched
{
    std::vector<std::string> requested;
    std::vector<std::string> footprint;
};

void Add(std::vector<std::string>& to, const std::string& path)
{
    if (std::find(to.begin(), to.end(), path) == to.end()) to.push_back(path);
}

std::string TrackPath(int track) { return "tracks[" + std::to_string(track) + "]"; }

const std::vector<std::string>& RescalePaths()
{
    // Everything DoRescaleEmitter writes (Rescale.cpp), plus groups[1],
    // which a save derives from lifetime.
    static const std::vector<std::string> paths = {
        "skipTime", "freezeTime", "initialDelay", "lifetime", "groups[1]", "burstDelay", "useBursts",
        "nParticlesPerSecond", "nBursts", "nParticlesPerBurst", "groups[0]", "groups[2]", "inwardSpeed",
        "inwardAcceleration", "acceleration", "gravity", "tailSize", "tracks[4]"};
    return paths;
}

// A rule's verdict (an EditResult or a plan): refused when Rejected.
template <typename Verdict>
bool Refuse(const Verdict& result, int op, Stop* stop)
{
    if (result.outcome != EditOutcome::Rejected) return true;
    return Fail(stop, kExitRule, "rule", result.reason, op);
}

// The lock rules for an edit of tracks[track]'s keys or interpolation, and
// the tracks it changes: itself and the channels locked to it.
bool TrackTargets(const Emitter& e, int track, const json& op, int at, std::vector<std::string>* paths, Stop* stop)
{
    if (!Refuse(ValidateTrackEditLocks(e, track, op.contains("dependents")), at, stop)) return false;
    const std::vector<int> locked = LockedDependents(e, track);
    if (op.contains("dependents"))
    {
        std::set<int> given;
        for (const json& d : op["dependents"]) given.insert(d.get<int>());
        if (given != std::set<int>(locked.begin(), locked.end()))
        {
            json expected = locked;
            return Fail(stop, kExitRule, "rule", "dependents must list exactly the channels locked to " + TrackPath(track) +
                        ": " + expected.dump(), at);
        }
    }
    if (track == ParticleSystem::TRACK_ROTATION_SPEED && e.randomRotation)
        return Fail(stop, kExitRule, "rule", "rotationSpeed keys are not saved while randomRotation is on", at);
    Add(*paths, TrackPath(track));
    for (int j : locked) Add(*paths, TrackPath(j));
    return true;
}

bool ApplyOp(ParticleSystem& system, Emitter& e, const json& op, int at, Touched* touched, Stop* stop)
{
    const std::string name = op["op"].get<std::string>();
    std::vector<std::string>& requested = touched->requested;
    std::vector<std::string>& footprint = touched->footprint;

    if (name == "setProperty" || name == "setBlendMode")
    {
        const std::string field = name == "setBlendMode" ? "blendMode" : op["field"].get<std::string>();
        const json& v = op["value"];
        PropertyValue value = {v.is_boolean(), v.is_boolean() && v.get<bool>(), v.is_number() ? v.get<double>() : 0.0};
        if (!Refuse(ValidateSetProperty(field, value), at, stop)) return false;
        ApplySetProperty(e, field, value);
        Add(requested, field);
        for (const std::string& p : PropertyFootprint(field)) Add(footprint, p);
        return true;
    }
    if (name == "setColorTexture" || name == "setNormalTexture")
    {
        const bool color = name == "setColorTexture";
        const std::string texture = op["name"].get<std::string>();
        if (!Refuse(ValidateTextureName(texture, color), at, stop)) return false;
        (color ? e.colorTexture : e.normalTexture) = texture;
        Add(requested, color ? "colorTexture" : "normalTexture");
        Add(footprint, color ? "colorTexture" : "normalTexture");
        return true;
    }
    if (name == "rename")
    {
        const std::string newName = op["name"].get<std::string>();
        if (!Refuse(ValidateRename(system, e, newName), at, stop)) return false;
        e.name = newName;
        Add(requested, "name");
        Add(footprint, "name");
        return true;
    }
    if (name == "rescale")
    {
        const double timeScale = op["timeScale"].get<double>(), sizeScale = op["sizeScale"].get<double>();
        if (!Refuse(ValidateRescale(e, timeScale, sizeScale), at, stop)) return false;
        DoRescaleEmitter(&e, static_cast<float>(timeScale), static_cast<float>(sizeScale));
        for (const std::string& p : RescalePaths()) { Add(requested, p); Add(footprint, p); }
        return true;
    }

    // The track ops.
    const int track = op["track"].get<int>();
    std::vector<std::string> paths;
    if (!TrackTargets(e, track, op, at, &paths, stop)) return false;
    Track& t = *e.tracks[track];
    if (name == "setInterpolation")
    {
        const InterpolationPlan plan = PlanSetTrackInterpolation(t, op["interpolation"].get<std::string>());
        if (!Refuse(plan, at, stop)) return false;
        ApplySetTrackInterpolation(t, plan);
    }
    else if (name == "replaceTrack")
    {
        std::vector<Track::Key> keys;
        for (const json& k : op["keys"])
            keys.push_back(Track::Key(static_cast<float>(k[0].get<double>()), static_cast<float>(k[1].get<double>())));
        if (!Refuse(ValidateFullTrackReplacement(e, track, keys), at, stop)) return false;
        ApplyFullTrackReplacement(e, track, keys);
    }
    else if (name == "setTrackKey")
    {
        const float time = static_cast<float>(op["time"].get<double>());
        const float newTime = op.contains("newTime") ? static_cast<float>(op["newTime"].get<double>()) : time;
        const TrackKeyPlan plan = PlanSetTrackKey(t, time, newTime, static_cast<float>(op["value"].get<double>()), IsColorTrack(track));
        if (!Refuse(plan, at, stop)) return false;
        if (plan.outcome == EditOutcome::NoOpNotFound)
            return Fail(stop, kExitRule, "rule", TrackPath(track) + " has no key at time " + json(static_cast<double>(time)).dump(), at);
        if (plan.outcome == EditOutcome::AppliedBorderTime && newTime != time)
            return Fail(stop, kExitRule, "rule", "the first and last keys keep their times; only their values can change", at);
        ApplySetTrackKey(t, plan);
    }
    else // addTrackKey
    {
        const float time = static_cast<float>(op["time"].get<double>()), value = static_cast<float>(op["value"].get<double>());
        const ProposedKey key = {true, time, value};
        if (!Refuse(ValidateAddTrackKey(key, IsColorTrack(track)), at, stop)) return false;
        if (!Refuse(ValidateKeyCount(t, 1), at, stop)) return false;
        const float at_time = DedupedInsertTime(t, time);
        if (at_time != time)
            return Fail(stop, kExitRule, "rule", TrackPath(track) + " already has a key at time " + json(static_cast<double>(time)).dump() +
                        "; the editor would move the new key to " + json(static_cast<double>(at_time)).dump(), at);
        t.keys.insert(Track::Key(time, value));
    }
    for (const std::string& p : paths) { Add(requested, p); Add(footprint, p); }
    return true;
}

// ---- Staging -----------------------------------------------------------------

// Creates a unique sibling of `out` holding `bytes`, flushed. "" on failure.
std::wstring Stage(const std::wstring& out, const std::string& bytes)
{
    LARGE_INTEGER counter = {};
    for (int attempt = 0; attempt < 16; attempt++)
    {
        QueryPerformanceCounter(&counter);
        const std::wstring path = out + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                                  std::to_wstring((counter.QuadPart ^ (attempt * 2654435761ull)) & 0xFFFFFF) + L".tmp";
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            return L"";
        }
        DWORD written = 0;
        const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) { DeleteFileW(path.c_str()); return L""; }
        return path;
    }
    return L"";
}

// The emitter positions from `pos` up to its root.
std::vector<json> Ancestry(const json& dump, size_t pos)
{
    std::vector<json> chain;
    json at = pos;
    while (at.is_number() && at.get<size_t>() < dump["emitters"].size() && chain.size() <= dump["emitters"].size())
    {
        chain.push_back(at);
        at = dump["emitters"][at.get<size_t>()]["parent"];
    }
    return chain;
}

// The atlas index keys of `e` outside its grid, by time and value.
std::set<std::pair<float, float>> OutOfGrid(const Emitter& e)
{
    std::set<std::pair<float, float>> keys;
    for (const Track::Key& key : e.tracks[ParticleSystem::TRACK_INDEX]->keys)
        if (ClassifyAtlasIndex(e, key.value).outcome == EditOutcome::OutOfGrid) keys.insert(std::make_pair(key.time, key.value));
    return keys;
}

// Steps 1-6 of §3.5. Fills `report` as it goes; returns false with `stop`.
bool Patch(const std::string& source, const std::string& sourceHash, const std::string& requestBytes,
           const std::wstring& inputPath, const std::wstring& out, const PatchHooks& hooks, json& report, Stop* stop)
{
    // The request.
    json request;
    std::string reason;
    if (!tool_selector::ParseStrictJson(requestBytes, &request, &reason))
        return Fail(stop, kExitUsage, "request-malformed", reason);
    if (!CheckRequest(request, stop)) return false;

    // 1. Parse twice: S stays pristine, W is edited.
    std::unique_ptr<ParticleSystem> S = ParseSource(source, &reason);
    if (!S) return Fail(stop, kExitSource, "source-invalid", "the source did not load: " + reason);
    if (!S->getGraphRepairs().empty())
        return Fail(stop, kExitSource, "source-invalid", "the loader repaired the source: " + S->getGraphRepairs()[0]);
    json dumpS = particle_json::ParticleSystemJson(*S);
    std::string path;
    if (particle_json::FindInvalidInspectValue(dumpS, &path, &reason))
        return Fail(stop, kExitSource, "source-invalid", path + " " + reason);
    dumpS["sourceDocumentHash"] = sourceHash;
    std::unique_ptr<ParticleSystem> W = ParseSource(source, &reason);
    if (!W) return Fail(stop, kExitInternal, "internal", "the second parse failed: " + reason);

    if (S->getName() != DeriveParticleSystemName(inputPath))
        report["warnings"].push_back({{"code", "internal-name"},
            {"message", "the internal name '" + S->getName() + "' differs from '" + DeriveParticleSystemName(inputPath) +
                        "', the name the input file name gives; the candidate keeps the internal name"}});

    // 2. Resolve, and refuse a linked target.
    const tool_selector::Resolution resolved = tool_selector::ResolveSelector(request["selector"], dumpS);
    report["selector"] = {{"candidates", resolved.candidates}};
    if (resolved.refusal != tool_selector::Refusal::None)
        return Fail(stop, kExitSelector, tool_selector::RefusalName(resolved.refusal), resolved.reason);
    const size_t pos = resolved.emitter;
    const std::string prefix = "emitters[" + std::to_string(pos) + "].";
    report["selector"]["emitter"] = pos;
    report["selector"]["name"] = dumpS["emitters"][pos]["name"];
    Emitter& target = W->getEmitter(pos);
    if (HasLinkGroupMembership(target))
        return Fail(stop, kExitRule, "linked-target", "the target is in link group " + std::to_string(target.linkGroup) +
                    "; the tool does not edit linked emitters");

    // 3. Apply, op by op, against W's current state.
    std::vector<std::string> requested, footprint;
    const std::set<std::pair<float, float>> outOfGridBefore = OutOfGrid(S->getEmitter(pos));
    for (size_t i = 0; i < request["mutations"].size(); i++)
    {
        const json& op = request["mutations"][i];
        const json before = particle_json::EmitterJson(target, nullptr);
        Touched touched;
        if (!ApplyOp(*W, target, op, static_cast<int>(i), &touched, stop)) return false;
        const json after = particle_json::EmitterJson(target, nullptr);
        json fields = json::array();
        bool changed = false;
        for (const std::string& p : touched.requested)
        {
            fields.push_back({{"path", prefix + p}, {"before", *At(before, p)}, {"requested", *At(after, p)}});
            changed = changed || *At(before, p) != *At(after, p);
        }
        json fullFootprint = json::array();
        for (const std::string& p : touched.footprint) { fullFootprint.push_back(prefix + p); Add(footprint, prefix + p); }
        for (const std::string& p : touched.requested) Add(requested, prefix + p);
        report["ops"].push_back({{"index", i}, {"op", op["op"]}, {"effect", changed ? "changed" : "unchanged"},
                                 {"fields", fields}, {"footprint", fullFootprint}});
    }
    report["footprint"] = footprint;

    // What a save will make of W, before anything is written.
    json dumpW = particle_json::ParticleSystemJson(*W);
    if (particle_json::FindInvalidInspectValue(dumpW, &path, &reason))
        return Fail(stop, kExitRule, "non-finite-result", "the edits leave " + path + " " + reason);
    json projected;
    if (!particle_json::ProjectPersisted(dumpW, &projected, &reason))
        return Fail(stop, kExitRule, "unsaveable", "the edited document would not reload: " + reason);
    for (const std::string& p : requested)
    {
        const json* was = At(dumpS, p);
        const json* asked = At(dumpW, p);
        const json* saved = At(projected, p);
        if (was && asked && saved && *asked != *was && *saved == *was)
            return Fail(stop, kExitRule, "value-lost", p + ": the requested " + asked->dump() + " saves as " + saved->dump() +
                        ", the value it already has");
    }
    std::unique_ptr<ParticleSystem> B;
    {
        const std::string baseline = SaveToBytes(*S);
        B = ParseSource(baseline, &reason);
        if (!B) return Fail(stop, kExitInternal, "internal", "the unedited source does not reload: " + reason);
    }
    json dumpB = particle_json::ParticleSystemJson(*B);
    {
        json p = projected, b = dumpB;
        for (const std::string& f : footprint) { Blank(p, f); Blank(b, f); }
        if (FirstDifference(p, b, "", &path))
            return Fail(stop, kExitRule, "collateral", "saving the edit would also change " + path + ", outside the edit's footprint");
    }
    const std::set<std::pair<float, float>> outOfGridAfter = OutOfGrid(target);
    for (const auto& key : outOfGridAfter)
        if (!outOfGridBefore.count(key))
            report["warnings"].push_back({{"code", "atlas-out-of-grid"},
                {"message", prefix + "tracks[5] has index " + json(static_cast<double>(key.second)).dump() + " at time " +
                            json(static_cast<double>(key.first)).dump() + ", outside the atlas grid; the engine wraps it"}});

    // 4. Stage the candidate.
    const std::string bytesW = SaveToBytes(*W);
    const std::wstring staged = Stage(out, bytesW);
    if (staged.empty()) return Fail(stop, kExitIo, "io", "the candidate could not be staged next to --out");
    struct StagedFile
    {
        std::wstring path;
        bool keep = false;
        ~StagedFile() { if (!keep) DeleteFileW(path.c_str()); }
    } guard{staged};
    if (hooks.afterStage) hooks.afterStage(staged);

    // 5. Verify the staged file.
    std::string stagedBytes;
    if (!ReadFileBytes(staged, &stagedBytes, &reason)) return Fail(stop, kExitIo, "io", "the staged candidate could not be read: " + reason);
    report["checks"] = json::object();
    auto postcheck = [&](int n, bool ok, const std::string& why) {
        report["checks"][std::to_string(n)] = ok;
        if (!ok) Fail(stop, kExitPostcheck, "postcondition", "check " + std::to_string(n) + ": " + why);
        return ok;
    };
    if (!postcheck(4, Sha256Hex(stagedBytes) == Sha256Hex(bytesW), "the staged file is not the document that was verified"))
        return false;
    std::unique_ptr<ParticleSystem> C = ParseSource(stagedBytes, &reason);
    if (!postcheck(3, C && SaveToBytes(*C) == stagedBytes,
                   C ? "a save of the reloaded candidate differs from it" : "the candidate does not load: " + reason))
        return false;
    json dumpC = particle_json::ParticleSystemJson(*C);
    {
        std::string why;
        if (!C->getGraphRepairs().empty()) why = "the loader repaired the candidate: " + C->getGraphRepairs()[0];
        else if (particle_json::FindInvalidInspectValue(dumpC, &path, &reason)) why = path + " " + reason;
        else if (dumpC["emitters"].size() != dumpW["emitters"].size()) why = "the emitter count changed";
        else if (dumpC["emitters"][pos]["name"] != dumpW["emitters"][pos]["name"]) why = "the target's name did not persist";
        else if (dumpC["emitters"][pos]["linkGroup"] != 0) why = "the target is in a link group";
        else if (Ancestry(dumpC, pos) != Ancestry(dumpS, pos)) why = "the target's place in the tree changed";
        if (!postcheck(5, why.empty(), why)) return false;
    }
    for (const std::string& f : footprint)
    {
        const json* got = At(dumpC, f);
        const json* want = At(projected, f);
        if (!got || !want || *got != *want)
            return postcheck(1, false, f + " saved as " + Shown(got) + ", projected " + Shown(want));
    }
    postcheck(1, true, "");
    {
        json c = dumpC, b = dumpB;
        for (const std::string& f : footprint) { Blank(c, f); Blank(b, f); }
        if (!postcheck(2, !FirstDifference(c, b, "", &path), "the candidate also changed " + path)) return false;
    }
    for (json& op : report["ops"])
        for (json& field : op["fields"])
            field["persisted"] = *At(dumpC, field["path"].get<std::string>());

    // 6. Commit.
    if (!MoveFileExW(staged.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Fail(stop, kExitIo, "io", "the candidate could not replace --out (error " + std::to_string(GetLastError()) + ")");
    guard.keep = true;
    std::string written;
    if (!ReadFileBytes(out, &written, &reason)) return Fail(stop, kExitIo, "io", "--out could not be read back: " + reason);
    report["candidateHash"] = Sha256Hex(written);
    return true;
}
} // namespace

int RunPatch(const std::string& source, const std::string& sourceHash, const std::string& request,
             const std::wstring& inputPath, const std::wstring& out, const PatchHooks& hooks, json* report)
{
    json& r = *report;
    r = {{"schema", 1}, {"verb", "patch"}, {"sourceDocumentHash", sourceHash}, {"textureResolution", "unverified"},
         {"warnings", json::array()}, {"ops", json::array()}};
    Stop stop;
    const bool ok = Patch(source, sourceHash, request, inputPath, out, hooks, r, &stop);
    r["outcome"] = ok ? "ok" : (stop.code == kExitPostcheck ? "postcondition-failed" : "refused");
    r["exitCode"] = ok ? kExitOk : stop.code;
    if (!ok)
    {
        r["refusal"] = {{"category", stop.category}, {"reason", stop.reason}};
        r["refusal"]["op"] = stop.op >= 0 ? json(stop.op) : json(nullptr);
    }
    return ok ? kExitOk : stop.code;
}
} // namespace host
