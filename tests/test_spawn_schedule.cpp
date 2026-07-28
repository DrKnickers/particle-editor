// Regression test for the spawn-rate reconcile clamp (src/SpawnSchedule.h,
// 2026-07 audit an-audit-finding).
//
// onParticleSystemChanged recomputes m_spawnDelay on a rate edit but the next
// spawn was scheduled against the OLD delay. Raise the rate on a slow emitter
// and nothing happens until that old delay elapses — the slider looks dead.
//
// The clamp has three obligations and only one of them is the headline. The
// tempting one-liner `scheduled = now + newDelay` fixes the headline and breaks
// the other two: it DEFERS a spawn that was already due sooner, and it drags an
// OVERDUE spawn forward, silently swallowing a round the catch-up loop owned.
// Both wrong versions are pinned below as overreach guards.
//
// Header-only; see tests/build_test_spawn_schedule.bat.

#include "SpawnSchedule.h"

#include <cstdio>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_spawn_schedule\n");

    // --- 1. THE HEADLINE. Emitter was at 1/s (delay 1.0) and last spawned at
    // t=10, so the next is due at 11. User drags the rate to 1000/s
    // (delay 0.001) at t=10.2. The next spawn must be pulled in to 10.201, not
    // left sitting at 11 — that 0.8 s of dead slider is the defect.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(11.0f, 10.2f, 0.001f);
        CHECK(got < 11.0f,       "rate increase pulls the scheduled spawn IN");
        CHECK(got == 10.201f,    "pulled in to exactly now + newDelay");
    }

    // --- 2. OVERREACH GUARD: never DEFER. The emitter is due in 0.05 s and the
    // user lowers the rate (new delay 2.0). The scheduled spawn was already
    // earned — leave it. `now + newDelay` would push it from 10.05 out to 12.0
    // and drop a round.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.05f, 10.0f, 2.0f);
        CHECK(got == 10.05f, "rate DECREASE does not defer an already-due spawn (overreach guard)");
    }

    // --- 3. OVERREACH GUARD: never drag an OVERDUE spawn forward. Scheduled at
    // 9.5, now is 10.0 — the emitter is behind and Update's catch-up loop owns
    // it. `now + newDelay` would rewrite 9.5 to 10.001 and swallow the round.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(9.5f, 10.0f, 0.001f);
        CHECK(got == 9.5f, "an OVERDUE spawn is left in the past (overreach guard)");
    }

    // --- 4. Exactly at the boundary: already equal to now + newDelay is not
    // "too far out", so it must pass through untouched rather than be rewritten
    // to the same value by a different route (a `>=` here would be harmless
    // today but is the wrong predicate).
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.001f, 10.0f, 0.001f);
        CHECK(got == 10.001f, "scheduled exactly at the new limit is unchanged");
    }

    // --- 5. An unchanged rate is a no-op for a spawn scheduled consistently
    // with it — the common case, since onParticleSystemChanged(-1) fires for
    // every property edit, not just rate edits.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.5f, 10.0f, 0.5f);
        CHECK(got == 10.5f, "unrelated property edit does not disturb the schedule");
    }

    // --- 6. A zero delay (burst emitters clamp to 0.01, but a 0 must not
    // produce a time BEFORE now) still yields now, never a negative offset.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(50.0f, 10.0f, 0.0f);
        CHECK(got == 10.0f, "zero delay clamps to now, not before it");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
