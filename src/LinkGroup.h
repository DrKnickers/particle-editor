#ifndef LINKGROUP_H
#define LINKGROUP_H

#include <vector>
#include <string>
#include <cstdint>

#include "ParticleSystem.h"

// Linked-emitter group helpers ().
//
// A "link group" is a set of two or more emitters in a single
// ParticleSystem whose non-exempt parameters are kept identical:
// editing any non-exempt field on one member auto-propagates to the
// rest, via the universal post-edit chokepoint in `CaptureUndo`
// (see src/main.cpp). The exempt-field set is hard-coded in v1
// (see `LinkExemptFlags` below); a future feature will let users
// toggle which fields participate.
//
// Group membership is stored as a `uint32_t linkGroup` on each
// `ParticleSystem::Emitter`. 0 = unlinked. Non-zero IDs are unique
// within the system, allocated as `max + 1` across live emitters,
// retired on dissolve, and never reused within a session. Stable
// across save/load via an editor-only chunk (game engine skips).
//
// Minimum group size is 2. Single-member groups are not produced by
// any user-visible operation; the data model permits the transient
// value during edits, but every operation here preserves the
// invariant (e.g. LeaveLinkGroup auto-dissolves the group when
// removal would leave only one member).

// Per-group exempt set (). Each bool toggles whether the
// corresponding emitter field is per-emitter (true = exempt = each
// member keeps its own) or shared (false = propagated). The default
// constructor restores the v1 hard-coded set: textures + atlas-index
// curve + name. `name` is always-exempt — surfaced in the struct for
// completeness, but the settings dialog doesn't display it (no sane
// workflow shares a name across linked emitters).
//
// Layout is POD so the whole struct can be serialized as a raw byte
// blob with a length prefix. Adding fields in the future is safe:
// older readers see a smaller blob and default the missing tail;
// newer readers tolerate larger blobs by reading what they know.
struct LinkExemptFlags
{
    // Textures + identity (default exempt).
    bool colorTexture;
    bool normalTexture;
    bool name;
    bool trackIndex;             // TRACK_INDEX curve (atlas sub-frame); default exempt

    // Curves (default shared except trackIndex above).
    bool trackRed;
    bool trackGreen;
    bool trackBlue;
    bool trackAlpha;
    bool trackScale;
    bool trackRotationSpeed;

    // Lifetime / spawning.
    bool lifetime;
    bool initialDelay;
    bool burstDelay;
    bool nBursts;
    bool nParticlesPerBurst;
    bool nParticlesPerSecond;
    bool useBursts;

    // Physics.
    bool gravity;
    bool acceleration;
    bool inwardSpeed;
    bool inwardAcceleration;
    bool bounciness;
    bool groundBehavior;
    bool objectSpaceAcceleration;
    bool affectedByWind;

    // Appearance.
    bool blendMode;
    bool textureSize;
    bool nTriangles;
    bool randomScalePerc;
    bool randomLifetimePerc;
    bool hasTail;
    bool tailSize;
    bool noDepthTest;
    bool randomColors;

    // Weather.
    bool isWeatherParticle;
    bool weatherCubeSize;
    bool weatherCubeDistance;
    bool weatherFadeoutDistance;

    // Rotation.
    bool randomRotation;
    bool randomRotationDirection;
    bool randomRotationAverage;
    bool randomRotationVariance;

    // Misc.
    bool linkToSystem;
    bool parentLinkStrength;
    bool doColorAddGrayscale;
    bool isHeatParticle;
    bool isWorldOriented;
    bool freezeTime;
    bool skipTime;
    bool emitFromMesh;
    bool emitFromMeshOffset;
    bool groupSpeed;             // groups[0]
    bool groupLifetime;          // groups[1]
    bool groupPosition;          // groups[2]

    // Unknown fields — data-model complete so save/restore is symmetric,
    // but the settings dialog doesn't surface them (the editor's
    // inspector doesn't either).
    bool unknown06;
    bool unknown11;
    bool unknown15;
    bool unknown2b;
    bool unknown3f;
    bool unknown44;
    bool unknown49;

    LinkExemptFlags();           // sets v1 defaults; see LinkGroup.cpp

    // Bytewise equality. Used by ParticleSystem::setLinkExemptFlags
    // to normalize on-default entries out of the map.
    bool operator == (const LinkExemptFlags& other) const;
    bool operator != (const LinkExemptFlags& other) const
    { return !(*this == other); }
};

// v1 defaults — what new groups (and groups without a per-group entry
// in m_linkExempts) use. Kept as a static const so the const reference
// returned by ParticleSystem::getLinkExemptFlags has a stable address
// to point at when the group has no map entry.
const LinkExemptFlags& GetDefaultLinkExemptFlags();

// The "shared params" copy is a member of `ParticleSystem::Emitter`
// (see ParticleSystem.h `copySharedParamsFrom`). Declared there
// because it needs access to the private `m_instances` set, which
// must be preserved on the destination emitter — propagating link
// params must not corrupt the runtime EmitterInstance bookkeeping.

// Allocate a fresh link-group ID in `system`. Returns max(linkGroup)
// across live emitters, plus 1. The returned ID is not yet assigned
// to any emitter — callers should immediately apply it.
uint32_t AllocateLinkGroupId(const ParticleSystem& system);

// Create a new link group containing every emitter in `members`.
// Returns the new group ID. The first member is the canonical
// source; every other member's non-exempt fields are overwritten to
// match. Requires `members.size() >= 2`; refuses with returned 0 if
// any member is already in a group, or if too few were supplied.
uint32_t CreateLinkGroup(ParticleSystem&                                       system,
                          const std::vector<ParticleSystem::Emitter*>&         members);

// Add `joiner` to existing group `groupId`. The joiner's non-exempt
// fields are overwritten to match the group's canonical member.
// Refuses (returns false) if joiner is already linked, the group
// doesn't exist, or arguments are NULL.
bool JoinLinkGroup(ParticleSystem&            system,
                    ParticleSystem::Emitter*   joiner,
                    uint32_t                   groupId);

// Remove `member` from its group. If the group would be left with
// exactly one member after removal, that remaining lone member is
// also detached (the whole group dissolves in a single call). The
// caller does not need to follow up with a Dissolve.
//
// Returns true on success, false if member is NULL or unlinked.
bool LeaveLinkGroup(ParticleSystem&          system,
                     ParticleSystem::Emitter* member);

// Detach every member of `groupId`. The ID is retired. Returns the
// number of emitters detached (0 if no such group existed).
size_t DissolveLinkGroup(ParticleSystem& system, uint32_t groupId);

// Return every emitter in `groupId` (empty vector if groupId == 0
// or no such group exists).
std::vector<ParticleSystem::Emitter*> GetLinkGroupMembers(
    const ParticleSystem& system, uint32_t groupId);

// Return field names that differ between `a` and `b` (considering
// only non-exempt fields per `exempt`). Used by the Join confirmation
// dialog: when the diff is empty, the join is silent; otherwise the
// dialog lists the affected fields so the user can give informed
// consent. Pre-callers passed no flags (implicit v1 defaults);
// post-callers pass the group's specific flags to get the right
// answer for groups with custom exempt sets.
std::vector<std::string> DiffNonExemptParams(
    const ParticleSystem::Emitter& a,
    const ParticleSystem::Emitter& b,
    const LinkExemptFlags&         exempt);

#endif // LINKGROUP_H
