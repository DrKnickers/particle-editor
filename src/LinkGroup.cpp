#include "LinkGroup.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

const LinkExemptFlags& GetLinkExemptFlags()
{
    static const LinkExemptFlags k;
    return k;
}

uint32_t AllocateLinkGroupId(const ParticleSystem& system)
{
    uint32_t maxId = 0;
    const std::vector<ParticleSystem::Emitter*>& v = system.getEmitters();
    for (size_t i = 0; i < v.size(); i++)
    {
        if (v[i]->linkGroup > maxId) maxId = v[i]->linkGroup;
    }
    return maxId + 1;
}

std::vector<ParticleSystem::Emitter*> GetLinkGroupMembers(
    const ParticleSystem& system, uint32_t groupId)
{
    std::vector<ParticleSystem::Emitter*> out;
    if (groupId == 0) return out;
    const std::vector<ParticleSystem::Emitter*>& v = system.getEmitters();
    for (size_t i = 0; i < v.size(); i++)
    {
        if (v[i]->linkGroup == groupId) out.push_back(v[i]);
    }
    return out;
}

std::vector<uint32_t> GetAllLinkGroupIds(const ParticleSystem& system)
{
    std::vector<uint32_t> ids;
    const std::vector<ParticleSystem::Emitter*>& v = system.getEmitters();
    for (size_t i = 0; i < v.size(); i++)
    {
        uint32_t g = v[i]->linkGroup;
        if (g == 0) continue;
        if (std::find(ids.begin(), ids.end(), g) == ids.end())
        {
            ids.push_back(g);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

uint32_t CreateLinkGroup(ParticleSystem&                              system,
                          const std::vector<ParticleSystem::Emitter*>& members)
{
    if (members.size() < 2) return 0;
    for (size_t i = 0; i < members.size(); i++)
    {
        if (members[i] == NULL)          return 0;
        if (members[i]->linkGroup != 0)  return 0;  // already grouped
    }

    const uint32_t newId = AllocateLinkGroupId(system);
    const LinkExemptFlags& exempt = GetLinkExemptFlags();

    // Members[0] is canonical; everyone else's non-exempt fields are
    // overwritten to match. We assign linkGroup BEFORE the bulk copy
    // so the propagation invariant (group members agree on every
    // non-exempt field) holds the moment the function returns.
    members[0]->linkGroup = newId;
    for (size_t i = 1; i < members.size(); i++)
    {
        members[i]->copySharedParamsFrom(*members[0], exempt);
        members[i]->linkGroup = newId;
    }

#ifndef NDEBUG
    printf("[Link] create group=%u members=%zu\n", newId, members.size());
    fflush(stdout);
#endif
    return newId;
}

bool JoinLinkGroup(ParticleSystem&          system,
                    ParticleSystem::Emitter* joiner,
                    uint32_t                 groupId)
{
    if (joiner == NULL || groupId == 0)      return false;
    if (joiner->linkGroup != 0)              return false;
    std::vector<ParticleSystem::Emitter*> members = GetLinkGroupMembers(system, groupId);
    if (members.empty())                     return false;

    joiner->copySharedParamsFrom(*members[0], GetLinkExemptFlags());
    joiner->linkGroup = groupId;

#ifndef NDEBUG
    printf("[Link] join group=%u (now %zu members)\n",
           groupId, members.size() + 1);
    fflush(stdout);
#endif
    return true;
}

bool LeaveLinkGroup(ParticleSystem&          system,
                     ParticleSystem::Emitter* member)
{
    if (member == NULL || member->linkGroup == 0) return false;

    const uint32_t groupId  = member->linkGroup;
    std::vector<ParticleSystem::Emitter*> remaining
        = GetLinkGroupMembers(system, groupId);

    // Detach the requested member.
    member->detachFromLinkGroup();

    // Recompute remaining (excludes member now). If exactly one
    // emitter remains in the group, auto-dissolve to preserve the
    // "no 1-member groups" invariant.
    std::vector<ParticleSystem::Emitter*> stillIn;
    for (size_t i = 0; i < remaining.size(); i++)
    {
        if (remaining[i] != member && remaining[i]->linkGroup == groupId)
        {
            stillIn.push_back(remaining[i]);
        }
    }
    if (stillIn.size() == 1)
    {
#ifndef NDEBUG
        printf("[Link] auto-dissolve group=%u (removal would leave 1)\n",
               groupId);
        fflush(stdout);
#endif
        stillIn[0]->detachFromLinkGroup();
    }

#ifndef NDEBUG
    printf("[Link] leave group=%u (now %zu members)\n",
           groupId, stillIn.size() == 1 ? 0u : stillIn.size());
    fflush(stdout);
#endif
    return true;
}

size_t DissolveLinkGroup(ParticleSystem& system, uint32_t groupId)
{
    if (groupId == 0) return 0;
    std::vector<ParticleSystem::Emitter*> members = GetLinkGroupMembers(system, groupId);
    for (size_t i = 0; i < members.size(); i++)
    {
        members[i]->detachFromLinkGroup();
    }
#ifndef NDEBUG
    if (!members.empty())
    {
        printf("[Link] dissolve group=%u (was %zu members)\n",
               groupId, members.size());
        fflush(stdout);
    }
#endif
    return members.size();
}

// Helper: compare a track's keys + interpolation type for equality.
static bool TracksEqual(const ParticleSystem::Emitter::Track& a,
                         const ParticleSystem::Emitter::Track& b)
{
    if (a.interpolation != b.interpolation) return false;
    if (a.keys.size()   != b.keys.size())   return false;
    return std::equal(a.keys.begin(), a.keys.end(), b.keys.begin());
}

std::vector<std::string> DiffNonExemptParams(
    const ParticleSystem::Emitter& a,
    const ParticleSystem::Emitter& b)
{
    std::vector<std::string> diffs;
    const LinkExemptFlags& exempt = GetLinkExemptFlags();

    // Scalars
    #define CHECK_FIELD(field, label) \
        do { if (a.field != b.field) diffs.push_back(label); } while (0)

    CHECK_FIELD(linkToSystem,            "linkToSystem");
    CHECK_FIELD(objectSpaceAcceleration, "objectSpaceAcceleration");
    CHECK_FIELD(doColorAddGrayscale,     "doColorAddGrayscale");
    CHECK_FIELD(affectedByWind,          "affectedByWind");
    CHECK_FIELD(isHeatParticle,          "isHeatParticle");
    CHECK_FIELD(isWeatherParticle,       "isWeatherParticle");
    CHECK_FIELD(hasTail,                 "hasTail");
    CHECK_FIELD(noDepthTest,             "noDepthTest");
    CHECK_FIELD(randomRotation,          "randomRotation");
    CHECK_FIELD(randomRotationDirection, "randomRotationDirection");
    CHECK_FIELD(isWorldOriented,         "isWorldOriented");
    CHECK_FIELD(useBursts,               "useBursts");
    CHECK_FIELD(emitFromMesh,            "emitFromMesh");
    CHECK_FIELD(gravity,                 "gravity");
    CHECK_FIELD(lifetime,                "lifetime");
    CHECK_FIELD(initialDelay,            "initialDelay");
    CHECK_FIELD(burstDelay,              "burstDelay");
    CHECK_FIELD(inwardSpeed,             "inwardSpeed");
    CHECK_FIELD(inwardAcceleration,      "inwardAcceleration");
    CHECK_FIELD(randomScalePerc,         "randomScalePerc");
    CHECK_FIELD(randomLifetimePerc,      "randomLifetimePerc");
    CHECK_FIELD(weatherCubeSize,         "weatherCubeSize");
    CHECK_FIELD(tailSize,                "tailSize");
    CHECK_FIELD(parentLinkStrength,      "parentLinkStrength");
    CHECK_FIELD(weatherCubeDistance,     "weatherCubeDistance");
    CHECK_FIELD(randomRotationAverage,   "randomRotationAverage");
    CHECK_FIELD(randomRotationVariance,  "randomRotationVariance");
    CHECK_FIELD(bounciness,              "bounciness");
    CHECK_FIELD(freezeTime,              "freezeTime");
    CHECK_FIELD(skipTime,                "skipTime");
    CHECK_FIELD(emitFromMeshOffset,      "emitFromMeshOffset");
    CHECK_FIELD(weatherFadeoutDistance,  "weatherFadeoutDistance");
    CHECK_FIELD(nBursts,                 "nBursts");
    CHECK_FIELD(blendMode,               "blendMode");
    CHECK_FIELD(textureSize,             "textureSize");
    CHECK_FIELD(nParticlesPerSecond,     "nParticlesPerSecond");
    CHECK_FIELD(nTriangles,              "nTriangles");
    CHECK_FIELD(nParticlesPerBurst,      "nParticlesPerBurst");
    CHECK_FIELD(groundBehavior,          "groundBehavior");

    #undef CHECK_FIELD

    if (memcmp(a.acceleration, b.acceleration, sizeof(a.acceleration)) != 0)
        diffs.push_back("acceleration");
    if (memcmp(a.randomColors, b.randomColors, sizeof(a.randomColors)) != 0)
        diffs.push_back("randomColors");

    // Random param groups
    for (int i = 0; i < ParticleSystem::NUM_GROUPS; i++)
    {
        if (memcmp(&a.groups[i], &b.groups[i],
                   sizeof(ParticleSystem::Emitter::Group)) != 0)
        {
            const char* label = (i == 0) ? "groups[SPEED]"
                              : (i == 1) ? "groups[LIFETIME]"
                                         : "groups[POSITION]";
            diffs.push_back(label);
        }
    }

    // Tracks — skip TRACK_INDEX (exempt) and check the rest
    static const char* trackLabels[ParticleSystem::NUM_TRACKS] = {
        "red curve", "green curve", "blue curve", "alpha curve",
        "scale curve", "index curve", "rotation curve"
    };
    for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++)
    {
        if (i == ParticleSystem::TRACK_INDEX && exempt.trackIndex) continue;
        if (!TracksEqual(*a.tracks[i], *b.tracks[i]))
        {
            diffs.push_back(trackLabels[i]);
        }
    }

    return diffs;
}
