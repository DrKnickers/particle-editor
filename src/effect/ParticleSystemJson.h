#ifndef PARTICLE_SYSTEM_JSON_H
#define PARTICLE_SYSTEM_JSON_H

// The `--tool inspect` dump (schema 1): everything the .alo writer persists,
// read from a loaded ParticleSystem. Pure -- no window, device or file IO.
//
// Floats are emitted as their exact float32 values (widened to double, which
// prints the shortest decimal that reads back to the same float). Strings are
// the loaded bytes, unchanged; FindInvalidInspectValue refuses any that are
// not valid UTF-8 rather than decoding them with replacements, which would
// make distinct names compare equal. It also refuses a non-finite float,
// which the JSON writer would otherwise collapse to `null`.
//
// The dump describes the LOADED state: values the loader normalises (for
// example randomRotationVariance, clamped into [0, 1]) appear normalised.
// Every member listed here is mirrored in tests/fixtures/persisted-members.json,
// which scripts/persisted-members.test.mjs checks against the declarations in
// ParticleSystem.h and LinkGroup.h.

#include <cmath>
#include <cstdint>
#include <string>
#include "ParticleSystem.h"
#include "LinkGroup.h"
#include "ParticleAuthoring.h"
#include "host/third_party/nlohmann/json.hpp"

