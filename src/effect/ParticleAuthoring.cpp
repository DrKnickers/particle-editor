#include "ParticleAuthoring.h"

#include <cassert>
#include <cfloat>
#include <climits>
#include <cstdio>

#include "common/ResourceLimits.h"
#include "EmitterNaming.h"

#include <functional>

using namespace std;

typedef ParticleSystem::Emitter        Emitter;
typedef ParticleSystem::Emitter::Track Track;

static EditResult Result(EditOutcome outcome, const char* reason = "")
{
    EditResult r;
    r.outcome = outcome;
    r.reason  = reason;
    return r;
}

// Fill the shared EditPlan fields of a derived plan.
template <typename Plan> static Plan& Decide(Plan& plan, EditOutcome outcome, bool willMutate, const char* reason = "")
{
    plan.outcome    = outcome;
    plan.reason     = reason;
    plan.willMutate = willMutate;
    return plan;
}

// ---- Track keys -------------------------------------------------------------

bool IsBorderKeyTime(const Track& track, float time)
{
    if (track.keys.empty()) return false;
    return time == track.keys.begin()->time || time == track.keys.rbegin()->time;
}

// A NaN time breaks the multiset's ordering: find(NaN) matches the FIRST key,
// so a NaN delete would erase a border key. The bridge cannot send NaN (JSON
// has none), so refusing it changes nothing there and protects the tool path.
static const char* kNaNKeyReason = "track key time/value must not be NaN";

TrackKeyPlan PlanSetTrackKey(const Track& track, float oldTime, float newTime, float newValue, bool isColor)
{
    TrackKeyPlan plan;
    plan.findTime    = oldTime;
    plan.replacement = Track::Key(newTime, newValue);
    if (std::isnan(oldTime) || std::isnan(newTime) || std::isnan(newValue))
        return Decide(plan, EditOutcome::Rejected, false, kNaNKeyReason);
    if (track.keys.empty()) return Decide(plan, EditOutcome::NoOpNotFound, false);

    // Border keys: time fixed, value still moves.
    const bool isBorder = IsBorderKeyTime(track, oldTime);
    if (isBorder) plan.replacement.time = oldTime;

    // Checked before the lookup, so a bad request fails loud even when no
    // key sits at oldTime.
    if (!isBorder && !InTrackTimeRange(newTime))
        return Decide(plan, EditOutcome::Rejected, false, "set-track-key 'newTime' must be within [0, 100]");
    if (!std::isfinite(newValue))
        return Decide(plan, EditOutcome::Rejected, false, "set-track-key 'newValue' must be finite");
    if (isColor && (newValue < 0.0f || newValue > 1.0f))
        return Decide(plan, EditOutcome::Rejected, false,
                      "set-track-key 'newValue' must be within [0, 1] on a color track");

    if (track.keys.find(Track::Key(oldTime, 0.0f)) == track.keys.end())
        return Decide(plan, EditOutcome::NoOpNotFound, false);

    return Decide(plan, isBorder ? EditOutcome::AppliedBorderTime : EditOutcome::Applied, true);
}

void ApplySetTrackKey(Track& track, const TrackKeyPlan& plan)
{
    if (!plan.willMutate) return;
    // Erase + insert is the ordered-key idiom: a key's time is its ordering
    // key, so it cannot be changed in place.
    Track::KeyMap::iterator it = track.keys.find(Track::Key(plan.findTime, 0.0f));
    assert(it != track.keys.end()); // the plan was made on a different track state
    if (it == track.keys.end()) return;
    track.keys.erase(it);
    track.keys.insert(plan.replacement);
}

