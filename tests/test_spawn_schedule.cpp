// Regression test for the spawn-rate reconcile clamp (src/SpawnSchedule.h,
// 2026-07 audit E-LIVE-03).
//
// onParticleSystemChanged recomputes m_spawnDelay on a rate edit but the next
// spawn was scheduled against the OLD delay. Raise the rate on a slow emitter
// and nothing happens until that old delay elapses — the slider looks dead.
//
// The clamp has four obligations and only one of them is the headline. The
// tempting one-liner `scheduled = now + newDelay` fixes the headline and breaks
// the other two: it DEFERS a spawn that was already due sooner, and it drags an
// OVERDUE spawn forward, silently swallowing a round the catch-up loop owned.
// Applying the clamp before initialDelay expires is a third wrong version: an
// unrelated edit collapses an authored five-second wait to the steady-state
// delay. All three wrong directions are pinned below.
//
// The builder links no production TU, so the final block also pins the actual
// EmitterInstance call site and provenance transitions. See the build script.

#include "SpawnSchedule.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

static void CheckTime(SpawnTimeF got, SpawnTimeF expected, const char* msg)
{
    if (got == expected)
    {
        std::printf("  ok: %s\n", msg);
    }
    else
    {
        ++g_failed;
        std::printf("  FAIL: %s (got %.6f, expected %.6f)\n", msg, got, expected);
    }
}

static std::string ReadSource(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

int main()
{
    std::printf("test_spawn_schedule\n");

    // --- 1. THE REGRESSION. The first spawn is due at t=15 from initialDelay=5.
    // An unrelated property edit at t=10 recomputes the steady delay as 0.1 s,
    // but must leave the authored first-spawn schedule at 15. The broken
    // unconditional clamp returns exactly 10.1.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(15.0f, 10.0f, 0.1f, true);
        CheckTime(got, 15.0f, "initialDelay=5 survives an unrelated property edit");
        CHECK(got != 10.1f, "initialDelay is not collapsed to the wrong 10.1 s");
    }

    // --- 2. THE ORIGINAL HEADLINE + OVERREACH GUARD. Emitter was at 1/s
    // (delay 1.0) and last spawned at
    // t=10, so the next is due at 11. User drags the rate to 1000/s
    // (delay 0.001) at t=10.2. The next spawn must be pulled in to 10.201, not
    // left sitting at 11 — that 0.8 s of dead slider is the defect. Marking every
    // schedule as initial-delay-backed would return the wrong 11.0.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(11.0f, 10.2f, 0.001f, false);
        CHECK(got < 11.0f,       "rate increase pulls the scheduled spawn IN");
        CheckTime(got, 10.201f,  "steady-state schedule pulls in to now + newDelay");
    }

    // --- 3. OVERREACH GUARD: never DEFER. The emitter is due in 0.05 s and the
    // user lowers the rate (new delay 2.0). The scheduled spawn was already
    // earned — leave it. `now + newDelay` would push it from 10.05 out to 12.0
    // and drop a round.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.05f, 10.0f, 2.0f, false);
        CHECK(got == 10.05f, "rate DECREASE does not defer an already-due spawn (overreach guard)");
    }

    // --- 4. OVERREACH GUARD: never drag an OVERDUE spawn forward. Scheduled at
    // 9.5, now is 10.0 — the emitter is behind and Update's catch-up loop owns
    // it. `now + newDelay` would rewrite 9.5 to 10.001 and swallow the round.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(9.5f, 10.0f, 0.001f, false);
        CHECK(got == 9.5f, "an OVERDUE spawn is left in the past (overreach guard)");
    }

    // --- 5. Exactly at the boundary: already equal to now + newDelay is not
    // "too far out", so it must pass through untouched rather than be rewritten
    // to the same value by a different route (a `>=` here would be harmless
    // today but is the wrong predicate).
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.001f, 10.0f, 0.001f, false);
        CHECK(got == 10.001f, "scheduled exactly at the new limit is unchanged");
    }

    // --- 6. An unchanged rate is a no-op for a spawn scheduled consistently
    // with it — the common case, since onParticleSystemChanged(-1) fires for
    // every property edit, not just rate edits.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(10.5f, 10.0f, 0.5f, false);
        CHECK(got == 10.5f, "unrelated property edit does not disturb the schedule");
    }

    // --- 7. A zero delay (burst emitters clamp to 0.01, but a 0 must not
    // produce a time BEFORE now) still yields now, never a negative offset.
    {
        const SpawnTimeF got = ReconcileNextSpawnTime(50.0f, 10.0f, 0.0f, false);
        CHECK(got == 10.0f, "zero delay clamps to now, not before it");
    }

    // --- 8. PRODUCTION BINDING. This test deliberately remains lightweight,
    // but it must fail if EmitterInstance stops forwarding provenance or stops
    // transitioning an elapsed/dropped first round to steady state.
    {
        const std::filesystem::path repoRoot =
            std::filesystem::path(__FILE__).parent_path().parent_path();
        const std::string header = ReadSource(repoRoot / "src" / "EmitterInstance.h");
        const std::string source = ReadSource(repoRoot / "src" / "EmitterInstance.cpp");

        CHECK(!header.empty() && !source.empty(), "production schedule sources are readable");
        CHECK(std::regex_search(header, std::regex(
            R"(TimeF\s+m_nextSpawnTime\s*=\s*0\.0f\s*;)")),
            "m_nextSpawnTime is initialized before the constructor callback (R-2)");
        CHECK(std::regex_search(header, std::regex(
            R"(bool\s+m_nextSpawnUsesInitialDelay\s*=\s*true\s*;)")),
            "new instances start with initial-delay schedule provenance");
        CHECK(std::regex_search(source, std::regex(
            R"(ReconcileNextSpawnTime\s*\(\s*m_nextSpawnTime\s*,\s*GetTimeF\s*\(\s*\)\s*,\s*m_spawnDelay\s*,\s*m_nextSpawnUsesInitialDelay\s*\))")),
            "production reconcile call forwards initial-delay provenance");
        CHECK(std::regex_search(source, std::regex(
            R"(m_nextSpawnUsesInitialDelay\s*=\s*false\s*;\s*m_nextSpawnTime\s*=\s*currentTime\s*\+\s*GetSpawnDelay\s*\(\s*\))")),
            "a budget-dropped due round transitions to steady-state provenance");
        CHECK(std::regex_search(source, std::regex(
            R"(m_nextSpawnUsesInitialDelay\s*=\s*false\s*;\s*m_nextSpawnTime\s*=\s*spawnTime\s*\+\s*GetSpawnDelay\s*\(\s*\))")),
            "a processed spawn round transitions to steady-state provenance");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
