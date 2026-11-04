// The mutation ops shared by `--tool patch` and `--tool build`. See ToolOps.h.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>
#include "ToolOps.h"
#include "ToolIO.h"
#include "effect/ParticleAuthoring.h"
#include "effect/ParticleSystem.h"
#include "effect/Rescale.h"

using nlohmann::json;
typedef ParticleSystem::Emitter        Emitter;
typedef ParticleSystem::Emitter::Track Track;

namespace host
{
namespace tool_ops
{
bool Fail(Stop* stop, int code, const std::string& category, const std::string& reason, int op)
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
    size_t      i   = 0;
    while (i < path.size())
    {
        if (path[i] == '.')
        {
            i++;
            continue;
        }
        if (path[i] == '[')
        {
            const size_t close = path.find(']', i);
            if (close == std::string::npos) return nullptr;
            const size_t index = std::stoul(path.substr(i + 1, close - i - 1));
            if (!cur->is_array() || index >= cur->size()) return nullptr;
            cur = &(*cur)[index];
            i   = close + 1;
            continue;
        }
        const size_t      end = path.find_first_of(".[", i);
        const std::string key = path.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (!cur->is_object() || !cur->contains(key)) return nullptr;
        cur = &(*cur)[key];
        i   = end == std::string::npos ? path.size() : end;
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
            if (!b.contains(it.key()))
            {
                *where = p;
                return true;
            }
            if (FirstDifference(it.value(), b[it.key()], p, where)) return true;
        }
        for (auto it = b.begin(); it != b.end(); ++it)
            if (!a.contains(it.key()))
            {
                *where = path.empty() ? it.key() : path + "." + it.key();
                return true;
            }
        return false;
    }
    if (a.is_array() && b.is_array())
    {
        if (a.size() != b.size())
        {
            *where = path;
            return true;
        }
        for (size_t i = 0; i < a.size(); i++)
            if (FirstDifference(a[i], b[i], path + "[" + std::to_string(i) + "]", where)) return true;
        return false;
    }
    if (a != b)
    {
        *where = path.empty() ? "(root)" : path;
        return true;
    }
    return false;
}

std::string Shown(const json* value)
{
    return value ? value->dump() : "(absent)";
}

void Add(std::vector<std::string>& to, const std::string& path)
{
    if (std::find(to.begin(), to.end(), path) == to.end()) to.push_back(path);
}

namespace
{
// ---- Request shape (exit 2) -------------------------------------------------

enum class Kind
{
    String,
    Number,
    Integer,
    Scalar,
    Keys,
    Dependents
};

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
        {"setProperty", {{"field", Kind::String, true}, {"value", Kind::Scalar, true}}},
        {"setBlendMode", {{"value", Kind::Number, true}}},
        {"setColorTexture", {{"name", Kind::String, true}}},
        {"setNormalTexture", {{"name", Kind::String, true}}},
        {"rename", {{"name", Kind::String, true}}},
        {"rescale", {{"timeScale", Kind::Number, true}, {"sizeScale", Kind::Number, true}}},
        {"setInterpolation",
         {{"track", Kind::Integer, true},
          {"interpolation", Kind::String, true},
          {"dependents", Kind::Dependents, false}}},
        {"replaceTrack",
         {{"track", Kind::Integer, true}, {"keys", Kind::Keys, true}, {"dependents", Kind::Dependents, false}}},
        {"setTrackKey",
         {{"track", Kind::Integer, true},
          {"time", Kind::Number, true},
          {"value", Kind::Number, true},
          {"newTime", Kind::Number, false},
          {"dependents", Kind::Dependents, false}}},
        {"addTrackKey",
         {{"track", Kind::Integer, true},
          {"time", Kind::Number, true},
          {"value", Kind::Number, true},
          {"dependents", Kind::Dependents, false}}},
        {"setTrackLock", {{"track", Kind::Integer, true}, {"lockTo", Kind::Integer, false}}},
    };
    return shapes;
}

bool IsIntegral(const json& v)
{
    return v.is_number_integer() || v.is_number_unsigned() ||
           (v.is_number_float() && v.get<double>() == std::floor(v.get<double>()) &&
            std::fabs(v.get<double>()) < 2147483648.0);
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

// ---- Ops ---------------------------------------------------------------------

std::string TrackPath(int track)
{
    return "tracks[" + std::to_string(track) + "]";
}

const std::vector<std::string>& RescalePaths()
{
    // Everything DoRescaleEmitter writes (Rescale.cpp), plus groups[1],
    // which a save derives from lifetime.
    static const std::vector<std::string> paths = {
        "skipTime",    "freezeTime",         "initialDelay", "lifetime",
        "groups[1]",   "burstDelay",         "useBursts",    "nParticlesPerSecond",
        "nBursts",     "nParticlesPerBurst", "groups[0]",    "groups[2]",
        "inwardSpeed", "inwardAcceleration", "acceleration", "gravity",
        "tailSize",    "tracks[4]"};
    return paths;
}

// A rule's verdict (an EditResult or a plan): refused when Rejected.
template <typename Verdict> bool Refuse(const Verdict& result, int op, Stop* stop)
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
            return Fail(
                stop, kExitRule, "rule",
                "dependents must list exactly the channels locked to " + TrackPath(track) + ": " + expected.dump(), at);
        }
    }
    if (track == ParticleSystem::TRACK_ROTATION_SPEED && e.randomRotation)
        return Fail(stop, kExitRule, "rule", "rotationSpeed keys are not saved while randomRotation is on", at);
    Add(*paths, TrackPath(track));
    for (int j : locked) Add(*paths, TrackPath(j));
    return true;
}
} // namespace