TrackKeyPlan PlanDeleteTrackKey(const Track& track, float time)
{
    TrackKeyPlan plan;
    plan.findTime    = time;
    plan.replacement = Track::Key(time, 0.0f);
    if (std::isnan(time)) return Decide(plan, EditOutcome::Rejected, false, kNaNKeyReason);
    if (track.keys.empty()) return Decide(plan, EditOutcome::NoOpNotFound, false);
    if (IsBorderKeyTime(track, time)) return Decide(plan, EditOutcome::NoOpBorderSkip, false);
    if (track.keys.find(Track::Key(time, 0.0f)) == track.keys.end())
        return Decide(plan, EditOutcome::NoOpNotFound, false);
    return Decide(plan, EditOutcome::Applied, true);
}

void ApplyDeleteTrackKey(Track& track, const TrackKeyPlan& plan)
{
    if (!plan.willMutate) return;
    Track::KeyMap::iterator it = track.keys.find(Track::Key(plan.findTime, 0.0f));
    assert(it != track.keys.end()); // the plan was made on a different track state
    if (it == track.keys.end()) return;
    track.keys.erase(it);
}

// Step `time` by `step` while a key occupies it. Bounded by 1000 iterations
// so a pathological dataset can't lock the dispatch thread. Kept verbatim
// from the add-track-key handler: the cap, the literal and the post-decrement
// decide which time a long collision run ends on.
static float BumpWhileOccupied(const Track& track, float time, float step)
{
    Track::Key probe(time, 0.0f);
    int        safety = 1000;
    while (track.keys.find(probe) != track.keys.end() && safety-- > 0)
    {
        time += step;
        probe = Track::Key(time, 0.0f);
    }
    return time;
}

float DedupedInsertTime(const Track& track, float desiredTime)
{
    const float up = BumpWhileOccupied(track, desiredTime, 0.001f);
    // An add at an occupied 100 would bump past the track's end and become a
    // new last key (which the save re-times to 100); search downward instead.
    if (up > 100.0f && desiredTime <= 100.0f) return BumpWhileOccupied(track, desiredTime, -0.001f);
    return up;
}

EditResult ValidateAddTrackKey(const ProposedKey& key, bool isColor)
{
    // A missing field defaulting to 0 would silently insert a wrong key
    // (a typo'd param name in a --record clip records as success).
    if (!key.numeric) return Result(EditOutcome::Rejected, "add-track-key requires numeric 'time' and 'value'");
    if (!std::isfinite(key.time) || !std::isfinite(key.value))
        return Result(EditOutcome::Rejected, "add-track-key 'time'/'value' must be finite");
    if (!InTrackTimeRange(key.time))
        return Result(EditOutcome::Rejected, "add-track-key 'time' must be within [0, 100]");
    if (isColor && (key.value < 0.0f || key.value > 1.0f))
        return Result(EditOutcome::Rejected, "add-track-key 'value' must be within [0, 1] on a color track");
    return Result(EditOutcome::Applied);
}

EditResult ValidateBatchKeys(const vector<ProposedKey>& keys, bool isColor)
{
    // Rejecting mid-insert would leave half a paste in the document, so the
    // whole batch is checked first, in order: the first bad key decides.
    for (size_t i = 0; i < keys.size(); i++)
    {
        if (!keys[i].numeric)
            return Result(EditOutcome::Rejected, "add-track-keys requires numeric 'time' and 'value' on every key");
        if (!std::isfinite(keys[i].time) || !std::isfinite(keys[i].value))
            return Result(EditOutcome::Rejected, "add-track-keys 'time'/'value' must be finite");
        if (!InTrackTimeRange(keys[i].time))
            return Result(EditOutcome::Rejected, "add-track-keys 'time' must be within [0, 100] on every key");
        if (isColor && (keys[i].value < 0.0f || keys[i].value > 1.0f))
            return Result(EditOutcome::Rejected, "add-track-keys 'value' must be within [0, 1] on a color track");
    }
    return Result(EditOutcome::Applied);
}

// ---- Track lock -------------------------------------------------------------

