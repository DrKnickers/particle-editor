#ifndef PERSISTED_PROJECTION_H
#define PERSISTED_PROJECTION_H

// What a save followed by a reload makes of an in-memory document, expressed
// on the `--tool inspect` dump (ParticleSystemJson.h): ProjectPersisted takes
// the dump of the document before saving and returns the dump the reloaded
// file will produce.
//
// Written by hand from the save and load rules, deliberately separate from
// the writer and the loader, so a test can hold the two against each other.
// Rules (ParticleSystemSerialization.cpp):
//   - colour endpoints are saved as a byte, (uint8)(int)(v*255), at times 0
//     and 100; colour interior keys as a 32-bit integer, (uint32)(v*255),
//     which must be <= 255 to load;
//   - every interior key time is saved as t/100 and read back times 100, and
//     must be <= 100 and non-decreasing to load; endpoints reload at 0 and 100;
//   - under randomRotation the rotation-speed track saves as the endpoint pair
//     (randomRotationAverage, 0) with no interior keys; otherwise the average
//     is not saved and reloads as its default, 0;
//   - the lifetime group is derived from lifetime and randomLifetimePerc;
//   - nTriangles reloads as max(1, n); nBursts 0xFFFFFFFF reloads as 0;
//     blendMode reloads modulo 14; randomRotationVariance is clamped to
//     [0, 1] (keeping -0); parentLinkStrength 0 (or -0) reloads as +0;
//   - interpolation reloads clamped to -1..2 (anything else is linear, 0);
//   - the reader re-locks a colour channel whose saved keys and
//     interpolation equal an earlier unlocked channel's, unless chunk 0x0101
//     marks it explicitly unlocked;
//   - an empty normal texture is not saved and reloads as the default name;
//   - the saved index is ignored; an emitter reloads at its position.
// Spawn links and link groups reload unchanged for a valid graph; loader graph
// repair is not a projection (a document needing it is refused elsewhere).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "host/third_party/nlohmann/json.hpp"

