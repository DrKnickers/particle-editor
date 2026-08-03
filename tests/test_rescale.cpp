// Unit test for DoRescaleEmitter (src/Rescale.cpp) -- the pure-IO emitter
// rescale used by `engine/action/rescale-system`.
//
// Table-driven over both emitter rate modes and both scale axes, with every
// expected value derived by hand from the source:
//   - time-field scaling            (Rescale.cpp:20-23)
//   - bursts-mode exact-integer flip to Particles/Second (Rescale.cpp:26-36):
//     burstDelay *= timeScale, then n = (int)(1/burstDelay*10000+0.5)/10000;
//     flips ONLY when (int)n == n && nParticlesPerBurst == 1 && nBursts == 0
//   - Particles/Second inverse (Rescale.cpp:38-53): n = pps/timeScale; a
//     non-integer n switches to infinite-burst mode (burstDelay = 1/n)
//   - speed group + speed scalars scale by 1/timeScale (Rescale.cpp:56-62)
//   - weather particles skip the whole rate/speed block (Rescale.cpp:24)
//   - size block (Rescale.cpp:66-89): scale-track VALUES, position + speed
//     groups, speed scalars, gravity and tailSize all scale by sizeScale
//   - scale == 1.0f is a strict no-op for its block (Rescale.cpp:18,66)
//
// All chosen inputs are exactly representable and scaled by powers of two, so
// float comparisons below are exact unless noted. Standalone console exe; see
// tests/build_test_rescale.bat.

#include "Rescale.h"
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"

#include <cstdio>

// Link stub -- see test_emitter_reorder.cpp / test_alo_roundtrip.cpp. ~Emitter
// calls ParticleSystemInstance::RemoveEmitter whose real body is D3D-coupled;
// no EmitterInstance is ever registered here, so a no-op keeps the link graph
// on the pure data-model TUs.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

using Emitter = ParticleSystem::Emitter;
using Group   = ParticleSystem::Emitter::Group;
using Track   = ParticleSystem::Emitter::Track;

// Fill the 13 float fields DoRescaleGroup touches (Rescale.cpp:5-14) with
// distinct exact values base+1 .. base+13, and stamp the fields it must NOT
// touch (type / sphereEdge / cylinderEdge) with sentinels.
static void fillGroup(Group& g, float base)
{
    g.cylinderHeight = base + 1;  g.cylinderRadius = base + 2;
    g.valX = base + 3;  g.valY = base + 4;  g.valZ = base + 5;
    g.minX = base + 6;  g.minY = base + 7;  g.minZ = base + 8;
    g.maxX = base + 9;  g.maxY = base + 10; g.maxZ = base + 11;
    g.sideLength   = base + 12;
    g.sphereRadius = base + 13;
    g.type = 3; g.sphereEdge = 1; g.cylinderEdge = 1;
}

// Assert every scaled field == (base+i)*k and the non-scaled fields kept
// their sentinels. k is a power of two in every caller, so == is exact.
static bool groupScaled(const Group& g, float base, float k)
{
    return g.cylinderHeight == (base + 1) * k && g.cylinderRadius == (base + 2) * k
        && g.valX == (base + 3) * k && g.valY == (base + 4) * k && g.valZ == (base + 5) * k
        && g.minX == (base + 6) * k && g.minY == (base + 7) * k && g.minZ == (base + 8) * k
        && g.maxX == (base + 9) * k && g.maxY == (base + 10) * k && g.maxZ == (base + 11) * k
        && g.sideLength   == (base + 12) * k
        && g.sphereRadius == (base + 13) * k
        && g.type == 3 && g.sphereEdge == 1 && g.cylinderEdge == 1;
}