bool CheckOp(const json& op, int at, Stop* stop)
{
    if (!op.is_object() || !op.contains("op") || !op["op"].is_string())
        return Fail(stop, kExitUsage, "request-malformed", "each mutation must be an object with an \"op\" string", at);
    const std::string name  = op["op"].get<std::string>();
    const OpShape*    shape = nullptr;
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
            return Fail(stop, kExitUsage, "request-malformed",
                        name + "." + it.key() + " must be " + KindName(field->kind), at);
    }
    for (const Field& f : shape->fields)
        if (f.required && !op.contains(f.name))
            return Fail(stop, kExitUsage, "request-malformed", name + " needs '" + f.name + "'", at);
    return true;
}

bool ApplyOp(ParticleSystem& system, Emitter& e, const json& op, int at, Touched* touched, Stop* stop)
{
    const std::string         name      = op["op"].get<std::string>();
    std::vector<std::string>& requested = touched->requested;
    std::vector<std::string>& footprint = touched->footprint;

    if (name == "setProperty" || name == "setBlendMode")
    {
        const std::string field = name == "setBlendMode" ? "blendMode" : op["field"].get<std::string>();
        const json&       v     = op["value"];
        PropertyValue value = {v.is_boolean(), v.is_boolean() && v.get<bool>(), v.is_number() ? v.get<double>() : 0.0};
        if (!Refuse(ValidateSetProperty(field, value), at, stop)) return false;
        ApplySetProperty(e, field, value);
        Add(requested, field);
        for (const std::string& p : PropertyFootprint(field)) Add(footprint, p);
        return true;
    }
    if (name == "setColorTexture" || name == "setNormalTexture")
    {
        const bool        color   = name == "setColorTexture";
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
        for (const std::string& p : RescalePaths())
        {
            Add(requested, p);
            Add(footprint, p);
        }
        return true;
    }

    if (name == "setTrackLock")
    {
        // A colour channel shows an earlier channel's keys (lockTo), or its own.
        const int           track  = op["track"].get<int>();
        const bool          unlock = !op.contains("lockTo");
        const TrackLockPlan plan   = PlanSetTrackLock(e, track, unlock ? -1 : op["lockTo"].get<int>(), unlock);
        if (!Refuse(plan, at, stop)) return false;
        if (plan.outcome == EditOutcome::NoOpOutOfDomain)
            return Fail(stop, kExitRule, "rule", "only the colour channels, tracks[0] to tracks[3], lock", at);
        ApplySetTrackLock(e, plan);
        Add(requested, TrackPath(track));
        Add(footprint, TrackPath(track));
        return true;
    }

    // The track ops.
    const int                track = op["track"].get<int>();
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
        const float        time    = static_cast<float>(op["time"].get<double>());
        const float        newTime = op.contains("newTime") ? static_cast<float>(op["newTime"].get<double>()) : time;
        const TrackKeyPlan plan =
            PlanSetTrackKey(t, time, newTime, static_cast<float>(op["value"].get<double>()), IsColorTrack(track));
        if (!Refuse(plan, at, stop)) return false;
        if (plan.outcome == EditOutcome::NoOpNotFound)
            return Fail(stop, kExitRule, "rule",
                        TrackPath(track) + " has no key at time " + json(static_cast<double>(time)).dump(), at);
        if (plan.outcome == EditOutcome::AppliedBorderTime && newTime != time)
            return Fail(stop, kExitRule, "rule",
                        "the first and last keys keep their times; only their values can change", at);
        ApplySetTrackKey(t, plan);
    }
    else // addTrackKey
    {
        const float       time  = static_cast<float>(op["time"].get<double>()),
                          value = static_cast<float>(op["value"].get<double>());
        const ProposedKey key   = {true, time, value};
        if (!Refuse(ValidateAddTrackKey(key, IsColorTrack(track)), at, stop)) return false;
        if (!Refuse(ValidateKeyCount(t, 1), at, stop)) return false;
        const float at_time = DedupedInsertTime(t, time);
        if (at_time != time)
            return Fail(stop, kExitRule, "rule",
                        TrackPath(track) + " already has a key at time " + json(static_cast<double>(time)).dump() +
                            "; the editor would move the new key to " + json(static_cast<double>(at_time)).dump(),
                        at);
        t.keys.insert(Track::Key(time, value));
    }
    for (const std::string& p : paths)
    {
        Add(requested, p);
        Add(footprint, p);
    }
    return true;
}

// ---- Staging -----------------------------------------------------------------

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
        DWORD      written = 0;
        const bool ok      = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                             written == bytes.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok)
        {
            DeleteFileW(path.c_str());
            return L"";
        }
        return path;
    }
    return L"";
}

std::set<std::pair<float, float>> OutOfGrid(const Emitter& e)
{
    std::set<std::pair<float, float>> keys;
    for (const Track::Key& key : e.tracks[ParticleSystem::TRACK_INDEX]->keys)
        if (ClassifyAtlasIndex(e, key.value).outcome == EditOutcome::OutOfGrid)
            keys.insert(std::make_pair(key.time, key.value));
    return keys;
}
} // namespace tool_ops
} // namespace host
