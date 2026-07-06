// Unit test for the device-free parts of SpawnerDriver (src/SpawnerDriver.cpp):
// config clamping, burst-rate math, and the burst/interval state machine.
//
// Coverage, per the source:
//   - ClampSpawnerConfig against every hard cap (SpawnerDriver.cpp:64-81),
//     which also exercises the file-static Clamp/ClampInt/ClampVec helpers
//     (SpawnerDriver.cpp:41-60) at their boundaries -- they are in an anonymous
//     namespace, so this public wrapper is the only reachable surface.
//   - ComputeBurstsPerSec cycle math + the 1e-4 floor (SpawnerDriver.cpp:129-136)
//   - SetConfig clamps, and resets the schedule ONLY on mode/enable changes,
//     never on parameter tweaks (SpawnerDriver.cpp:91-127)
//   - Tick/Trigger NULL guards and dt sanitization (SpawnerDriver.cpp:145-156)
//   - Auto interval countdown -> burst start (SpawnerDriver.cpp:159-169)
//   - the live-instance cap dropping a burst (SpawnerDriver.cpp:187-193)
//   - the NULL-spawn hard-guard disabling the driver (SpawnerDriver.cpp:211-222)
//   - position stamping + jitter bounds, observed through the spawn call
//     (SpawnerDriver.cpp:195-197)
//
// The engine seam: SpawnerDriver only calls two NON-VIRTUAL Engine members
// (SpawnParticleSystem + ActiveSpawnerInstanceCount). engine.cpp is
// D3D-coupled, so this test provides those two definitions itself (the same
// link-stub idea as ParticleSystemInstance::RemoveEmitter in
// test_alo_roundtrip.cpp), records the calls, and returns NULL -- which is the
// documented gate-refusal path, so every burst also exercises the hard-guard.
// The Engine/ParticleSystem pointers handed to Tick/Trigger point at aligned
// static storage that is never constructed: the driver never dereferences the
// system, and the only engine members called are the non-virtual stubs below,
// whose bodies never touch `this`. No real Engine can exist device-free.
//
// NOT covered here (needs a real Engine + D3D, or a src change):
//   - multi-instance bursts / spacing pacing (a NULL spawn ends the burst)
//   - velocity/path-shape stamping on the instance (needs a live
//     ParticleSystemInstance; MarkSpawnerOwned/SetPathShape are inline and
//     virtual-dispatch on a fake instance would be UB)
//   - the file-static RandPhase helper.
//
// Standalone console exe; see tests/build_test_spawner_driver.bat.

#include "SpawnerDriver.h"

#include <cmath>
#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// ---- Engine link stubs (see header comment) --------------------------------
static int         g_spawnCalls  = 0;
static int         g_activeCount = 0;
static D3DXVECTOR3 g_lastSpawnPos(0, 0, 0);
static D3DXVECTOR3 g_lastSpawnVel(0, 0, 0);

ParticleSystemInstance* Engine::SpawnParticleSystem(const ParticleSystem&, Object3D* parent)
{
    ++g_spawnCalls;
    if (parent != NULL)
    {
        g_lastSpawnPos = parent->GetPosition();
        g_lastSpawnVel = parent->GetVelocity();
    }
    return NULL;   // gate-refusal path: SpawnerDriver.cpp:211-222 must disarm
}

int Engine::ActiveSpawnerInstanceCount() const
{
    return g_activeCount;
}

// Never-constructed, never-dereferenced stand-ins (see header comment).
// HAZARD: this stays safe only while the two stubbed Engine members
// (SpawnParticleSystem / ActiveSpawnerInstanceCount) remain NON-virtual —
// a virtual call here would read a garbage vtable from this raw storage
// instead of failing at link time. If either goes virtual, replace this
// with a real seam (interface or link-time stub object).
alignas(Engine) static unsigned char g_engineStorage[sizeof(Engine)];
static Engine* FakeEngine() { return reinterpret_cast<Engine*>(g_engineStorage); }
alignas(16) static unsigned char g_systemStorage[16];
static const ParticleSystem* FakeSystem()
{
    return reinterpret_cast<const ParticleSystem*>(g_systemStorage);
}

