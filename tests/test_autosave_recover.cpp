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

    // ---- filename classification (audit an-audit-finding) ---------------------------
    //
    // An interrupted write leaves `autosave-<pid>-recent.alo.tmp`. The
    // classifier compared the whole tail EXACTLY against the three tier
    // suffixes, so that name matched none of them and the scan skipped it
    // entirely — a crashed session's temp was neither offered for recovery nor
    // swept, and they accumulated across crashes forever.
    using Autosave::ClassifyAutosaveName;
    {
        const auto recent = ClassifyAutosaveName(L"autosave-1234-recent.alo");
        CHECK(recent.pid == 1234);
        CHECK(recent.isRecent && !recent.isStable && !recent.isMeta && !recent.isTmp);

        const auto stable = ClassifyAutosaveName(L"autosave-77-stable.alo");
        CHECK(stable.pid == 77 && stable.isStable && !stable.isTmp);

        const auto meta = ClassifyAutosaveName(L"autosave-5.meta");
        CHECK(meta.pid == 5 && meta.isMeta && !meta.isTmp);

        // THE REGRESSION: recognised, attributed to its PID, and flagged as a
        // temp. pid != 0 is what lets the scan reach it at all; isTmp is what
        // keeps it out of the recovery candidates.
        const auto tmp = ClassifyAutosaveName(L"autosave-1234-recent.alo.tmp");
        CHECK(tmp.pid == 1234);
        CHECK(tmp.isTmp);
        CHECK(tmp.isRecent);   // still tier-attributed, just not recoverable

        const auto tmpStable = ClassifyAutosaveName(L"autosave-88-stable.alo.tmp");
        CHECK(tmpStable.pid == 88 && tmpStable.isStable && tmpStable.isTmp);

        // Case-insensitive, like every other comparison here.
        const auto upper = ClassifyAutosaveName(L"AUTOSAVE-9-RECENT.ALO.TMP");
        CHECK(upper.pid == 9 && upper.isRecent && upper.isTmp);

        // Not ours — must stay pid 0 so the scan skips them. A stray ".tmp"
        // whose stem isn't a tier must NOT become a deletion candidate just
        // because it sits in the autosave directory.
        CHECK(ClassifyAutosaveName(L"autosave-1234-bogus.alo").pid == 0);
        CHECK(ClassifyAutosaveName(L"autosave-1234-bogus.alo.tmp").pid == 0);
        CHECK(ClassifyAutosaveName(L"autosave-0-recent.alo").pid == 0);   // pid 0 is not a pid
        CHECK(ClassifyAutosaveName(L"notautosave-1-recent.alo").pid == 0);
        CHECK(ClassifyAutosaveName(L"autosave--recent.alo").pid == 0);    // no digits
        CHECK(ClassifyAutosaveName(L".tmp").pid == 0);
        CHECK(ClassifyAutosaveName(nullptr).pid == 0);
    }

    if (g_fail == 0) std::printf("=== AutosaveRecover: ALL PASS ===\n");
    else             std::printf("=== AutosaveRecover: %d FAILURE(S) ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