TrackLockPlan PlanSetTrackLock(const Emitter& emitter, int channelIdx, int lockToIdx, bool unlock)
{
    TrackLockPlan plan;
    plan.channel = channelIdx;
    plan.source  = channelIdx;
    if (channelIdx < 0 || channelIdx >= ParticleSystem::NUM_TRACKS)
        return Decide(plan, EditOutcome::Rejected, false, "unknown channel");

    // Only RGBA take part; the other tracks accept the request and ignore it.
    if (channelIdx >= 4) return Decide(plan, EditOutcome::NoOpOutOfDomain, false);

    if (!unlock)
    {
        // A lock target must be a color channel EARLIER than this one. An
        // unresolved or later target is refused rather than read as an
        // unlock, so a typo'd --record step cannot record as success.
        if (lockToIdx >= 0 && lockToIdx < 4 && lockToIdx < channelIdx)
            plan.source = lockToIdx;
        else
            return Decide(plan, EditOutcome::Rejected, false,
                          "invalid lockTo channel (must be an earlier RGBA channel)");
    }

    if (emitter.tracks[channelIdx] == &emitter.trackContents[plan.source])
        return Decide(plan, EditOutcome::NoOpUnchanged, false);
    return Decide(plan, EditOutcome::Applied, true);
}

void ApplySetTrackLock(Emitter& emitter, const TrackLockPlan& plan)
{
    if (!plan.willMutate) return;
    // Lock state is pointer identity: tracks[i] == &trackContents[j] (i != j)
    // shows channel j's keys. Unlocking restores channel i's own keys, which
    // the lock never touched.
    emitter.tracks[plan.channel] = &emitter.trackContents[plan.source];
}

// ---- Interpolation ----------------------------------------------------------

bool ParseInterpolation(const string& name, Track::InterpolationType& out)
{
    if (name == "linear")
        out = Track::IT_LINEAR;
    else if (name == "smooth")
        out = Track::IT_SMOOTH;
    else if (name == "step")
        out = Track::IT_STEP;
    else
        return false;
    return true;
}

bool IsValidInterpolation(const string& name)
{
    Track::InterpolationType unused;
    return ParseInterpolation(name, unused);
}

InterpolationPlan PlanSetTrackInterpolation(const Track& track, const string& name)
{
    InterpolationPlan plan;
    plan.next = track.interpolation;
    if (!ParseInterpolation(name, plan.next))
        return Decide(plan, EditOutcome::Rejected, false, "unknown interpolation");
    if (track.interpolation == plan.next) return Decide(plan, EditOutcome::NoOpUnchanged, false);
    return Decide(plan, EditOutcome::Applied, true);
}

void ApplySetTrackInterpolation(Track& track, const InterpolationPlan& plan)
{
    if (!plan.willMutate) return;
    track.interpolation = plan.next;
}

// ---- Scalar fields ----------------------------------------------------------

bool IsFiniteFloatField(double value)
{
    return std::isfinite(value) && std::fabs(value) <= FLT_MAX;
}

bool InIntFieldRange(double value)
{
    return value >= INT_MIN && value <= INT_MAX;
}

// ---- Full-track replacement -------------------------------------------------

