#include "ParticleAuthoring.h"

#include <cassert>
#include <cfloat>
#include <climits>
#include <cstdio>

#include "common/ResourceLimits.h"

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
template <typename Plan>
static Plan& Decide(Plan& plan, EditOutcome outcome, bool willMutate, const char* reason = "")
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

TrackKeyPlan PlanSetTrackKey(const Track& track, float oldTime, float newTime, float newValue)
{
    TrackKeyPlan plan;
    plan.findTime    = oldTime;
    plan.replacement = Track::Key(newTime, newValue);
    if (std::isnan(oldTime) || std::isnan(newTime) || std::isnan(newValue))
        return Decide(plan, EditOutcome::Rejected, false, kNaNKeyReason);
    if (track.keys.empty())
        return Decide(plan, EditOutcome::NoOpNotFound, false);

    // Border keys: time fixed, value still moves.
    const bool isBorder = IsBorderKeyTime(track, oldTime);
    if (isBorder) plan.replacement.time = oldTime;

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
    assert(it != track.keys.end());   // the plan was made on a different track state
    if (it == track.keys.end()) return;
    track.keys.erase(it);
    track.keys.insert(plan.replacement);
}

TrackKeyPlan PlanDeleteTrackKey(const Track& track, float time)
{
    TrackKeyPlan plan;
    plan.findTime    = time;
    plan.replacement = Track::Key(time, 0.0f);
    if (std::isnan(time))
        return Decide(plan, EditOutcome::Rejected, false, kNaNKeyReason);
    if (track.keys.empty())
        return Decide(plan, EditOutcome::NoOpNotFound, false);
    if (IsBorderKeyTime(track, time))
        return Decide(plan, EditOutcome::NoOpBorderSkip, false);
    if (track.keys.find(Track::Key(time, 0.0f)) == track.keys.end())
        return Decide(plan, EditOutcome::NoOpNotFound, false);
    return Decide(plan, EditOutcome::Applied, true);
}

void ApplyDeleteTrackKey(Track& track, const TrackKeyPlan& plan)
{
    if (!plan.willMutate) return;
    Track::KeyMap::iterator it = track.keys.find(Track::Key(plan.findTime, 0.0f));
    assert(it != track.keys.end());   // the plan was made on a different track state
    if (it == track.keys.end()) return;
    track.keys.erase(it);
}

float DedupedInsertTime(const Track& track, float desiredTime)
{
    // Bounded by 1000 iterations so a pathological dataset can't lock the
    // dispatch thread. Kept verbatim from the add-track-key handler: the
    // cap, the literal and the post-decrement decide which time a long
    // collision run ends on.
    float time = desiredTime;
    Track::Key probe(time, 0.0f);
    int safety = 1000;
    while (track.keys.find(probe) != track.keys.end() && safety-- > 0)
    {
        time += 0.001f;
        probe = Track::Key(time, 0.0f);
    }
    return time;
}

EditResult ValidateAddTrackKey(const ProposedKey& key)
{
    // A missing field defaulting to 0 would silently insert a wrong key
    // (a typo'd param name in a --record clip records as success).
    if (!key.numeric)
        return Result(EditOutcome::Rejected, "add-track-key requires numeric 'time' and 'value'");
    if (!std::isfinite(key.time) || !std::isfinite(key.value))
        return Result(EditOutcome::Rejected, "add-track-key 'time'/'value' must be finite");
    return Result(EditOutcome::Applied);
}

EditResult ValidateBatchKeys(const vector<ProposedKey>& keys)
{
    // Rejecting mid-insert would leave half a paste in the document, so the
    // whole batch is checked first, in order: the first bad key decides.
    for (size_t i = 0; i < keys.size(); i++)
    {
        if (!keys[i].numeric)
            return Result(EditOutcome::Rejected, "add-track-keys requires numeric 'time' and 'value' on every key");
        if (!std::isfinite(keys[i].time) || !std::isfinite(keys[i].value))
            return Result(EditOutcome::Rejected, "add-track-keys 'time'/'value' must be finite");
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
    if (channelIdx >= 4)
        return Decide(plan, EditOutcome::NoOpOutOfDomain, false);

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
    if      (name == "linear") out = Track::IT_LINEAR;
    else if (name == "smooth") out = Track::IT_SMOOTH;
    else if (name == "step")   out = Track::IT_STEP;
    else return false;
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
    if (track.interpolation == plan.next)
        return Decide(plan, EditOutcome::NoOpUnchanged, false);
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

EditResult ValidateFullTrackReplacement(const Emitter& emitter, int trackIdx,
                                        const vector<Track::Key>& proposed)
{
    if (trackIdx < 0 || trackIdx >= ParticleSystem::NUM_TRACKS)
        return Result(EditOutcome::Rejected, "unknown track");
    if (emitter.tracks[trackIdx] == NULL)
        return Result(EditOutcome::Rejected, "no track slot bound for this channel");

    // A locked color channel displays an earlier channel's keys; replacing
    // "its" keys would rewrite that other channel.
    if (trackIdx < 4 && emitter.tracks[trackIdx] != &emitter.trackContents[trackIdx])
        return Result(EditOutcome::Rejected, "track is locked to an earlier channel; unlock it before replacing its keys");

    // With randomRotation on, the save writes the rotation track from
    // randomRotationAverage and drops its keys.
    if (trackIdx == ParticleSystem::TRACK_ROTATION_SPEED && emitter.randomRotation)
        return Result(EditOutcome::Rejected, "rotationSpeed keys are not saved while randomRotation is on");

    // The file stores the two endpoint values and the keys between them.
    if (proposed.size() < 2)
        return Result(EditOutcome::Rejected, "a track needs at least 2 keys (the endpoints at 0 and 100)");
    // readTracks admits an intermediate key only while the track holds fewer
    // than kMaxAloTrackKeys keys, the first endpoint included.
    if (proposed.size() - 2 >= kMaxAloTrackKeys)
        return Result(EditOutcome::Rejected, "too many keys for one track");

    for (size_t i = 0; i < proposed.size(); i++)
    {
        if (!std::isfinite(proposed[i].time) || !std::isfinite(proposed[i].value))
            return Result(EditOutcome::Rejected, "key time and value must be finite");
    }

    // The endpoints reload at exactly 0 and 100 whatever their saved time.
    if (proposed.front().time != 0.0f)
        return Result(EditOutcome::Rejected, "the first key must be at time 0");
    if (proposed.back().time != 100.0f)
        return Result(EditOutcome::Rejected, "the last key must be at time 100");

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
    for (size_t i = 0; i < proposed.size(); i++)
        track.keys.insert(proposed[i]);
}

// ---- Atlas ------------------------------------------------------------------

EditResult ClassifyAtlasIndex(const Emitter& emitter, float indexValue)
{
    const int    side  = AtlasGridSide(emitter.textureSize);
    const double cells = (double)side * side;
    const double frame = std::floor((double)indexValue);
    if (std::isfinite(indexValue) && frame >= 0.0 && frame < cells)
        return Result(EditOutcome::Applied);

    char reason[160];
    snprintf(reason, sizeof(reason),
             "atlas index %g is outside the %d x %d grid; the engine wraps it to another frame",
             (double)indexValue, side, side);
    return Result(EditOutcome::OutOfGrid, reason);
}