namespace particle_json
{
namespace projection_detail
{
using json = nlohmann::json;

inline float F(const json& v) { return static_cast<float>(v.get<double>()); }
inline json J(float v) { return json(static_cast<double>(v)); }

inline int ClampInterpolation(long long raw)
{
    return (raw < -1 || raw > 2) ? 0 : static_cast<int>(raw);
}

// The reloaded value of a colour endpoint.
inline bool ColourEndpoint(float v, float* out)
{
    const float scaled = v * 255;
    if (!(std::fabs(scaled) < 2147483648.0f)) return false;
    *out = static_cast<uint8_t>(static_cast<int>(scaled)) / 255.0f;
    return true;
}

typedef std::vector<std::pair<float, float>> Keys;   // (time, value)

// The reloaded keys of track `i`, or false with a reason.
inline bool ProjectTrackKeys(const json& emitter, int i, Keys* out, std::string* reason)
{
    const json& keys = emitter["tracks"][i]["keys"];
    if (keys.size() < 2) { *reason = "a track has fewer than two keys"; return false; }
    const bool colour = i < 4;
    const bool rotation = i == 6 && emitter["randomRotation"].get<bool>();
    out->clear();
    float first = F(keys.front()[1]);
    float last = F(keys.back()[1]);
    if (rotation)
    {
        first = F(emitter["randomRotationAverage"]);
        last = 0.0f;
    }
    if (colour && !(ColourEndpoint(first, &first) && ColourEndpoint(last, &last)))
    {
        *reason = "a colour endpoint cannot be saved as a byte";
        return false;
    }
    out->push_back({0.0f, first});
    if (!rotation)
    {
        for (size_t k = 1; k + 1 < keys.size(); k++)
        {
            float value = F(keys[k][1]);
            const float time = (F(keys[k][0]) / 100.0f) * 100.0f;
            if (colour)
            {
                const float scaled = value * 255;
                if (!(scaled >= 0.0f && scaled < 4294967296.0f))
                {
                    *reason = "a colour key value cannot be saved";
                    return false;
                }
                const uint32_t stored = static_cast<uint32_t>(static_cast<unsigned long>(scaled));
                value = stored / 255.0f;
                if (!(value <= 1.0f)) { *reason = "a colour key above 1 makes the file unloadable"; return false; }
            }
            if (!(time <= 100.0f && time >= out->back().first))
            {
                *reason = "a key time above 100 or out of order makes the file unloadable";
                return false;
            }
            out->push_back({time, value});
        }
    }
    out->push_back({100.0f, last});
    return true;
}

inline json KeysJson(const Keys& keys)
{
    json out = json::array();
    for (const auto& k : keys) out.push_back(json::array({J(k.first), J(k.second)}));
    return out;
}

inline bool ProjectEmitter(const json& in, size_t position, json* out, std::string* reason)
{
    json e = in;
    e["index"] = static_cast<uint64_t>(position);

    const uint64_t nTriangles = in["nTriangles"].get<uint64_t>();
    e["nTriangles"] = nTriangles < 1 ? 1 : nTriangles;
    if (in["nBursts"].get<uint64_t>() == 0xFFFFFFFFull) e["nBursts"] = 0;
    e["blendMode"] = in["blendMode"].get<uint64_t>() % 14;
    // The loader clamps with the Windows min/max macros, whose argument order
    // keeps -0.0 (a std::max would return +0.0).
    const float variance = F(in["randomRotationVariance"]);
    const float below = (1.0f < variance) ? 1.0f : variance;
    e["randomRotationVariance"] = J((0.0f > below) ? 0.0f : below);
    // An empty normal texture is not saved and reloads as the default.
    if (in["normalTexture"].get<std::string>().empty()) e["normalTexture"] = "p_particle_depth_master.tga";
    if (F(in["parentLinkStrength"]) == 0.0f) e["parentLinkStrength"] = J(0.0f);
    if (!in["randomRotation"].get<bool>()) e["randomRotationAverage"] = J(0.0f);

    // The lifetime group (ParticleSystem::GROUP_LIFETIME) is derived on save.
    // The saved lifetime group's type is always 1, so only the speed and
    // position groups can carry an unloadable type.
    for (int g : {0, 2})
        if (in["groups"][g]["type"].get<uint64_t>() >= 5) { *reason = "a group type of 5 or more makes the file unloadable"; return false; }
    json& life = e["groups"][1];
    const float lifetime = F(in["lifetime"]);
    life["type"] = 1;
    life["minX"] = J(0.0f);
    life["maxX"] = J(0.0f);
    life["minY"] = J(lifetime * (1 - F(in["randomLifetimePerc"])));
    life["maxY"] = J(lifetime);
    life["minZ"] = J(0.0f);
    life["maxZ"] = J(0.0f);

    Keys keys[7];
    int interpolation[7];
    for (int i = 0; i < 7; i++)
    {
        if (!ProjectTrackKeys(in, i, &keys[i], reason)) return false;
        interpolation[i] = ClampInterpolation(in["tracks"][i]["interpolation"].get<long long>());
    }

    // The reader's re-lock pass, then chunk 0x0101's explicit unlocks.
    int source[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (source[i] == i && interpolation[i] == interpolation[j] && keys[i] == keys[j])
                source[j] = source[i];
    for (int j = 1; j < 4; j++)
        if (in["tracks"][j]["explicitUnlock"].get<bool>()) source[j] = j;

    for (int i = 0; i < 7; i++)
    {
        const int s = i < 4 ? source[i] : i;
        json& track = e["tracks"][i];
        track["source"] = s;
        track["interpolation"] = interpolation[s];
        track["keys"] = KeysJson(keys[s]);
        bool unlockedIdentical = false;
        if (i >= 1 && i < 4 && s == i)
            for (int k = 0; k < i; k++)
                if (interpolation[source[k]] == interpolation[i] && keys[source[k]] == keys[i])
                    unlockedIdentical = true;
        track["explicitUnlock"] = unlockedIdentical;
    }
    *out = e;
    return true;
}
} // namespace projection_detail

// Projects a whole `--tool inspect` dump. Returns false, with a reason, when
// the document's save would not load back.
inline bool ProjectPersisted(const nlohmann::json& in, nlohmann::json* out, std::string* reason)
{
    nlohmann::json result = in;
    nlohmann::json emitters = nlohmann::json::array();
    for (size_t i = 0; i < in["emitters"].size(); i++)
    {
        nlohmann::json projected;
        if (!projection_detail::ProjectEmitter(in["emitters"][i], i, &projected, reason))
        {
            *reason = "emitters[" + std::to_string(i) + "]: " + *reason;
            return false;
        }
        emitters.push_back(projected);
    }
    result["emitters"] = emitters;
    *out = result;
    return true;
}

} // namespace particle_json

#endif // PERSISTED_PROJECTION_H