EditResult ValidateFullTrackReplacement(const Emitter& emitter, int trackIdx, const vector<Track::Key>& proposed)
{
    if (trackIdx < 0 || trackIdx >= ParticleSystem::NUM_TRACKS) return Result(EditOutcome::Rejected, "unknown track");
    if (emitter.tracks[trackIdx] == NULL) return Result(EditOutcome::Rejected, "no track slot bound for this channel");

    // A locked color channel displays an earlier channel's keys; replacing
    // "its" keys would rewrite that other channel.
    if (trackIdx < 4 && emitter.tracks[trackIdx] != &emitter.trackContents[trackIdx])
        return Result(EditOutcome::Rejected,
                      "track is locked to an earlier channel; unlock it before replacing its keys");

    // With randomRotation on, the save writes the rotation track from
    // randomRotationAverage and drops its keys.
    if (trackIdx == ParticleSystem::TRACK_ROTATION_SPEED && emitter.randomRotation)
        return Result(EditOutcome::Rejected, "rotationSpeed keys are not saved while randomRotation is on");

    // The file stores the two endpoint values and the keys between them.
    if (proposed.size() < 2)
        return Result(EditOutcome::Rejected, "a track needs at least 2 keys (the endpoints at 0 and 100)");
    // readTracks admits an intermediate key only while the track holds fewer
    // than kMaxAloTrackKeys keys, the first endpoint included.
    if (proposed.size() - 2 >= kMaxAloTrackKeys) return Result(EditOutcome::Rejected, "too many keys for one track");

    for (size_t i = 0; i < proposed.size(); i++)
    {
        if (!std::isfinite(proposed[i].time) || !std::isfinite(proposed[i].value))
            return Result(EditOutcome::Rejected, "key time and value must be finite");
    }

    // The endpoints reload at exactly 0 and 100 whatever their saved time.
    if (proposed.front().time != 0.0f) return Result(EditOutcome::Rejected, "the first key must be at time 0");
    if (proposed.back().time != 100.0f) return Result(EditOutcome::Rejected, "the last key must be at time 100");

    // readTracks refuses a key earlier than the one before it or past 100.
    for (size_t i = 1; i < proposed.size(); i++)
    {
        if (proposed[i].time < proposed[i - 1].time || proposed[i].time > 100.0f)
            return Result(EditOutcome::Rejected, "key times must be non-decreasing within [0, 100]");
    }

    // Color values are stored as bytes; readTracks refuses one outside [0, 1].
    if (trackIdx < 4)
    {
        for (size_t i = 0; i < proposed.size(); i++)
        {
            if (proposed[i].value < 0.0f || proposed[i].value > 1.0f)
                return Result(EditOutcome::Rejected, "color and alpha key values must be within [0, 1]");
        }
    }

    return Result(EditOutcome::Applied);
}

void ApplyFullTrackReplacement(Emitter& emitter, int trackIdx, const vector<Track::Key>& proposed)
{
    // A multiset insert places a key after any equal-time keys, so inserting
    // in order keeps the proposed order of same-time keys.
    Track& track = *emitter.tracks[trackIdx];
    track.keys.clear();
    for (size_t i = 0; i < proposed.size(); i++) track.keys.insert(proposed[i]);
}

// ---- Atlas ------------------------------------------------------------------

EditResult ClassifyAtlasIndex(const Emitter& emitter, float indexValue)
{
    const int    side  = AtlasGridSide(emitter.textureSize);
    const double cells = (double)side * side;
    const double frame = std::floor((double)indexValue);
    if (std::isfinite(indexValue) && frame >= 0.0 && frame < cells) return Result(EditOutcome::Applied);

    char reason[160];
    snprintf(reason, sizeof(reason), "atlas index %g is outside the %d x %d grid; the engine wraps it to another frame",
             (double)indexValue, side, side);
    return Result(EditOutcome::OutOfGrid, reason);
}

// ---- Tool-only rules --------------------------------------------------------

