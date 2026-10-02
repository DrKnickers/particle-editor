// Regression test for the WM_CLOSE veto decision (src/CloseDecision.h).
//
// A native frame-X / Alt-F4 must prompt-to-save only for a REAL interactive
// session with unsaved work. Ephemeral runs (--drive / --capture) and
// --test-host runs have no user to prompt and must close cleanly. The data-loss
// blocker was vetoing (or not vetoing) on the wrong combination. ShouldVetoClose
// must return true on EXACTLY one of the sixteen (dirty, ephemeral, testHost,
// webAlive) combinations: dirty && !ephemeral && !testHost && webAlive. The
// webAlive leg: with the web process dead, the page's Save/Discard/Cancel prompt
// can never answer, so a veto would leave the window unclosable. Header-only;
// see the test_close_guard entry in tests/native-tests.json.

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

    // Full truth table over (dirty, ephemeral, testHost) with a live web. Veto
    // iff the lone interactive-with-unsaved-work case.
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
        bool got = ShouldVetoClose(r.dirty, r.ephemeral, r.testHost, /*webAlive*/true);
        CHECK(got == r.expect, r.label);
        if (got) ++vetoCount;
    }

    // The same eight rows with the web process dead: nothing may veto, because
    // the page that would answer the prompt is gone.
    for (const Row& r : rows)
    {
        bool got = ShouldVetoClose(r.dirty, r.ephemeral, r.testHost, /*webAlive*/false);
        char label[96];
        std::snprintf(label, sizeof(label), "dead web: %s", r.label);
        CHECK(!got, r.expect ? "dead web: DIRTY, interactive  -> no veto (page can't answer)"
                             : label);
        if (got) ++vetoCount;
    }

    // Reinforce the "EXACTLY one" invariant independent of the per-row checks.
    CHECK(vetoCount == 1, "exactly one of the sixteen combinations vetoes");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
