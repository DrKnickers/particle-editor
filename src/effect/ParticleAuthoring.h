#ifndef PARTICLEAUTHORING_H
#define PARTICLEAUTHORING_H

#include <cmath>
#include <string>
#include <vector>

#include "ParticleSystem.h"

// Window-free authoring rules for emitter tracks: the legality the bridge
// track handlers (src/host/BridgeDispatch_EmitterTracks.cpp) apply today,
// in one place, so the editor and the planned `--tool patch` verbs apply the
// same rules (agentic-VFX design §14). Pure over the data model: nothing here
// touches undo, dirty state, selection, engine invalidation, events or the
// wire. Those stay with the caller (the "adapter").
//
// Mutating edits come as a Plan*/Apply* pair. Plan* is side-effect-free and
// reports whether the edit will mutate; the caller uses that bit to decide
// undo capture and events, then calls Apply*:
//
//     plan = PlanX(...);
//     if (plan.willMutate) captureUndo();
//     ApplyX(..., plan);                  // a no-op unless plan.willMutate
//     if (plan.willMutate) { invalidate; emit; }
//
// A plan is only valid against the unchanged object it was planned on.
//
// A Rejected `reason` from an extracted rule is the verbatim error string the
// bridge sends today, so the bridge can pass it straight to the wire. The new
// rules (NaN keys, full-track replacement) have reasons of their own.

enum class EditOutcome
{
    Applied,            // the edit is legal and mutates (or, for a check, passes)
    AppliedBorderTime,  // set-track-key on a first/last key: time pinned, value applied
    NoOpBorderSkip,     // delete-track-keys on a first/last key: skipped
    NoOpNotFound,       // no key at the requested time
    NoOpUnchanged,      // already in the requested state
    NoOpOutOfDomain,    // the edit does not apply to this target (lock on a non-RGBA track)
    OutOfGrid,          // advisory only: an atlas index outside the grid (the engine wraps it)
    Rejected            // illegal; `reason` says why
};

struct EditResult
{
    EditOutcome outcome;
    std::string reason;
};

struct EditPlan
{
    EditOutcome outcome;
    std::string reason;
    bool        willMutate;
};

// set-track-key / delete-track-keys: the key found at `findTime` is erased
// and, for a set, `replacement` is inserted.
struct TrackKeyPlan : EditPlan
{
    float      findTime;
    ParticleSystem::Emitter::Track::Key replacement;
};

// set-track-lock: tracks[channel] = &trackContents[source]. source == channel unlocks.
struct TrackLockPlan : EditPlan
{
    int channel;
    int source;
};

struct InterpolationPlan : EditPlan
{
    ParticleSystem::Emitter::Track::InterpolationType next;
};

// A key as the caller received it. `numeric` is false when the request did
// not carry a numeric time and value (the caller's JSON shape check).
struct ProposedKey
{
    bool  numeric;
    float time;
    float value;
};

// ---- Track keys -------------------------------------------------------------

// The first and last keys (in time order) are the track's border keys. They
// pin the [0, 100] range: their time never moves and they are never deleted.
bool IsBorderKeyTime(const ParticleSystem::Emitter::Track& track, float time);

// Key bounds. The save stores the two endpoint VALUES and each interior key's
// time / 100; the reload puts the endpoints back at 0 and 100 and refuses an
// interior time past 100 or a color value outside [0, 1]. So a key time
// outside [0, 100] is silently re-timed by a save (two of them make the file
// unloadable), and a color value outside [0, 1] makes it unloadable.
inline bool IsColorTrack(int trackIdx) { return trackIdx >= 0 && trackIdx < 4; }
inline bool InTrackTimeRange(float time) { return time >= 0.0f && time <= 100.0f; }   // NaN fails

// Border key: the time stays at oldTime, the value is still applied. Refuses
// a non-border key's newTime outside [0, 100] (a border key's newTime is
// ignored, so it is not checked), a non-finite newValue, and on a color
// track a newValue outside [0, 1].
TrackKeyPlan PlanSetTrackKey(const ParticleSystem::Emitter::Track& track, float oldTime, float newTime,
                             float newValue, bool isColor);
void         ApplySetTrackKey(ParticleSystem::Emitter::Track& track, const TrackKeyPlan& plan);

// One time of a delete-track-keys request. A border key is skipped. Borders
// never move, so planning each time against the current track matches the
// handler's up-front border capture. Unlike the other edits, the handler
// captures undo once before its loop whenever the track has keys, whatever
// the plans say; it reports a change iff some plan mutated.
TrackKeyPlan PlanDeleteTrackKey(const ParticleSystem::Emitter::Track& track, float time);
void         ApplyDeleteTrackKey(ParticleSystem::Emitter::Track& track, const TrackKeyPlan& plan);

