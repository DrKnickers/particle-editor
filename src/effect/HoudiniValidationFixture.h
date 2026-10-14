#ifndef HOUDINI_VALIDATION_FIXTURE_H
#define HOUDINI_VALIDATION_FIXTURE_H

#include "ParticleSystem.h"
#include <string>

// --gen-houdini-validation-fixture <kind> <texture-name> <path>: one emitter
// that shows one generated texture as plainly as the engine allows, so a
// Houdini texture can be observed in isolation: one motionless particle at
// the origin (or lifted, for review in game), white tint. The normal texture
// keeps the emitter default: a file cannot record none (a missing chunk loads
// as the default), so the observed attestation also names that one. Window-free so
// tests/test_houdini_validation_fixture.cpp can pin it.
namespace houdinifixture {

enum class Kind { Puff, Ring, Fireball, Smoke4, Smoke8 };

struct Profile
{
    int           blendMode;    // ParticleSystem::BLEND_*
    unsigned long textureSize;  // atlas FRAME COUNT (grid side = floor(sqrt))
    float         lifetime;     // seconds
};

inline bool ParseKind(const std::wstring& text, Kind& kind)
{
    if (text == L"puff")     { kind = Kind::Puff;     return true; }
    if (text == L"ring")     { kind = Kind::Ring;     return true; }
    if (text == L"fireball") { kind = Kind::Fireball; return true; }
    if (text == L"smoke4")   { kind = Kind::Smoke4;   return true; }
    if (text == L"smoke8")   { kind = Kind::Smoke8;   return true; }
    return false;
}

inline const char* KindName(Kind kind)
{
    switch (kind)
    {
    case Kind::Puff:   return "puff";
    case Kind::Ring:   return "ring";
    case Kind::Smoke4: return "smoke4";
    case Kind::Smoke8: return "smoke8";
    default:           return "fireball";
    }
}

// Puff and the smoke flipbooks are smoke (straight alpha); ring and fireball
// are emissive. The 4x4 flipbooks hold each cell 0.25 s, 15 frames of the
// 60 Hz clock; the 8x8 smoke holds each 0.1 s, 6 frames, so its 64 cells
// play in 6.4 s.
inline Profile ProfileFor(Kind kind)
{
    switch (kind)
    {
    case Kind::Puff:   return { ParticleSystem::BLEND_TRANSPARENT, 1, 2.0f };
    case Kind::Ring:   return { ParticleSystem::BLEND_ADDITIVE, 1, 2.0f };
    case Kind::Smoke4: return { ParticleSystem::BLEND_TRANSPARENT, 16, 4.0f };
    case Kind::Smoke8: return { ParticleSystem::BLEND_TRANSPARENT, 64, 6.4f };
    default:           return { ParticleSystem::BLEND_ADDITIVE, 16, 4.0f };
    }
}

// A bare texture file name, resolved under Data\Art\Textures\ like the
// game's own: no folders, no drive, nothing a path could hide in.
inline bool IsTextureName(const std::string& name)
{
    if (name.size() < 5 || name.size() > 64) return false;
    for (const char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    std::string extension = name.substr(name.size() - 4);
    for (char& c : extension)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return name[0] != '.' && (extension == ".tga" || extension == ".dds");
}

// --gen-houdini-review-fixture: the same particle raised by its own scale, so
// a camera-facing sprite spawned on the ground (the game's click marker) is
// not cut in half by the terrain. Z is up.
const float kReviewLift = 40.0f;

inline void Configure(ParticleSystem::Emitter* e, Kind kind, const std::string& texture, float lift = 0)
{
    using PS = ParticleSystem;
    using Track = PS::Emitter::Track;
    const Profile profile = ProfileFor(kind);
    e->name = KindName(kind);
    e->colorTexture = texture;
    e->blendMode = profile.blendMode;
    e->textureSize = profile.textureSize;
    e->useBursts = true;
    e->nBursts = 1;
    e->nParticlesPerBurst = 1;
    e->nParticlesPerSecond = 0;
    e->initialDelay = 0;
    e->skipTime = 0;
    e->burstDelay = 10000;
    e->lifetime = profile.lifetime;
    e->gravity = 0;
    e->freezeTime = 0;
    e->randomLifetimePerc = 0;
    e->randomScalePerc = 0;
    e->isWeatherParticle = false;
    // Everything else that could tint, move, spin or clip it, set rather than
    // inherited so a changed default can't leak into the observation.
    for (float& c : e->randomColors) c = 0;
    for (float& a : e->acceleration) a = 0;
    e->doColorAddGrayscale = false;
    e->randomRotation = false;
    e->randomRotationDirection = false;
    e->randomRotationAverage = 0;
    e->randomRotationVariance = 0;
    e->inwardSpeed = 0;
    e->inwardAcceleration = 0;
    e->objectSpaceAcceleration = false;
    e->affectedByWind = false;
    e->linkToSystem = false;
    e->isHeatParticle = false;
    e->hasTail = false;
    e->isWorldOriented = false;
    e->noDepthTest = false;
    e->emitFromMesh = PS::EMIT_DISABLE;
    e->groundBehavior = 0;
    for (int group : { PS::GROUP_POSITION, PS::GROUP_SPEED })
    {
        e->groups[group].type = PS::GT_EXACT;
        e->groups[group].valX = 0;
        e->groups[group].valY = 0;
        e->groups[group].valZ = 0;
    }
    e->groups[PS::GROUP_POSITION].valZ = lift;
    // Defaults alias RGBA to red; separate the tracks before authoring them.
    for (int track : { PS::TRACK_RED_CHANNEL, PS::TRACK_GREEN_CHANNEL, PS::TRACK_BLUE_CHANNEL,
                       PS::TRACK_ALPHA_CHANNEL, PS::TRACK_SCALE, PS::TRACK_INDEX,
                       PS::TRACK_ROTATION_SPEED })
    {
        e->tracks[track] = &e->trackContents[track];
        e->tracks[track]->interpolation = track == PS::TRACK_INDEX ? Track::IT_STEP : Track::IT_LINEAR;
        e->tracks[track]->keys.clear();
    }
    for (int track : { PS::TRACK_RED_CHANNEL, PS::TRACK_GREEN_CHANNEL, PS::TRACK_BLUE_CHANNEL,
                       PS::TRACK_ALPHA_CHANNEL })
    {
        e->tracks[track]->keys.insert(Track::Key(0, 1.0f));
        e->tracks[track]->keys.insert(Track::Key(100, 1.0f));
    }
    e->tracks[PS::TRACK_SCALE]->keys.insert(Track::Key(0, 40.0f));
    e->tracks[PS::TRACK_SCALE]->keys.insert(Track::Key(100, 40.0f));
    e->tracks[PS::TRACK_ROTATION_SPEED]->keys.insert(Track::Key(0, 0.0f));
    e->tracks[PS::TRACK_ROTATION_SPEED]->keys.insert(Track::Key(100, 0.0f));
    // Frame k fills the k-th equal slice of life. The cursor only passes a key
    // once relTime is strictly beyond it, so a key exactly on a cell start
    // would show frame k-1 on the clock tick that lands there (every cell
    // start does, at 60 Hz). Each key therefore sits half a 60 Hz tick early.
    // The closing key keeps the last frame to the end.
    const float lead = 100.0f * (0.5f / 60.0f) / profile.lifetime;
    for (unsigned long k = 0; k < profile.textureSize; ++k)
    {
        const float start = 100.0f * static_cast<float>(k) / static_cast<float>(profile.textureSize);
        e->tracks[PS::TRACK_INDEX]->keys.insert(Track::Key(k == 0 ? 0.0f : start - lead, static_cast<float>(k)));
    }
    e->tracks[PS::TRACK_INDEX]->keys.insert(
        Track::Key(100, static_cast<float>(profile.textureSize - 1)));
}

} // namespace houdinifixture

#endif