namespace
{
enum class PropertyKind
{
    Bool,
    Float,
    ULong,
    UInt,
    Int
};

struct PropertyRule
{
    std::string                    name;
    PropertyKind                   kind;
    bool                           ranged; // floats: whether [lo, hi] applies; integers always
    double                         lo, hi;
    std::function<void*(Emitter&)> ref;
};

const double kU32Max = 4294967295.0;

std::vector<PropertyRule> BuildPropertyRules()
{
    std::vector<PropertyRule> rules;
    auto                      boolean = [&](const char* name, bool Emitter::* m)
    {
        rules.push_back({name, PropertyKind::Bool, false, 0, 0, [m](Emitter& e) -> void* { return &(e.*m); }});
    };
    auto real = [&](const std::string& name, std::function<float*(Emitter&)> f, bool ranged = false, double lo = 0,
                    double hi = 0)
    {
        rules.push_back({name, PropertyKind::Float, ranged, lo, hi, [f](Emitter& e) -> void* { return f(e); }});
    };
    auto ulong = [&](const char* name, unsigned long Emitter::* m, double lo, double hi)
    {
        rules.push_back({name, PropertyKind::ULong, true, lo, hi, [m](Emitter& e) -> void* { return &(e.*m); }});
    };
    auto member = [](float Emitter::* m)
    {
        return [m](Emitter& e)
        {
            return &(e.*m);
        };
    };

    boolean("linkToSystem", &Emitter::linkToSystem);
    boolean("objectSpaceAcceleration", &Emitter::objectSpaceAcceleration);
    boolean("doColorAddGrayscale", &Emitter::doColorAddGrayscale);
    boolean("affectedByWind", &Emitter::affectedByWind);
    boolean("isHeatParticle", &Emitter::isHeatParticle);
    boolean("isWeatherParticle", &Emitter::isWeatherParticle);
    boolean("hasTail", &Emitter::hasTail);
    boolean("noDepthTest", &Emitter::noDepthTest);
    boolean("randomRotation", &Emitter::randomRotation);
    boolean("randomRotationDirection", &Emitter::randomRotationDirection);
    boolean("isWorldOriented", &Emitter::isWorldOriented);
    boolean("useBursts", &Emitter::useBursts);
    boolean("unknown15", &Emitter::unknown15);
    boolean("unknown2b", &Emitter::unknown2b);
    boolean("unknown44", &Emitter::unknown44);

    real("gravity", member(&Emitter::gravity));
    real("lifetime", member(&Emitter::lifetime));
    real("initialDelay", member(&Emitter::initialDelay));
    real("burstDelay", member(&Emitter::burstDelay));
    real("inwardSpeed", member(&Emitter::inwardSpeed));
    real("inwardAcceleration", member(&Emitter::inwardAcceleration));
    real("randomScalePerc", member(&Emitter::randomScalePerc));
    real("randomLifetimePerc", member(&Emitter::randomLifetimePerc));
    real("weatherCubeSize", member(&Emitter::weatherCubeSize));
    real("tailSize", member(&Emitter::tailSize));
    real("parentLinkStrength", member(&Emitter::parentLinkStrength));
    real("weatherCubeDistance", member(&Emitter::weatherCubeDistance));
    real("randomRotationAverage", member(&Emitter::randomRotationAverage));
    real("randomRotationVariance", member(&Emitter::randomRotationVariance), true, 0.0, 1.0);
    real("bounciness", member(&Emitter::bounciness));
    real("freezeTime", member(&Emitter::freezeTime));
    real("skipTime", member(&Emitter::skipTime));
    real("emitFromMeshOffset", member(&Emitter::emitFromMeshOffset));
    real("weatherFadeoutDistance", member(&Emitter::weatherFadeoutDistance));
    real("unknown3f", member(&Emitter::unknown3f));
    for (int i = 0; i < 3; i++)
        real("acceleration[" + std::to_string(i) + "]", [i](Emitter& e) { return &e.acceleration[i]; });
    for (int i = 0; i < 4; i++)
        real("randomColors[" + std::to_string(i) + "]", [i](Emitter& e) { return &e.randomColors[i]; });

    ulong("nBursts", &Emitter::nBursts, 0, kU32Max - 1); // 0xFFFFFFFF reloads as 0
    ulong("blendMode", &Emitter::blendMode, 0, 13);
    ulong("textureSize", &Emitter::textureSize, 0, kU32Max);
    ulong("nParticlesPerSecond", &Emitter::nParticlesPerSecond, 0, kU32Max);
    ulong("nTriangles", &Emitter::nTriangles, 1, kU32Max);
    ulong("nParticlesPerBurst", &Emitter::nParticlesPerBurst, 0, kU32Max);
    ulong("groundBehavior", &Emitter::groundBehavior, ParticleSystem::GROUND_NONE, ParticleSystem::GROUND_STICK);
    ulong("unknown06", &Emitter::unknown06, 0, kU32Max);
    ulong("unknown49", &Emitter::unknown49, 0, kU32Max);
    rules.push_back({"emitFromMesh", PropertyKind::Int, true, ParticleSystem::EMIT_DISABLE,
                     ParticleSystem::EMIT_EVERY_VERTEX, [](Emitter& e) -> void* { return &e.emitFromMesh; }});

    // The speed and position groups. The lifetime group is derived on save.
    for (int g : {ParticleSystem::GROUP_SPEED, ParticleSystem::GROUP_POSITION})
    {
        const std::string prefix     = "groups[" + std::to_string(g) + "].";
        auto              groupFloat = [&](const char* name, float Emitter::Group::* m)
        {
            real(prefix + name, [g, m](Emitter& e) { return &(e.groups[g].*m); });
        };
        auto groupUInt = [&](const char* name, unsigned int Emitter::Group::* m, double hi)
        {
            rules.push_back({prefix + name, PropertyKind::UInt, true, 0, hi,
                             [g, m](Emitter& e) -> void* { return &(e.groups[g].*m); }});
        };
        groupUInt("type", &Emitter::Group::type, ParticleSystem::NUM_GROUP_TYPES - 1);
        groupFloat("minX", &Emitter::Group::minX);
        groupFloat("minY", &Emitter::Group::minY);
        groupFloat("minZ", &Emitter::Group::minZ);
        groupFloat("maxX", &Emitter::Group::maxX);
        groupFloat("maxY", &Emitter::Group::maxY);
        groupFloat("maxZ", &Emitter::Group::maxZ);
        groupFloat("sideLength", &Emitter::Group::sideLength);
        groupFloat("sphereRadius", &Emitter::Group::sphereRadius);
        groupUInt("sphereEdge", &Emitter::Group::sphereEdge, kU32Max);
        groupFloat("cylinderRadius", &Emitter::Group::cylinderRadius);
        groupUInt("cylinderEdge", &Emitter::Group::cylinderEdge, kU32Max);
        groupFloat("cylinderHeight", &Emitter::Group::cylinderHeight);
        groupFloat("valX", &Emitter::Group::valX);
        groupFloat("valY", &Emitter::Group::valY);
        groupFloat("valZ", &Emitter::Group::valZ);
    }
    return rules;
}

const std::vector<PropertyRule>& PropertyRules()
{
    static const std::vector<PropertyRule> rules = BuildPropertyRules();
    return rules;
}

const PropertyRule* FindPropertyRule(const std::string& field)
{
    for (const PropertyRule& rule : PropertyRules())
        if (rule.name == field) return &rule;
    return nullptr;
}

EditResult Rejected(const std::string& reason)
{
    EditResult r;
    r.outcome = EditOutcome::Rejected;
    r.reason  = reason;
    return r;
}
} // namespace

