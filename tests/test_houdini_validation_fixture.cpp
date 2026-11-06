// Pins effect/HoudiniValidationFixture.h: the one-emitter .alo that shows one
// generated texture, written by --gen-houdini-validation-fixture (and, lifted
// off the ground, by --gen-houdini-review-fixture). The Houdini
// validators read a texture through this fixture, so a drift here (a second
// particle, a tint, a misnumbered flipbook key) would make them observe the
// fixture instead of the texture. Section C round-trips it through the real
// save and load. See the test_houdini_validation_fixture entry in
// tests/native-tests.json.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "effect/HoudiniValidationFixture.h"
#include "effect/ParticleSystemIO.h"
#include "simulation/ParticleSystemInstance.h"

#include <cmath>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Link stub -- see test_particle_system_io.cpp. No EmitterInstance is
// registered here.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

using PS    = ParticleSystem;
using Track = PS::Emitter::Track;
using houdinifixture::Kind;

static const Kind kKinds[] = {Kind::Puff, Kind::Ring, Kind::Fireball, Kind::Smoke4, Kind::Smoke8, Kind::Fireball8};

// The value a STEP track holds at relTime (percent of life), as
// EmitterInstance computes it: the cursor starts on the first key and passes a
// later key only once relTime is STRICTLY beyond it (UpdateTrackCursors), and
// STEP returns cursor.prev->value.
static float StepAt(const Track& track, float relTime)
{
    float value = track.keys.begin()->value;
    for (auto it = std::next(track.keys.begin()); it != track.keys.end(); ++it)
        if (relTime > it->time) value = it->value;
    return value;
}

static bool Flat(const Track& track, float value)
{
    if (track.interpolation != Track::IT_LINEAR || track.keys.size() != 2) return false;
    for (const auto& key : track.keys)
        if (key.value != value) return false;
    return track.keys.begin()->time == 0 && track.keys.rbegin()->time == 100;
}

// Everything but the per-kind profile is identical across kinds.
static void CheckEmitter(const PS::Emitter& e, Kind kind, const std::string& texture, const char* label, float lift = 0)
{
    const houdinifixture::Profile profile = houdinifixture::ProfileFor(kind);
    std::printf(" %s (%s)\n", label, houdinifixture::KindName(kind));
    CHECK(e.name == houdinifixture::KindName(kind), "emitter is named for its kind");
    CHECK(e.colorTexture == texture && e.normalTexture == PS::Emitter().normalTexture,
          "the generated texture is the colour texture; the normal keeps the default");
    CHECK(e.blendMode == profile.blendMode && e.textureSize == profile.textureSize && e.lifetime == profile.lifetime,
          "blend mode, frame count and lifetime follow the profile");
    CHECK(e.useBursts && e.nBursts == 1 && e.nParticlesPerBurst == 1 && e.burstDelay >= 1000 && e.initialDelay == 0 &&
              e.skipTime == 0,
          "exactly one particle, spawned at time zero");
    CHECK(e.randomLifetimePerc == 0 && e.randomScalePerc == 0 && !e.randomRotation && !e.isWeatherParticle &&
              e.parent == nullptr && e.spawnDuringLife == static_cast<size_t>(-1) &&
              e.spawnOnDeath == static_cast<size_t>(-1),
          "no randomness and no linked emitters");
    bool still = e.gravity == 0 && e.acceleration[0] == 0 && e.acceleration[1] == 0 && e.acceleration[2] == 0;
    for (int group : {PS::GROUP_POSITION, PS::GROUP_SPEED})
    {
        const auto& g = e.groups[group];
        still         = still && g.type == PS::GT_EXACT && g.valX == 0 && g.valY == 0 &&
                        g.valZ == (group == PS::GROUP_POSITION ? lift : 0);
    }
    CHECK(still && e.inwardSpeed == 0 && e.inwardAcceleration == 0 && !e.affectedByWind && !e.linkToSystem &&
              e.emitFromMesh == PS::EMIT_DISABLE && e.groundBehavior == 0,
          lift == 0 ? "the particle sits motionless at the origin"
                    : "the particle sits motionless, lifted straight up");
    CHECK(e.randomColors[0] == 0 && e.randomColors[1] == 0 && e.randomColors[2] == 0 && e.randomColors[3] == 0 &&
              !e.doColorAddGrayscale && !e.isHeatParticle && !e.hasTail && !e.isWorldOriented && !e.noDepthTest &&
              !e.randomRotationDirection && e.randomRotationAverage == 0 && e.randomRotationVariance == 0,
          "nothing tints, spins, trails or reorients it");
    bool separate = true;
    for (int track = 0; track < PS::NUM_TRACKS; ++track)
        for (int other = 0; other < track; ++other) separate = separate && e.tracks[track] != e.tracks[other];
    CHECK(separate, "no track aliases another (defaults alias RGBA to red)");
    CHECK(Flat(*e.tracks[PS::TRACK_RED_CHANNEL], 1) && Flat(*e.tracks[PS::TRACK_GREEN_CHANNEL], 1) &&
              Flat(*e.tracks[PS::TRACK_BLUE_CHANNEL], 1) && Flat(*e.tracks[PS::TRACK_ALPHA_CHANNEL], 1),
          "tint is flat white at full alpha, so the texture is seen unmodulated");
    CHECK(Flat(*e.tracks[PS::TRACK_SCALE], 40), "scale is flat");
    CHECK(Flat(*e.tracks[PS::TRACK_ROTATION_SPEED], 0), "no spin");
    const Track& index = *e.tracks[PS::TRACK_INDEX];
    CHECK(index.interpolation == Track::IT_STEP && index.keys.size() == profile.textureSize + 1,
          "index is a STEP track with one key per frame plus a closing key");
    // Every tick of the 60 Hz clock across the life shows the frame for its
    // slice: tick f is in cell f / ticksPerCell (15 for 4x4, 6 for 8x8).
    const float frames       = static_cast<float>(profile.textureSize);
    const int   ticks        = static_cast<int>(profile.lifetime * 60.0f + 0.5f);
    const int   ticksPerCell = ticks / static_cast<int>(profile.textureSize);
    bool        cells        = ticksPerCell * static_cast<int>(profile.textureSize) == ticks;
    for (int f = 0; f <= ticks; ++f)
    {
        const float relTime  = 100.0f * (static_cast<float>(f) / 60.0f) / profile.lifetime;
        const int   expected = (std::min)(f / ticksPerCell, static_cast<int>(profile.textureSize) - 1);
        cells                = cells && std::floor(StepAt(index, relTime)) == static_cast<float>(expected);
    }
    CHECK(cells, "every 60 Hz tick shows its slice's frame, the last frame holding to death");
    // Half a tick of margin on both sides of every cell start.
    bool        margin   = true;
    const float halfTick = 100.0f * (0.5f / 60.0f) / profile.lifetime;
    for (unsigned long k = 1; k < profile.textureSize; ++k)
    {
        const float start = 100.0f * static_cast<float>(k) / frames;
        margin            = margin && StepAt(index, start - 1.01f * halfTick) == static_cast<float>(k - 1) &&
                            StepAt(index, start - 0.99f * halfTick) == static_cast<float>(k);
    }
    CHECK(margin, "each frame starts half a tick before its slice");
    float lastValue = -1;
    for (const auto& key : index.keys)
    {
        cells     = cells && key.value < frames && key.value >= lastValue;
        lastValue = key.value;
    }
    CHECK(cells, "index never leaves the atlas and never steps backwards");
}

