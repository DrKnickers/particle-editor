// Regression test for the WM_CLOSE veto decision (src/CloseDecision.h).
//
// A native frame-X / Alt-F4 must prompt-to-save only for a REAL interactive
// session with unsaved work. Ephemeral runs (--drive / --capture) and
// --test-host runs have no user to prompt and must close cleanly. The data-loss
// blocker was vetoing (or not vetoing) on the wrong combination. ShouldVetoClose
// must return true on EXACTLY one of the eight (dirty, ephemeral, testHost)
// combinations: dirty && !ephemeral && !testHost. Header-only; see
// tests/build_test_close_guard.bat.

#include "CloseDecision.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_close_guard\n");

    // Full truth table over (dirty, ephemeral, testHost). Veto iff the lone
    // interactive-with-unsaved-work case.
    struct Row { bool dirty, ephemeral, testHost, expect; const char* label; };
    const Row rows[] = {
        // dirty  ephem  test   expect
        { false, false, false, false, "clean, interactive            -> no veto" },
        { false, false, true,  false, "clean, test-host              -> no veto" },
        { false, true,  false, false, "clean, ephemeral              -> no veto" },
        { false, true,  true,  false, "clean, ephemeral+test-host    -> no veto" },
        { true,  false, false, true,  "DIRTY, interactive            -> VETO (the one)" },
        { true,  false, true,  false, "dirty, test-host              -> no veto (no user)" },
        { true,  true,  false, false, "dirty, ephemeral              -> no veto (no user)" },
        { true,  true,  true,  false, "dirty, ephemeral+test-host    -> no veto (no user)" },
    };

    int vetoCount = 0;
    for (const Row& r : rows)
    {
        bool got = ShouldVetoClose(r.dirty, r.ephemeral, r.testHost);
        CHECK(got == r.expect, r.label);
        if (got) ++vetoCount;
    }

    // Reinforce the "EXACTLY one" invariant independent of the per-row checks.
    CHECK(vetoCount == 1, "exactly one of the eight combinations vetoes");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