static bool feq(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main()
{
    std::printf("test_spawner_driver\n");

    // ---- A: ClampSpawnerConfig vs every hard cap (SpawnerDriver.cpp:64-81) --
    {
        SpawnerConfig c;
        c.burstSize         = 0;
        c.spacingSec        = -1.0f;
        c.intervalSec       = 61.0f;
        c.maxLifetimeSec    = 601.0f;
        c.position          = D3DXVECTOR3( 10001.0f, -10001.0f, 5.0f);
        c.velocity          = D3DXVECTOR3(-20000.0f,  0.0f,     10000.0f);
        c.jitterPosition    = D3DXVECTOR3( 99999.0f, -99999.0f, 0.0f);
        c.acceleration      = D3DXVECTOR3( 10000.5f, -10000.5f, 1.0f);
        c.squiggleAmplitude = D3DXVECTOR3( 12345.0f,  0.0f,    -12345.0f);
        c.squiggleFrequency = 20.5f;
        ClampSpawnerConfig(c);

        CHECK(c.burstSize == 1, "burstSize 0 clamps up to 1");
        CHECK(c.spacingSec == 0.0f, "negative spacing clamps to 0");
        CHECK(c.intervalSec == SpawnerDriver::MAX_INTERVAL_SEC, "interval 61 clamps to MAX_INTERVAL_SEC");
        CHECK(c.maxLifetimeSec == SpawnerDriver::MAX_LIFETIME_SEC, "lifetime 601 clamps to MAX_LIFETIME_SEC");
        CHECK(c.position.x ==  SpawnerDriver::JITTER_MAX
           && c.position.y == -SpawnerDriver::JITTER_MAX
           && c.position.z == 5.0f, "position clamps per-axis to +/-JITTER_MAX");
        CHECK(c.velocity.x == -SpawnerDriver::JITTER_MAX
           && c.velocity.z ==  SpawnerDriver::JITTER_MAX, "velocity clamps per-axis; boundary value passes");
        CHECK(c.jitterPosition.x == SpawnerDriver::JITTER_MAX
           && c.jitterPosition.y == -SpawnerDriver::JITTER_MAX, "jitter clamps per-axis");
        CHECK(c.acceleration.x == SpawnerDriver::JITTER_MAX
           && c.acceleration.z == 1.0f, "acceleration clamps per-axis");
        CHECK(c.squiggleAmplitude.x == SpawnerDriver::JITTER_MAX
           && c.squiggleAmplitude.z == -SpawnerDriver::JITTER_MAX, "squiggle amplitude clamps per-axis");
        CHECK(c.squiggleFrequency == SpawnerDriver::SQUIGGLE_FREQ_MAX, "squiggle frequency clamps to max");
    }
    {
        SpawnerConfig c;
        c.burstSize         = 11;
        c.squiggleFrequency = -1.0f;
        c.intervalSec       = -2.0f;
        c.mode              = static_cast<SpawnerConfig::Mode>(7);
        ClampSpawnerConfig(c);
        CHECK(c.burstSize == SpawnerDriver::MAX_BURST_SIZE, "burstSize 11 clamps down to MAX_BURST_SIZE");
        CHECK(c.squiggleFrequency == 0.0f, "negative squiggle frequency clamps to 0");
        CHECK(c.intervalSec == 0.0f, "negative interval clamps to 0");
        CHECK(c.mode == SpawnerConfig::Mode::Auto, "out-of-range mode coerces to Auto");
    }
    {
        SpawnerConfig c;   // in-range values must pass through untouched
        c.mode = SpawnerConfig::Mode::Manual;
        c.burstSize = 10; c.spacingSec = 10.0f; c.intervalSec = 60.0f;
        c.maxLifetimeSec = 600.0f; c.squiggleFrequency = 20.0f;
        ClampSpawnerConfig(c);
        CHECK(c.mode == SpawnerConfig::Mode::Manual && c.burstSize == 10
           && c.spacingSec == 10.0f && c.intervalSec == 60.0f
           && c.maxLifetimeSec == 600.0f && c.squiggleFrequency == 20.0f,
           "boundary values pass through unclamped");
    }

    // ---- B: ComputeBurstsPerSec (SpawnerDriver.cpp:129-136) -----------------
    {
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        CHECK(SpawnerDriver::ComputeBurstsPerSec(c) == 0.0f, "Manual mode -> 0 bursts/sec");

        c.mode = SpawnerConfig::Mode::Auto;
        c.burstSize = 1; c.spacingSec = 5.0f; c.intervalSec = 10.0f;
        CHECK(feq(SpawnerDriver::ComputeBurstsPerSec(c), 0.1f),
              "burstSize 1: spacing ignored, cycle == interval (1/10)");

        c.burstSize = 4; c.spacingSec = 2.0f; c.intervalSec = 10.0f;
        CHECK(feq(SpawnerDriver::ComputeBurstsPerSec(c), 1.0f / 16.0f),
              "cycle = (burstSize-1)*spacing + interval = 16 -> 1/16");

        c.burstSize = 1; c.spacingSec = 0.0f; c.intervalSec = 0.0f;
        CHECK(feq(SpawnerDriver::ComputeBurstsPerSec(c), 10000.0f),
              "zero cycle floors at 1e-4 -> 10000 bursts/sec");
    }

    // ---- C: SetConfig clamps what it stores ---------------------------------
    {
        SpawnerDriver d;
        SpawnerConfig c;
        c.burstSize = 99; c.intervalSec = 1000.0f;
        d.SetConfig(c);
        CHECK(d.GetConfig().burstSize == SpawnerDriver::MAX_BURST_SIZE
           && d.GetConfig().intervalSec == SpawnerDriver::MAX_INTERVAL_SEC,
           "SetConfig stores the clamped config");
    }

    // ---- D: NULL guards ------------------------------------------------------
    {
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = true; c.intervalSec = 0.0f;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Tick(0.5f, NULL, FakeEngine());
        d.Tick(0.5f, FakeSystem(), NULL);
        d.Tick(0.5f, NULL, NULL);
        CHECK(g_spawnCalls == before, "Tick with NULL system/engine never spawns");

        SpawnerDriver m;
        SpawnerConfig mc; mc.mode = SpawnerConfig::Mode::Manual;
        m.SetConfig(mc);
        m.Trigger(NULL, FakeEngine());
        m.Trigger(FakeSystem(), NULL);
        m.Tick(1.0f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "Trigger with NULL system/engine arms nothing");
    }

    // ---- E: Trigger is Manual-only (SpawnerDriver.cpp:147) ------------------
    {
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = false; c.intervalSec = 10.0f;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());
        d.Tick(1.0f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "Trigger in Auto mode is a no-op");
    }

    // ---- F: Auto interval countdown + NULL-spawn hard-guard -----------------
    {
        g_activeCount = 0;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = true; c.intervalSec = 1.0f;
        c.position = D3DXVECTOR3(4.0f, 8.0f, -16.0f);
        c.velocity = D3DXVECTOR3(1.0f, 2.0f, 3.0f);
        d.SetConfig(c);   // enable toggle schedules the first burst 1s out

        int before = g_spawnCalls;
        d.Tick(0.5f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "0.5s of a 1s interval: no burst yet");
        d.Tick(0.6f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "interval elapsed: exactly one spawn attempt");
        CHECK(g_lastSpawnPos.x == 4.0f && g_lastSpawnPos.y == 8.0f && g_lastSpawnPos.z == -16.0f,
              "spawn anchor stamped with cfg.position (no jitter configured)");
        CHECK(g_lastSpawnVel.x == 1.0f && g_lastSpawnVel.y == 2.0f && g_lastSpawnVel.z == 3.0f,
              "spawn anchor stamped with cfg.velocity");
        CHECK(d.GetConfig().enabled == false,
              "NULL spawn (gate refusal) latches the hard-guard: driver disarmed");
        d.Tick(5.0f, FakeSystem(), FakeEngine());
        d.Tick(5.0f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "disarmed driver never spawns again");
    }

    // ---- G: parameter tweak does NOT reset the interval timer ---------------
    // (SpawnerDriver.cpp:97-127: only mode/enable changes touch the schedule.)
    {
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = true; c.intervalSec = 1.0f;
        d.SetConfig(c);
        d.Tick(0.9f, FakeSystem(), FakeEngine());
        c.burstSize = 2;              // spinner tweak mid-countdown
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Tick(0.2f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1,
              "param tweak preserved the running countdown (0.9+0.2 crosses 1.0)");
    }

    // ---- H: re-enabling reschedules a FULL interval (SpawnerDriver.cpp:120-126)
    {
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = true; c.intervalSec = 1.0f;
        d.SetConfig(c);
        d.Tick(0.9f, FakeSystem(), FakeEngine());
        c.enabled = false; d.SetConfig(c);           // pause with 0.1s left
        c.enabled = true;  d.SetConfig(c);           // re-enable -> full interval again
        int before = g_spawnCalls;
        d.Tick(0.9f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "re-enable rescheduled a full interval (0.9 < 1.0)");
        d.Tick(0.2f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "burst fires one full interval after re-enable");
    }

    // ---- I: live-instance cap drops the burst (SpawnerDriver.cpp:187-193) ---
    {
        g_activeCount = SpawnerDriver::MAX_ACTIVE_INSTANCES;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "at the instance cap: burst dropped before any spawn call");
        g_activeCount = 0;
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "dropped burst is not queued (nothing fires after cap clears)");
        d.Trigger(FakeSystem(), FakeEngine());
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "a fresh Trigger after the cap clears spawns again");
    }

    // ---- J: dt sanitization (SpawnerDriver.cpp:156) --------------------------
    // dt <= 0 or dt > 1 is coerced to 1/60, so 30 huge ticks advance only 0.5s.
    {
        g_activeCount = 0;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Auto; c.enabled = true; c.intervalSec = 1.0f;
        d.SetConfig(c);
        int before = g_spawnCalls;
        for (int i = 0; i < 15; ++i) d.Tick(1000.0f, FakeSystem(), FakeEngine());
        for (int i = 0; i < 15; ++i) d.Tick(-5.0f,   FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "30 insane-dt ticks == 0.5s simulated: no burst");
        for (int i = 0; i < 40; ++i) d.Tick(0.0f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "70 sanitized ticks (~1.17s) fire exactly one burst");
    }

    // ---- K: jitter keeps spawns inside cfg.position +/- jitter --------------
    // JitterAxis/Jitter (SpawnerDriver.cpp:21-31) are file-static; their
    // contract is observable through the stamped anchor position.
    {
        g_activeCount = 0;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        c.position       = D3DXVECTOR3(100.0f, -50.0f, 0.0f);
        c.jitterPosition = D3DXVECTOR3(8.0f, 0.0f, 0.0f);
        d.SetConfig(c);

        bool inBounds = true, yzExact = true, varied = false;
        float firstX = 0.0f;
        for (int i = 0; i < 50; ++i)
        {
            int before = g_spawnCalls;
            d.Trigger(FakeSystem(), FakeEngine());
            d.Tick(0.016f, FakeSystem(), FakeEngine());
            if (g_spawnCalls != before + 1) { inBounds = false; break; }
            if (i == 0) firstX = g_lastSpawnPos.x;
            else if (g_lastSpawnPos.x != firstX) varied = true;
            if (std::fabs(g_lastSpawnPos.x - 100.0f) > 8.0f) inBounds = false;
            if (g_lastSpawnPos.y != -50.0f || g_lastSpawnPos.z != 0.0f) yzExact = false;
        }
        CHECK(inBounds, "50 jittered spawns all inside position.x +/- jitter.x");
        CHECK(yzExact, "zero-jitter axes stamped exactly");
        CHECK(varied, "jittered axis actually varies across spawns");
    }

    // ---- L: manual triggers arriving before the armed burst begins are
    // QUEUED, never dropped (SpawnerDriver.cpp Trigger/Tick pending-burst
    // path). Regression for the preview-overload spec flake: two
    // back-to-back bridge spawner/trigger requests can land inside one
    // render-frame window; the old mid-burst debounce swallowed the second
    // (it couldn't tell "armed, Tick hasn't run yet" from "mid-burst"), so
    // the third trigger placed instance #2 instead of refusing at #3 and
    // engine/overload/refused never fired. Genuine mid-burst re-triggers
    // (burst has begun emitting) stay debounced — not coverable here: the
    // NULL-spawn stub refuses the first instance, so a burst can never be
    // begun-and-still-firing (see "NOT covered" in the header).
    {
        // L1: the pre-begun queue survives a cap-dropped burst. At the
        // instance cap the first burst is dropped WITHOUT disarming; the
        // queued second burst must still fire once the cap clears. On the
        // pre-fix code the second Trigger was silently dropped and this
        // second Tick spawned nothing.
        g_activeCount = SpawnerDriver::MAX_ACTIVE_INSTANCES;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());   // arms burst 1
        d.Trigger(FakeSystem(), FakeEngine());   // pre-Tick: queues burst 2
        d.Tick(0.016f, FakeSystem(), FakeEngine());   // burst 1 cap-dropped
        CHECK(g_spawnCalls == before, "at the cap: no spawn call for the dropped burst");
        g_activeCount = 0;
        d.Tick(0.016f, FakeSystem(), FakeEngine());   // queued burst 2 fires
        CHECK(g_spawnCalls == before + 1,
              "a trigger queued before the burst began survives a cap-dropped burst");
    }
    {
        // L2: a gate refusal (NULL spawn) drops the queue along with the
        // disarm — queued bursts would refuse-and-clear again next Tick
        // (banner churn), mirroring the auto-mode churn stop.
        g_activeCount = 0;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());   // arms burst 1
        d.Trigger(FakeSystem(), FakeEngine());   // queues burst 2
        d.Tick(0.016f, FakeSystem(), FakeEngine());   // burst 1: refused -> disarm
        CHECK(g_spawnCalls == before + 1, "refused burst made exactly one spawn attempt");
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "gate refusal drops the queued burst too");
    }
    {
        // L3: NULL-guarded triggers never queue (mirrors test D for the
        // queue path — a rejected trigger must not leave a pending burst).
        g_activeCount = SpawnerDriver::MAX_ACTIVE_INSTANCES;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());   // arms burst 1
        d.Trigger(NULL, FakeEngine());           // rejected: must not queue
        d.Trigger(FakeSystem(), NULL);           // rejected: must not queue
        d.Tick(0.016f, FakeSystem(), FakeEngine());   // burst 1 cap-dropped
        g_activeCount = 0;
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "NULL-guarded triggers left nothing queued");
    }
    {
        // L4: CancelPending (spawner/stop + the host's refusal mirror)
        // drops BOTH the armed-but-unbegun burst and the queue; a fresh
        // Trigger afterwards still works (no lock-out).
        g_activeCount = 0;
        SpawnerDriver d;
        SpawnerConfig c;
        c.mode = SpawnerConfig::Mode::Manual;
        d.SetConfig(c);
        int before = g_spawnCalls;
        d.Trigger(FakeSystem(), FakeEngine());   // arms burst 1
        d.Trigger(FakeSystem(), FakeEngine());   // queues burst 2
        d.CancelPending();
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before, "CancelPending disarms the unbegun burst and the queue");
        d.Trigger(FakeSystem(), FakeEngine());
        d.Tick(0.016f, FakeSystem(), FakeEngine());
        CHECK(g_spawnCalls == before + 1, "a fresh Trigger after CancelPending spawns again");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
