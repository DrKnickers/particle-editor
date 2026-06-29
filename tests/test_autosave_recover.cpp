// Unit test for src/Autosave.h pure recovery-outcome helper (Tranche B / finding #3).
// ShouldDeleteOrphan decides whether an orphan autosave session's files are consumed
// (deleted) after a recovery attempt. We compile ONLY this TU — the non-inline
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

    // Consume (delete) the orphan ONLY on a successful recover or an explicit discard.
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Recovered) == true);
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Discarded) == true);
    // NEVER on a failed load — the other tier / next launch must still be recoverable.
    CHECK(ShouldDeleteOrphan(RecoverOutcome::Failed) == false);

    if (g_fail == 0) std::printf("=== AutosaveRecover: ALL PASS ===\n");
    else             std::printf("=== AutosaveRecover: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
