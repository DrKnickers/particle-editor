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

// Hard-coded v1 exempt set. Future work: make this user-configurable.
struct LinkExemptFlags
{
    bool colorTexture;   // p_*.tga filename
    bool normalTexture;  // depth/normal filename
    bool trackIndex;     // TRACK_INDEX keymap (atlas sub-frame curve)
    bool name;           // each emitter keeps its own identifier

    LinkExemptFlags()
        : colorTexture(true)
        , normalTexture(true)
        , trackIndex(true)
        , name(true)
    {}
};

const LinkExemptFlags& GetLinkExemptFlags();

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

// Return every distinct non-zero linkGroup ID present in `system`,
// in ascending order. Useful for menu population.
std::vector<uint32_t> GetAllLinkGroupIds(const ParticleSystem& system);

// Return field names that differ between `a` and `b` (considering
// only non-exempt fields). Used by the Join confirmation dialog:
// when the diff is empty, the join is silent; otherwise the dialog
// lists the affected fields so the user can give informed consent.
std::vector<std::string> DiffNonExemptParams(
    const ParticleSystem::Emitter& a,
    const ParticleSystem::Emitter& b);

#endif // LINKGROUP_H