std::vector<std::string> SettablePropertyNames()
{
    std::vector<std::string> names;
    for (const PropertyRule& rule : PropertyRules()) names.push_back(rule.name);
    return names;
}

EditResult ValidateSetProperty(const std::string& field, const PropertyValue& value)
{
    const PropertyRule* rule = FindPropertyRule(field);
    if (!rule) return Rejected("'" + field + "' is not a settable property");
    if (rule->kind == PropertyKind::Bool)
    {
        if (!value.isBool) return Rejected("'" + field + "' must be true or false");
        return Result(EditOutcome::Applied);
    }
    if (value.isBool) return Rejected("'" + field + "' must be a number");
    char range[96];
    if (rule->kind == PropertyKind::Float)
    {
        if (!IsFiniteFloatField(value.number))
            return Rejected("'" + field + "' must be a finite number that fits a float");
        if (rule->ranged && !(value.number >= rule->lo && value.number <= rule->hi))
        {
            snprintf(range, sizeof(range), "[%g, %g]", rule->lo, rule->hi);
            return Rejected("'" + field + "' must be within " + range);
        }
        return Result(EditOutcome::Applied);
    }
    if (!std::isfinite(value.number) || value.number != std::floor(value.number))
        return Rejected("'" + field + "' must be an integer");
    if (!(value.number >= rule->lo && value.number <= rule->hi))
    {
        snprintf(range, sizeof(range), "%.0f..%.0f", rule->lo, rule->hi);
        return Rejected("'" + field + "' must be within " + range);
    }
    return Result(EditOutcome::Applied);
}

