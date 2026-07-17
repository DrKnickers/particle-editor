// Unit test for src/Autosave.h pure helpers: ShouldDeleteOrphan (Tranche B / finding
// #3) decides whether an orphan autosave session's files are consumed (deleted) after a
// recovery attempt; ShouldSuppressRecoveryPrompt decides whether check-recovery skips the
// prompt per host mode (regression pin for the --record/--drive contamination fix). We
// compile ONLY this TU — the non-inline
// Autosave functions are declarations we never call — so it builds standalone like
// tests/test_mod_layers.cpp (no link against Autosave.cpp / the engine).
#include <cstdio>
#include "Autosave.h"

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

int main()
{
    using Autosave::RecoverOutcome;
    using Autosave::ShouldDeleteOrphan;
    using Autosave::ShouldSuppressRecoveryPrompt;

    // Consume (delete) the orphan ONLY on a successful recover or an explicit discard.
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Recovered) == true);
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Discarded) == true);
    // NEVER on a failed load — the other tier / next launch must still be recoverable.
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Failed) == false);

    // check-recovery prompt suppression (args: testHost, ephemeral, hasCurrentFile).
    // REGRESSION PIN: automation mode (--record / --drive) must suppress the prompt
    // even with no document loaded — React runs the mount-time check BEFORE the
    // timeline opens its file, so a real orphan would otherwise render over every
    // captured frame. Dropping `ephemeral` from the condition re-breaks recorded clips.
    CHECK(ShouldSuppressRecoveryPrompt(/*testHost*/false, /*ephemeral*/true,  /*hasFile*/false) == true);
    // Harness capture: --test-host always suppresses.
    CHECK(ShouldSuppressRecoveryPrompt(true,  false, false) == true);
    // A document already loaded (CLI file / non-untitled) wins over recovery.
    CHECK(ShouldSuppressRecoveryPrompt(false, false, true)  == true);
    // Any mode set suppresses.
    CHECK(ShouldSuppressRecoveryPrompt(true,  true,  true)  == true);
    // The ONLY case that shows the prompt: a normal interactive, untitled session
    // with a real orphan on disk.
    CHECK(ShouldSuppressRecoveryPrompt(false, false, false) == false);

    if (g_fail == 0) std::printf("=== AutosaveRecover: ALL PASS ===\n");
    else             std::printf("=== AutosaveRecover: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