namespace particle_json
{
using json = nlohmann::json;

inline json Float(float value) { return json(static_cast<double>(value)); }

inline json Index(size_t value)
{
    return value == static_cast<size_t>(-1) ? json(nullptr) : json(static_cast<uint64_t>(value));
}

inline json GroupJson(const ParticleSystem::Emitter::Group& g)
{
    return {
        {"type", g.type},
        {"minX", Float(g.minX)}, {"minY", Float(g.minY)}, {"minZ", Float(g.minZ)},
        {"maxX", Float(g.maxX)}, {"maxY", Float(g.maxY)}, {"maxZ", Float(g.maxZ)},
        {"sideLength", Float(g.sideLength)},
        {"sphereRadius", Float(g.sphereRadius)}, {"sphereEdge", g.sphereEdge},
        {"cylinderRadius", Float(g.cylinderRadius)}, {"cylinderEdge", g.cylinderEdge},
        {"cylinderHeight", Float(g.cylinderHeight)},
        {"valX", Float(g.valX)}, {"valY", Float(g.valY)}, {"valZ", Float(g.valZ)},
    };
}

// `source` is the channel whose contents this track shows (itself when
// unlocked); `explicitUnlock` is this channel's bit in chunk 0x0101.
inline json TrackJson(const ParticleSystem::Emitter& e, int i)
{
    int source = i;
    for (int j = 0; j < ParticleSystem::NUM_TRACKS; j++)
        if (e.tracks[i] == &e.trackContents[j]) { source = j; break; }
    json keys = json::array();
    for (const auto& key : e.tracks[i]->keys)
        keys.push_back(json::array({Float(key.time), Float(key.value)}));
    return {
        {"source", source},
        {"explicitUnlock", (e.explicitUnlockMask() & (1ul << i)) != 0},
        {"interpolation", static_cast<int>(e.tracks[i]->interpolation)},
        {"keys", keys},
    };
}

// `parent` is the parent's position in the emitter list (what the spawn
// slots refer to), or null for a root.
inline json EmitterJson(const ParticleSystem::Emitter& e, json parent)
{
    json groups = json::array();
    for (int i = 0; i < ParticleSystem::NUM_GROUPS; i++) groups.push_back(GroupJson(e.groups[i]));
    json tracks = json::array();
    for (int i = 0; i < ParticleSystem::NUM_TRACKS; i++) tracks.push_back(TrackJson(e, i));
    return {
        {"index", static_cast<uint64_t>(e.index)},
        {"name", e.name},
        {"parent", parent},
        {"spawnOnDeath", Index(e.spawnOnDeath)},
        {"spawnDuringLife", Index(e.spawnDuringLife)},
        {"linkGroup", e.linkGroup},
        {"colorTexture", e.colorTexture},
        {"normalTexture", e.normalTexture},
        {"groups", groups},
        {"tracks", tracks},
        {"linkToSystem", e.linkToSystem},
        {"objectSpaceAcceleration", e.objectSpaceAcceleration},
        {"doColorAddGrayscale", e.doColorAddGrayscale},
        {"affectedByWind", e.affectedByWind},
        {"isHeatParticle", e.isHeatParticle},
        {"isWeatherParticle", e.isWeatherParticle},
        {"hasTail", e.hasTail},
        {"noDepthTest", e.noDepthTest},
        {"randomRotation", e.randomRotation},
        {"randomRotationDirection", e.randomRotationDirection},
        {"isWorldOriented", e.isWorldOriented},
        {"useBursts", e.useBursts},
        {"emitFromMesh", e.emitFromMesh},
        {"gravity", Float(e.gravity)},
        {"lifetime", Float(e.lifetime)},
        {"initialDelay", Float(e.initialDelay)},
        {"burstDelay", Float(e.burstDelay)},
        {"inwardSpeed", Float(e.inwardSpeed)},
        {"inwardAcceleration", Float(e.inwardAcceleration)},
        {"acceleration", json::array({Float(e.acceleration[0]), Float(e.acceleration[1]), Float(e.acceleration[2])})},
        {"randomScalePerc", Float(e.randomScalePerc)},
        {"randomLifetimePerc", Float(e.randomLifetimePerc)},
        {"weatherCubeSize", Float(e.weatherCubeSize)},
        {"tailSize", Float(e.tailSize)},
        {"parentLinkStrength", Float(e.parentLinkStrength)},
        {"weatherCubeDistance", Float(e.weatherCubeDistance)},
        {"randomRotationAverage", Float(e.randomRotationAverage)},
        {"randomRotationVariance", Float(e.randomRotationVariance)},
        {"randomColors", json::array({Float(e.randomColors[0]), Float(e.randomColors[1]),
                                      Float(e.randomColors[2]), Float(e.randomColors[3])})},
        {"bounciness", Float(e.bounciness)},
        {"freezeTime", Float(e.freezeTime)},
        {"skipTime", Float(e.skipTime)},
        {"emitFromMeshOffset", Float(e.emitFromMeshOffset)},
        {"weatherFadeoutDistance", Float(e.weatherFadeoutDistance)},
        {"nBursts", static_cast<uint64_t>(e.nBursts)},
        {"blendMode", static_cast<uint64_t>(e.blendMode)},
        {"textureSize", static_cast<uint64_t>(e.textureSize)},
        {"nParticlesPerSecond", static_cast<uint64_t>(e.nParticlesPerSecond)},
        {"nTriangles", static_cast<uint64_t>(e.nTriangles)},
        {"nParticlesPerBurst", static_cast<uint64_t>(e.nParticlesPerBurst)},
        {"groundBehavior", static_cast<uint64_t>(e.groundBehavior)},
        {"unknown15", e.unknown15},
        {"unknown2b", e.unknown2b},
        {"unknown44", e.unknown44},
        {"unknown11", e.has11 ? Float(e.unknown11) : json(nullptr)},
        {"unknown3f", Float(e.unknown3f)},
        {"unknown06", static_cast<uint64_t>(e.unknown06)},
        {"unknown49", static_cast<uint64_t>(e.unknown49)},
    };
}

// Every LinkExemptFlags member, by name. persisted-members.test.mjs keeps this
// in step with LinkGroup.h through persisted-members.json.
struct ExemptMember { const char* name; bool LinkExemptFlags::* member; };
inline const ExemptMember* ExemptMembers(size_t* count)
{
    static const ExemptMember members[] = {
        {"colorTexture", &LinkExemptFlags::colorTexture}, {"normalTexture", &LinkExemptFlags::normalTexture},
        {"name", &LinkExemptFlags::name}, {"trackIndex", &LinkExemptFlags::trackIndex},
        {"trackRed", &LinkExemptFlags::trackRed}, {"trackGreen", &LinkExemptFlags::trackGreen},
        {"trackBlue", &LinkExemptFlags::trackBlue}, {"trackAlpha", &LinkExemptFlags::trackAlpha},
        {"trackScale", &LinkExemptFlags::trackScale}, {"trackRotationSpeed", &LinkExemptFlags::trackRotationSpeed},
        {"lifetime", &LinkExemptFlags::lifetime}, {"initialDelay", &LinkExemptFlags::initialDelay},
        {"burstDelay", &LinkExemptFlags::burstDelay}, {"nBursts", &LinkExemptFlags::nBursts},
        {"nParticlesPerBurst", &LinkExemptFlags::nParticlesPerBurst},
        {"nParticlesPerSecond", &LinkExemptFlags::nParticlesPerSecond}, {"useBursts", &LinkExemptFlags::useBursts},
        {"gravity", &LinkExemptFlags::gravity}, {"acceleration", &LinkExemptFlags::acceleration},
        {"inwardSpeed", &LinkExemptFlags::inwardSpeed}, {"inwardAcceleration", &LinkExemptFlags::inwardAcceleration},
        {"bounciness", &LinkExemptFlags::bounciness}, {"groundBehavior", &LinkExemptFlags::groundBehavior},
        {"objectSpaceAcceleration", &LinkExemptFlags::objectSpaceAcceleration},
        {"affectedByWind", &LinkExemptFlags::affectedByWind},
        {"blendMode", &LinkExemptFlags::blendMode}, {"textureSize", &LinkExemptFlags::textureSize},
        {"nTriangles", &LinkExemptFlags::nTriangles}, {"randomScalePerc", &LinkExemptFlags::randomScalePerc},
        {"randomLifetimePerc", &LinkExemptFlags::randomLifetimePerc}, {"hasTail", &LinkExemptFlags::hasTail},
        {"tailSize", &LinkExemptFlags::tailSize}, {"noDepthTest", &LinkExemptFlags::noDepthTest},
        {"randomColors", &LinkExemptFlags::randomColors},
        {"isWeatherParticle", &LinkExemptFlags::isWeatherParticle},
        {"weatherCubeSize", &LinkExemptFlags::weatherCubeSize},
        {"weatherCubeDistance", &LinkExemptFlags::weatherCubeDistance},
        {"weatherFadeoutDistance", &LinkExemptFlags::weatherFadeoutDistance},
        {"randomRotation", &LinkExemptFlags::randomRotation},
        {"randomRotationDirection", &LinkExemptFlags::randomRotationDirection},
        {"randomRotationAverage", &LinkExemptFlags::randomRotationAverage},
        {"randomRotationVariance", &LinkExemptFlags::randomRotationVariance},
        {"linkToSystem", &LinkExemptFlags::linkToSystem},
        {"parentLinkStrength", &LinkExemptFlags::parentLinkStrength},
        {"doColorAddGrayscale", &LinkExemptFlags::doColorAddGrayscale},
        {"isHeatParticle", &LinkExemptFlags::isHeatParticle}, {"isWorldOriented", &LinkExemptFlags::isWorldOriented},
        {"freezeTime", &LinkExemptFlags::freezeTime}, {"skipTime", &LinkExemptFlags::skipTime},
        {"emitFromMesh", &LinkExemptFlags::emitFromMesh}, {"emitFromMeshOffset", &LinkExemptFlags::emitFromMeshOffset},
        {"groupSpeed", &LinkExemptFlags::groupSpeed}, {"groupLifetime", &LinkExemptFlags::groupLifetime},
        {"groupPosition", &LinkExemptFlags::groupPosition},
        {"unknown06", &LinkExemptFlags::unknown06}, {"unknown11", &LinkExemptFlags::unknown11},
        {"unknown15", &LinkExemptFlags::unknown15}, {"unknown2b", &LinkExemptFlags::unknown2b},
        {"unknown3f", &LinkExemptFlags::unknown3f}, {"unknown44", &LinkExemptFlags::unknown44},
        {"unknown49", &LinkExemptFlags::unknown49},
    };
    *count = sizeof(members) / sizeof(members[0]);
    return members;
}

inline json ParticleSystemJson(const ParticleSystem& system)
{
    json exempts = json::array();
    system.forEachLinkExempt([&](uint32_t group, const LinkExemptFlags& flags) {
        size_t count = 0;
        const ExemptMember* members = ExemptMembers(&count);
        json named = json::object();
        for (size_t i = 0; i < count; i++) named[members[i].name] = flags.*(members[i].member);
        exempts.push_back({{"linkGroup", group}, {"flags", named}});
    });
    json emitters = json::array();
    const std::vector<ParticleSystem::Emitter*>& list = system.getEmitters();
    for (const ParticleSystem::Emitter* e : list)
    {
        json parent = nullptr;
        for (size_t i = 0; i < list.size(); i++)
            if (e->parent == list[i]) parent = static_cast<uint64_t>(i);
        emitters.push_back(EmitterJson(*e, parent));
    }
    return {
        {"schema", 1},
        {"name", system.getName()},
        {"leaveParticles", system.getLeaveParticles()},
        {"linkExempts", exempts},
        {"emitters", emitters},
    };
}

// IsValidUtf8 lives in ParticleAuthoring.h, shared with the tool rules.
using ::IsValidUtf8;

// The first string that is not valid UTF-8 or float that is not finite, as a
// path such as `emitters[2].groups[0].maxY`. Returns false when there is none.
inline bool FindInvalidInspectValue(const json& value, std::string* path, std::string* reason,
                                    const std::string& at = "")
{
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
    {
        *path = at;
        *reason = "is not a finite number";
        return true;
    }
    if (value.is_string() && !IsValidUtf8(value.get_ref<const std::string&>()))
    {
        *path = at;
        *reason = "is not valid UTF-8";
        return true;
    }
    if (value.is_array())
    {
        for (size_t i = 0; i < value.size(); i++)
            if (FindInvalidInspectValue(value[i], path, reason, at + "[" + std::to_string(i) + "]")) return true;
    }
    else if (value.is_object())
    {
        for (auto it = value.begin(); it != value.end(); ++it)
            if (FindInvalidInspectValue(it.value(), path, reason, at.empty() ? it.key() : at + "." + it.key()))
                return true;
    }
    return false;
}

} // namespace particle_json

#endif // PARTICLE_SYSTEM_JSON_H