static std::wstring TempAlo(const wchar_t* name)
{
    wchar_t     root[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, root);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(root) + L"pe_houdini_fixture_" + std::to_wstring(GetCurrentProcessId()) + L"_" + name;
}

int main()
{
    std::printf("test_houdini_validation_fixture\n");

    std::printf("A. kinds and texture names\n");
    Kind kind = Kind::Puff;
    CHECK(houdinifixture::ParseKind(L"puff", kind) && kind == Kind::Puff && houdinifixture::ParseKind(L"ring", kind) &&
              kind == Kind::Ring && houdinifixture::ParseKind(L"fireball", kind) && kind == Kind::Fireball &&
              houdinifixture::ParseKind(L"smoke4", kind) && kind == Kind::Smoke4 &&
              houdinifixture::ParseKind(L"smoke8", kind) && kind == Kind::Smoke8 &&
              houdinifixture::ParseKind(L"fireball8", kind) && kind == Kind::Fireball8,
          "the six kinds parse");
    bool named = true;
    for (Kind k : kKinds)
        named = named &&
                houdinifixture::ParseKind(
                    std::wstring(houdinifixture::KindName(k),
                                 houdinifixture::KindName(k) + std::strlen(houdinifixture::KindName(k))),
                    kind) &&
                kind == k;
    CHECK(named, "every kind's name parses back to it");
    for (const wchar_t* bad : {L"", L"Puff", L"puff ", L"weather", L"fire-ball", L"smoke", L"smoke2", L"fireball16"})
    {
        kind = Kind::Ring;
        CHECK(!houdinifixture::ParseKind(bad, kind) && kind == Kind::Ring, "an unknown kind is refused untouched");
    }
    CHECK(houdinifixture::ProfileFor(Kind::Puff).blendMode == PS::BLEND_TRANSPARENT &&
              houdinifixture::ProfileFor(Kind::Ring).blendMode == PS::BLEND_ADDITIVE &&
              houdinifixture::ProfileFor(Kind::Fireball).blendMode == PS::BLEND_ADDITIVE,
          "puff is alpha-blended smoke; ring and fireball are additive");
    CHECK(houdinifixture::ProfileFor(Kind::Puff).textureSize == 1 &&
              houdinifixture::ProfileFor(Kind::Ring).textureSize == 1 &&
              houdinifixture::ProfileFor(Kind::Fireball).textureSize == 16 &&
              houdinifixture::ProfileFor(Kind::Fireball).lifetime / 16 == 0.25f,
          "the fireball is an additive flipbook: 16 frames (4x4), 0.25 s each");
    CHECK(houdinifixture::ProfileFor(Kind::Smoke4).blendMode == PS::BLEND_TRANSPARENT &&
              houdinifixture::ProfileFor(Kind::Smoke4).textureSize == 16 &&
              houdinifixture::ProfileFor(Kind::Smoke4).lifetime / 16 == 0.25f,
          "smoke4 is an alpha-blended flipbook: 16 frames (4x4), 0.25 s each");
    CHECK(houdinifixture::ProfileFor(Kind::Smoke8).blendMode == PS::BLEND_TRANSPARENT &&
              houdinifixture::ProfileFor(Kind::Smoke8).textureSize == 64 &&
              static_cast<int>(houdinifixture::ProfileFor(Kind::Smoke8).lifetime * 60.0f + 0.5f) == 64 * 6,
          "smoke8 is an alpha-blended flipbook: 64 frames (8x8), 6 ticks of 60 Hz each");
    CHECK(houdinifixture::ProfileFor(Kind::Fireball8).blendMode == PS::BLEND_ADDITIVE &&
              houdinifixture::ProfileFor(Kind::Fireball8).textureSize == 64 &&
              houdinifixture::ProfileFor(Kind::Fireball8).lifetime == houdinifixture::ProfileFor(Kind::Smoke8).lifetime,
          "fireball8 is an additive flipbook timed as smoke8");
    for (const char* good :
         {"pe_puff.tga", "A.TGA", "x-1_y.2.dds", "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghij.tga"})
        CHECK(houdinifixture::IsTextureName(good), "a bare .tga/.dds name is accepted");
    for (const char* bad :
         {"", ".tga", "a.png", "a.tga ", "dir\\a.tga", "dir/a.tga", "C:a.tga", "..\\a.tga", ".hidden.tga", "a b.tga",
          "a.tga.png", "a%41.tga", "abcdefghijabcdefghijabcdefghijabcdefghijabcdefghijabcdefghijk.tga"})
        CHECK(!houdinifixture::IsTextureName(bad), "anything but a bare short .tga/.dds name is refused");

    std::printf("B. configured emitter\n");
    for (Kind k : kKinds)
    {
        PS    system;
        auto* e = system.addRootEmitter();
        houdinifixture::Configure(e, k, "pe_test.tga");
        CheckEmitter(*e, k, "pe_test.tga", "in memory");
    }

    std::printf("C. save and reload\n");
    for (Kind k : kKinds)
    {
        const std::string  kindName = houdinifixture::KindName(k);
        const std::wstring path     = TempAlo((std::wstring(kindName.begin(), kindName.end()) + L".alo").c_str());
        CHECK(!path.empty(), "temp path resolves");
        auto system = std::make_unique<PS>();
        system->setName(DeriveParticleSystemName(path));
        houdinifixture::Configure(system->addRootEmitter(), k, "PE_Gen_Fixture.dds");
        std::string error;
        const bool  saved = SaveParticleSystem(system.get(), path, &error);
        CHECK(saved, "fixture saves");
        auto loaded = saved ? LoadParticleSystem(path, &error) : nullptr;
        CHECK(loaded && loaded->getEmitters().size() == 1, "fixture reloads as one emitter");
        if (loaded && loaded->getEmitters().size() == 1)
            CheckEmitter(*loaded->getEmitters()[0], k, "PE_Gen_Fixture.dds", "reloaded");
        DeleteFileW(path.c_str());
    }

    // The game spawns the click marker on the ground and draws it facing the
    // camera, so the terrain hides whatever hangs below the spawn point. The
    // review fixture lifts the particle by its own scale and changes nothing else.
    std::printf("D. review fixture\n");
    CHECK(houdinifixture::kReviewLift == 40.0f, "the lift is the particle's scale");
    for (Kind k : kKinds)
    {
        const std::wstring path   = TempAlo(L"review.alo");
        auto               system = std::make_unique<PS>();
        system->setName(DeriveParticleSystemName(path));
        houdinifixture::Configure(system->addRootEmitter(), k, "pe_test.tga", houdinifixture::kReviewLift);
        CheckEmitter(*system->getEmitters()[0], k, "pe_test.tga", "in memory", houdinifixture::kReviewLift);
        std::string error;
        const bool  saved  = SaveParticleSystem(system.get(), path, &error);
        auto        loaded = saved ? LoadParticleSystem(path, &error) : nullptr;
        CHECK(loaded && loaded->getEmitters().size() == 1, "review fixture saves and reloads as one emitter");
        if (loaded && loaded->getEmitters().size() == 1)
            CheckEmitter(*loaded->getEmitters()[0], k, "pe_test.tga", "reloaded", houdinifixture::kReviewLift);
        DeleteFileW(path.c_str());
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