void ApplySetProperty(Emitter& emitter, const std::string& field, const PropertyValue& value)
{
    const PropertyRule* rule = FindPropertyRule(field);
    assert(rule != nullptr);
    if (!rule) return;
    void* target = rule->ref(emitter);
    switch (rule->kind)
    {
        case PropertyKind::Bool:  *static_cast<bool*>(target) = value.boolValue; break;
        case PropertyKind::Float: *static_cast<float*>(target) = static_cast<float>(value.number); break;
        case PropertyKind::ULong:
            *static_cast<unsigned long*>(target) = static_cast<unsigned long>(value.number);
            break;
        case PropertyKind::UInt: *static_cast<unsigned int*>(target) = static_cast<unsigned int>(value.number); break;
        case PropertyKind::Int:  *static_cast<int*>(target) = static_cast<int>(value.number); break;
    }
}

PropertyValue GetProperty(const Emitter& emitter, const std::string& field)
{
    PropertyValue       value = {false, false, 0.0};
    const PropertyRule* rule  = FindPropertyRule(field);
    assert(rule != nullptr);
    if (!rule) return value;
    const void* source = rule->ref(const_cast<Emitter&>(emitter));
    switch (rule->kind)
    {
        case PropertyKind::Bool:
            value.isBool    = true;
            value.boolValue = *static_cast<const bool*>(source);
            break;
        case PropertyKind::Float: value.number = *static_cast<const float*>(source); break;
        case PropertyKind::ULong: value.number = *static_cast<const unsigned long*>(source); break;
        case PropertyKind::UInt:  value.number = *static_cast<const unsigned int*>(source); break;
        case PropertyKind::Int:   value.number = *static_cast<const int*>(source); break;
    }
    return value;
}

std::vector<std::string> PropertyFootprint(const std::string& field)
{
    std::vector<std::string> paths = {field};
    if (field == "lifetime" || field == "randomLifetimePerc") paths.push_back("groups[1]");
    if (field == "randomRotation")
    {
        paths.push_back("randomRotationAverage");
        paths.push_back("tracks[6]");
    }
    if (field == "randomRotationAverage") paths.push_back("tracks[6]");
    return paths;
}

EditResult ValidateRename(const ParticleSystem& system, const Emitter& target, const std::string& name)
{
    if (name.empty()) return Rejected("rename: the name must not be empty");
    if (name.find('\0') != std::string::npos) return Rejected("rename: the name must not contain a NUL");
    if (!IsValidUtf8(name)) return Rejected("rename: the name must be valid UTF-8");
    if (name == target.name) return Result(EditOutcome::NoOpUnchanged);
    std::vector<std::string> names;
    bool                     taken = false;
    for (const Emitter* e : system.getEmitters())
    {
        names.push_back(e->name);
        if (e != &target && e->name == name) taken = true;
    }
    if (taken)
        return Rejected("rename: another emitter is already named '" + name + "' (a free name is '" +
                        NextDuplicateName(names, name) + "')");
    return Result(EditOutcome::Applied);
}

EditResult ValidateTextureName(const std::string& name, bool colorTexture)
{
    const char* which = colorTexture ? "colorTexture" : "normalTexture";
    if (name.empty())
        return Rejected(colorTexture ? std::string(which) + " must not be empty"
                                     : std::string(which) + " must not be empty; an empty one reloads as "
                                                            "p_particle_depth_master.tga, so name that");
    if (name.find('\0') != std::string::npos) return Rejected(std::string(which) + " must not contain a NUL");
    if (!IsValidUtf8(name)) return Rejected(std::string(which) + " must be valid UTF-8");
    if (name.size() > kMaxTextureNameBytes)
        return Rejected(std::string(which) + " must be at most " + std::to_string(kMaxTextureNameBytes) + " bytes");
    return Result(EditOutcome::Applied);
}