// The time an added key is inserted at: bumped by 0.001 until no key
// occupies it, at most 1000 times. If the upward bumps would leave [0, 100]
// (an add at an occupied 100), the search runs downward from the desired
// time instead, so the key stays inside the track. If every bump collides,
// the last time is returned and the insert makes a true duplicate. Within a
// batch, insert each key before computing the next one's time.
float DedupedInsertTime(const ParticleSystem::Emitter::Track& track, float desiredTime);

// emitters/add-track-key input check: numeric, finite, time within [0, 100],
// and on a color track a value within [0, 1].
EditResult ValidateAddTrackKey(const ProposedKey& key, bool isColor);

// emitters/add-track-keys: checks the WHOLE batch, key by key, with the same
// rules, and reports the first bad key's reason. The handler runs this before
// it resolves the emitter, so a bad key wins over an unknown emitter.
EditResult ValidateBatchKeys(const std::vector<ProposedKey>& keys, bool isColor);

// ---- Track lock -------------------------------------------------------------

// A color channel (0..3) can lock only to an EARLIER color channel.
// `unlock` restores the channel's own keys. With unlock false, `lockToIdx` is
// the resolved target track (-1 when the name did not resolve).
TrackLockPlan PlanSetTrackLock(const ParticleSystem::Emitter& emitter, int channelIdx, int lockToIdx, bool unlock);
void          ApplySetTrackLock(ParticleSystem::Emitter& emitter, const TrackLockPlan& plan);

// ---- Interpolation ----------------------------------------------------------

// "linear" / "smooth" / "step".
bool ParseInterpolation(const std::string& name, ParticleSystem::Emitter::Track::InterpolationType& out);
bool IsValidInterpolation(const std::string& name);

InterpolationPlan PlanSetTrackInterpolation(const ParticleSystem::Emitter::Track& track, const std::string& name);
void              ApplySetTrackInterpolation(ParticleSystem::Emitter::Track& track, const InterpolationPlan& plan);

// ---- Scalar fields ----------------------------------------------------------

// A float field value that is finite and fits in a float.
bool IsFiniteFloatField(double value);

// An int field value that fits in an int (NaN does not).
bool InIntFieldRange(double value);

// ---- Full-track replacement (new; called by the --tool verbs only) ----------

// Replace every key of tracks[trackIdx] with `proposed`, in order. Legal
// only when the result saves and reloads as given (ParticleSystemSerialization.cpp
// writeTracks/readTracks): the endpoints at exactly 0 and 100, times
// non-decreasing within [0, 100], everything finite, color values in [0, 1],
// at most kMaxAloTrackKeys intermediate keys. It also refuses a locked color
// channel (the keys belong to the channel it is locked to) and the rotation
// track while randomRotation is on (the save does not store those keys).
// Index values outside the atlas grid are legal; see ClassifyAtlasIndex.
EditResult ValidateFullTrackReplacement(const ParticleSystem::Emitter& emitter, int trackIdx,
                                        const std::vector<ParticleSystem::Emitter::Track::Key>& proposed);
// Call only after ValidateFullTrackReplacement returned Applied.
void       ApplyFullTrackReplacement(ParticleSystem::Emitter& emitter, int trackIdx,
                                     const std::vector<ParticleSystem::Emitter::Track::Key>& proposed);

// ---- Atlas ------------------------------------------------------------------

// Grid side of an emitter's atlas: textureSize is the FRAME COUNT, and the
// sheet is a side x side grid. The renderer's m_textureSizeSqrt uses this.
inline int AtlasGridSide(unsigned long textureSize)
{
    return (int)std::floor(std::sqrt((float)(textureSize > 1 ? textureSize : 1ul)));
}

// Advisory: OutOfGrid when an index-track value falls outside the side x side
// grid. The engine wraps such an index rather than refusing it, so this is a
// warning and never Rejected. Applied when the index is inside the grid.
EditResult ClassifyAtlasIndex(const ParticleSystem::Emitter& emitter, float indexValue);

// ---- Link groups ------------------------------------------------------------

// The --tool verbs refuse to edit a linked emitter (design §14.3). The bridge
// does not use this: the editor keeps its linked-edit behavior.
inline bool HasLinkGroupMembership(const ParticleSystem::Emitter& emitter)
{
    return emitter.linkGroup != 0;
}

#endif