// A default emitter with exact-binary time/speed fields and marked groups.
static void prime(Emitter& e)
{
    e.skipTime        = 0.5f;
    e.freezeTime      = 0.25f;
    e.initialDelay    = 0.75f;
    e.lifetime        = 1.5f;
    e.inwardSpeed     = 8.0f;
    e.inwardAcceleration = 2.0f;
    e.acceleration[0] = 2.0f; e.acceleration[1] = 4.0f; e.acceleration[2] = 6.0f;
    e.gravity         = 10.0f;
    e.tailSize        = 64.0f;
    fillGroup(e.groups[ParticleSystem::GROUP_SPEED],    16.0f);
    fillGroup(e.groups[ParticleSystem::GROUP_POSITION], 48.0f);
}

int main()
{
    std::printf("test_rescale\n");

    // ---- A: scale 1.0 / 1.0 is a strict no-op (Rescale.cpp:18,66) ----------
    {
        Emitter e; prime(e);
        e.nParticlesPerSecond = 7;
        DoRescaleEmitter(&e, 1.0f, 1.0f);
        CHECK(e.lifetime == 1.5f && e.skipTime == 0.5f && e.freezeTime == 0.25f
              && e.initialDelay == 0.75f, "no-op: time fields untouched");
        CHECK(!e.useBursts && e.nParticlesPerSecond == 7, "no-op: rate mode untouched");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_SPEED], 16.0f, 1.0f),
              "no-op: speed group untouched");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_POSITION], 48.0f, 1.0f),
              "no-op: position group untouched");
        CHECK(e.gravity == 10.0f && e.tailSize == 64.0f, "no-op: gravity/tailSize untouched");
    }

    // ---- B: timeScale=2, PPS mode, integer result stays PPS ----------------
    // n = 10 / 2 = 5.0; (int)5.0 == 5.0 -> stays Particles/Second (line 49-52).
    {
        Emitter e; prime(e);
        e.useBursts = false;
        e.nParticlesPerSecond = 10;
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(!e.useBursts && e.nParticlesPerSecond == 5,
              "timeScale=2: pps 10 -> 5, stays Particles/Second");
        CHECK(e.skipTime == 1.0f && e.freezeTime == 0.5f
              && e.initialDelay == 1.5f && e.lifetime == 3.0f,
              "timeScale=2: skip/freeze/delay/lifetime all x2");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_SPEED], 16.0f, 0.5f),
              "timeScale=2: speed group scaled by 1/2");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_POSITION], 48.0f, 1.0f),
              "timeScale=2: position group untouched by time scaling");
        CHECK(e.inwardSpeed == 4.0f && e.inwardAcceleration == 1.0f,
              "timeScale=2: inward speed/accel halved");
        CHECK(e.acceleration[0] == 1.0f && e.acceleration[1] == 2.0f
              && e.acceleration[2] == 3.0f, "timeScale=2: acceleration halved");
        CHECK(e.gravity == 5.0f, "timeScale=2: gravity halved");
        CHECK(e.tailSize == 64.0f, "timeScale=2: tailSize untouched (size-only field)");
    }

    // ---- C: timeScale=2, PPS mode, fractional result switches to bursts ----
    // n = 5 / 2 = 2.5; (int)2.5 != 2.5 -> infinite-burst mode (lines 40-48):
    // useBursts=true, nBursts=0, burstDelay = 1/2.5 = 0.4f, perBurst=1.
    {
        Emitter e; prime(e);
        e.useBursts = false;
        e.nParticlesPerSecond = 5;
        e.nBursts = 99;              // must be overwritten to 0
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(e.useBursts, "fractional pps: switched to bursts");
        CHECK(e.nBursts == 0, "fractional pps: nBursts forced 0 (infinite)");
        CHECK(e.nParticlesPerBurst == 1, "fractional pps: one particle per burst");
        CHECK(e.burstDelay == 0.4f, "fractional pps: burstDelay == 1/2.5 == 0.4f");
    }

    // ---- D: timeScale=2, bursts mode, exact-integer flip back to PPS -------
    // burstDelay 0.25*2 = 0.5; n = (int)(1/0.5*10000+0.5)/10000 = 20000/10000
    // = 2.0; (int)2.0 == 2.0 && perBurst==1 && nBursts==0 -> flip (lines 28-35).
    {
        Emitter e; prime(e);
        e.useBursts          = true;
        e.burstDelay         = 0.25f;
        e.nParticlesPerBurst = 1;
        e.nBursts            = 0;
        e.nParticlesPerSecond = 777;    // overwritten by the flip
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(!e.useBursts, "burst flip: switched back to Particles/Second");
        CHECK(e.nParticlesPerSecond == 2, "burst flip: pps == (int)(1/0.5) == 2");
        CHECK(e.burstDelay == 0.5f, "burst flip: burstDelay still scaled (0.25*2)");
    }

    // ---- E: same numbers but nParticlesPerBurst != 1 -> NO flip ------------
    {
        Emitter e; prime(e);
        e.useBursts          = true;
        e.burstDelay         = 0.25f;
        e.nParticlesPerBurst = 3;
        e.nBursts            = 0;
        e.nParticlesPerSecond = 777;
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(e.useBursts && e.burstDelay == 0.5f && e.nParticlesPerBurst == 3
              && e.nParticlesPerSecond == 777,
              "bursts: perBurst!=1 blocks the flip; delay still scaled");
    }

    // ---- F: same numbers but nBursts != 0 -> NO flip ------------------------
    {
        Emitter e; prime(e);
        e.useBursts          = true;
        e.burstDelay         = 0.25f;
        e.nParticlesPerBurst = 1;
        e.nBursts            = 5;
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(e.useBursts && e.burstDelay == 0.5f && e.nBursts == 5,
              "bursts: finite nBursts blocks the flip; delay still scaled");
    }

    // ---- G: bursts mode, non-integer 1/burstDelay -> NO flip ----------------
    // 0.15f*2 = 0.3f; n = (int)(33333.83)/10000 = 3.3333f; (int)3 != n.
    {
        Emitter e; prime(e);
        e.useBursts          = true;
        e.burstDelay         = 0.15f;
        e.nParticlesPerBurst = 1;
        e.nBursts            = 0;
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(e.useBursts && e.burstDelay == 0.15f * 2.0f,
              "bursts: non-integer 1/delay stays bursts, delay scaled");
    }

    // ---- H: weather particle skips the rate/speed block (line 24) ----------
    {
        Emitter e; prime(e);
        e.isWeatherParticle   = true;
        e.useBursts           = false;
        e.nParticlesPerSecond = 10;
        DoRescaleEmitter(&e, 2.0f, 1.0f);
        CHECK(e.nParticlesPerSecond == 10 && !e.useBursts,
              "weather: rate untouched by time scaling");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_SPEED], 16.0f, 1.0f),
              "weather: speed group untouched");
        CHECK(e.gravity == 10.0f && e.inwardSpeed == 8.0f,
              "weather: gravity/inwardSpeed untouched");
        CHECK(e.lifetime == 3.0f && e.skipTime == 1.0f,
              "weather: lifetime/skipTime still scaled x2 (lines 20-23)");
    }

    // ---- I: sizeScale=2 scales the scale track, groups and size scalars ----
    {
        Emitter e; prime(e);
        Track& scale = e.trackContents[ParticleSystem::TRACK_SCALE];
        scale.keys.clear();
        scale.keys.insert(Track::Key(  0.0f, 5.0f));
        scale.keys.insert(Track::Key( 50.0f, 7.0f));
        scale.keys.insert(Track::Key(100.0f, 9.0f));
        DoRescaleEmitter(&e, 1.0f, 2.0f);

        const Track::KeyMap& keys = e.tracks[ParticleSystem::TRACK_SCALE]->keys;
        CHECK(keys.size() == 3, "sizeScale=2: key count preserved");
        float expTime[3] = { 0.0f, 50.0f, 100.0f };
        float expVal[3]  = { 10.0f, 14.0f, 18.0f };
        int i = 0; bool keysOk = true;
        for (Track::KeyMap::const_iterator p = keys.begin(); p != keys.end(); ++p, ++i)
            keysOk = keysOk && p->time == expTime[i] && p->value == expVal[i];
        CHECK(keysOk && i == 3, "sizeScale=2: key VALUES x2, key TIMES unchanged");

        // Red track (tracks[0]) must be untouched -- only TRACK_SCALE rescales.
        const Track::KeyMap& red = e.tracks[0]->keys;
        CHECK(red.size() == 2 && red.begin()->value == 1.0f,
              "sizeScale=2: non-scale tracks untouched");

        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_POSITION], 48.0f, 2.0f),
              "sizeScale=2: position group x2");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_SPEED], 16.0f, 2.0f),
              "sizeScale=2: speed group x2");
        CHECK(e.inwardSpeed == 16.0f && e.inwardAcceleration == 4.0f,
              "sizeScale=2: inward speed/accel x2");
        CHECK(e.acceleration[0] == 4.0f && e.acceleration[1] == 8.0f
              && e.acceleration[2] == 12.0f, "sizeScale=2: acceleration x2");
        CHECK(e.gravity == 20.0f, "sizeScale=2: gravity x2");
        CHECK(e.tailSize == 128.0f, "sizeScale=2: tailSize x2");
        CHECK(e.lifetime == 1.5f && e.skipTime == 0.5f,
              "sizeScale=2: time fields untouched by size scaling");
    }

    // ---- J: timeScale=2 AND sizeScale=2 -> velocities invariant ------------
    // Speed group and speed scalars get x(1/2) from time then x2 from size;
    // position group only x2; time fields only x2.
    {
        Emitter e; prime(e);
        e.useBursts = false;
        e.nParticlesPerSecond = 10;
        DoRescaleEmitter(&e, 2.0f, 2.0f);
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_SPEED], 16.0f, 1.0f),
              "time+size x2: speed group net-unchanged");
        CHECK(e.inwardSpeed == 8.0f && e.gravity == 10.0f
              && e.acceleration[2] == 6.0f,
              "time+size x2: speed scalars net-unchanged");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_POSITION], 48.0f, 2.0f),
              "time+size x2: position group x2");
        CHECK(e.lifetime == 3.0f && e.tailSize == 128.0f,
              "time+size x2: lifetime x2, tailSize x2");
        CHECK(e.nParticlesPerSecond == 5, "time+size x2: pps halved");
    }

    // ---- K: extreme factors ------------------------------------------------
    // timeScale=1024 on pps=1: n = 1/1024 (exact), non-integer -> bursts with
    // burstDelay = 1/n = 1024 (exact). sizeScale=1/1024 shrinks exactly.
    {
        Emitter e; prime(e);
        e.useBursts = false;
        e.nParticlesPerSecond = 1;
        DoRescaleEmitter(&e, 1024.0f, 1.0f);
        CHECK(e.useBursts && e.nBursts == 0 && e.nParticlesPerBurst == 1,
              "timeScale=1024: pps=1 switches to infinite bursts");
        CHECK(e.burstDelay == 1024.0f, "timeScale=1024: burstDelay == 1024");
        CHECK(e.lifetime == 1.5f * 1024.0f, "timeScale=1024: lifetime x1024");
        CHECK(e.gravity == 10.0f / 1024.0f, "timeScale=1024: gravity /1024");
    }
    {
        Emitter e; prime(e);
        DoRescaleEmitter(&e, 1.0f, 1.0f / 1024.0f);
        CHECK(e.tailSize == 64.0f / 1024.0f, "sizeScale=1/1024: tailSize shrinks exactly");
        CHECK(groupScaled(e.groups[ParticleSystem::GROUP_POSITION], 48.0f, 1.0f / 1024.0f),
              "sizeScale=1/1024: position group shrinks exactly");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