EditResult ValidateRescale(const Emitter& emitter, double timeScale, double sizeScale)
{
    // Normal floats only: a subnormal scale's reciprocal overflows.
    if (!(IsFiniteFloatField(timeScale) && timeScale >= FLT_MIN))
        return Rejected("rescale: the time scale must be a finite number above 0");
    if (!(IsFiniteFloatField(sizeScale) && sizeScale >= FLT_MIN))
        return Rejected("rescale: the size scale must be a finite number above 0");
    const float t = static_cast<float>(timeScale);
    if (t == 1.0f || emitter.isWeatherParticle) return Result(EditOutcome::Applied);
    // Rescale.cpp converts these through an int, in float arithmetic.
    if (emitter.useBursts)
    {
        // A zero delay gives an infinite rate; a tiny one of either sign, a
        // rate past the int range.
        const float perSecond = 1.0f / (emitter.burstDelay * t) * 10000.0f + 0.5f;
        if (!(perSecond >= -2147483648.0f && perSecond < 2147483648.0f))
            return Rejected("rescale: the burst delay becomes too close to 0 to convert");
    }
    else
    {
        const float perSecond = emitter.nParticlesPerSecond / t;
        if (!(perSecond < 2147483648.0f)) return Rejected("rescale: the particle rate becomes too large to convert");
        // A fractional rate becomes bursts of one every 1/rate seconds.
        if (perSecond != std::floor(perSecond) && !std::isfinite(1.0f / perSecond))
            return Rejected("rescale: the particle rate becomes too small to convert");
    }
    return Result(EditOutcome::Applied);
}

EditResult ValidateKeyCount(const Track& track, size_t adding)
{
    // The file stores the two endpoints and the keys between them.
    const size_t total    = track.keys.size() + adding;
    const size_t interior = total >= 2 ? total - 2 : 0;
    if (interior >= kMaxAloTrackKeys)
        return Rejected("a track keeps fewer than " + std::to_string(kMaxAloTrackKeys) + " interior keys");
    return Result(EditOutcome::Applied);
}

std::vector<int> LockedDependents(const Emitter& emitter, int source)
{
    std::vector<int> dependents;
    if (source < 0 || source >= 4) return dependents;
    for (int j = 0; j < 4; j++)
        if (j != source && emitter.tracks[j] == &emitter.trackContents[source]) dependents.push_back(j);
    return dependents;
}

EditResult ValidateTrackEditLocks(const Emitter& emitter, int track, bool allowDependents)
{
    if (track < 0 || track >= ParticleSystem::NUM_TRACKS) return Rejected("no such track");
    if (track >= 4) return Result(EditOutcome::Applied);
    if (emitter.tracks[track] != &emitter.trackContents[track])
    {
        int source = track;
        for (int j = 0; j < 4; j++)
            if (emitter.tracks[track] == &emitter.trackContents[j]) source = j;
        return Rejected("tracks[" + std::to_string(track) + "] is locked to tracks[" + std::to_string(source) +
                        "]; edit tracks[" + std::to_string(source) + "]");
    }
    const std::vector<int> dependents = LockedDependents(emitter, track);
    if (!dependents.empty() && !allowDependents)
    {
        std::string list;
        for (int j : dependents) list += (list.empty() ? "" : ", ") + std::string("tracks[") + std::to_string(j) + "]";
        return Rejected("tracks[" + std::to_string(track) + "] also drives " + list +
                        ", which are locked to it; name them in the edit's footprint to change them too");
    }
    return Result(EditOutcome::Applied);
}
