// Regression test for UndoStack::EditorAux — the side-band
// reference-object transform that rides on every undo snapshot so the
// reference-object gizmo / Reset / spinners undo on the same Ctrl+Z timeline
// as particle edits.
//
// Drives UndoStack directly (no BridgeDispatcher) and asserts the stack-level
// contract the host wiring depends on:
//   1. aux round-trips through Undo/Redo (A -> B -> Undo=A -> Redo=B)
//   2. aux is carried by a "particle edit" capture, so undoing it does NOT
//      yank the reference object
//   3. a coalesced PRE-mutation burst keeps the SESSION-START aux, not the
//      last pre-fold value (the bug the design review caught)
//   4. different coalesce keys do NOT fold (per-field granularity)
//   5. a defaulted capture (no aux arg) yields {0,0,0} (legacy back-compat)
//   6. the shipped byte budget is 256 MiB
//   7. only the explicit immediate-pair policy retains two over-budget entries
//
// Build/run: tests\build_test_undo_aux.bat ; tests\test_undo_aux.exe
// Expects the final line "=== undo aux: ALL PASS ===" and exit code 0.

#include "UndoStack.h"
#include "RefTransformUndoKey.h"
#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include <cstdio>
#include <cmath>

// Link stub — see test_emitter_reorder.cpp. ~Emitter calls this; the tests
// never register an EmitterInstance, so a no-op keeps the link graph to the
// pure data-model TUs and off the rendering stack.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: %s\n", __LINE__, msg); } \
} while (0)

// A non-coalescing capture (key 0) of a tiny system carrying the given aux.
static UndoStack::EditorAux mkAux(float px, float py, float pz,
                                  float rx, float ry, float rz)
{
    UndoStack::EditorAux a;
    a.refPos[0] = px; a.refPos[1] = py; a.refPos[2] = pz;
    a.refRot[0] = rx; a.refRot[1] = ry; a.refRot[2] = rz;
    return a;
}

static bool auxEq(const UndoStack::EditorAux& a, const UndoStack::EditorAux& b)
{
    for (int i = 0; i < 3; ++i)
        if (a.refPos[i] != b.refPos[i] || a.refRot[i] != b.refRot[i]) return false;
    return true;
}

// 1. aux round-trips through Undo/Redo.
static void test_round_trip(ParticleSystem& ps)
{
    UndoStack u;
    const UndoStack::EditorAux A = mkAux(1, 2, 3, 10, 20, 30);
    const UndoStack::EditorAux B = mkAux(4, 5, 6, 40, 50, 60);
    u.Capture(ps, SIZE_MAX, 0, A);   // baseline (ref at A)
    u.Capture(ps, SIZE_MAX, 0, B);   // ref moved to B

    const std::vector<char>* buf = nullptr; size_t sel = 0;
    UndoStack::EditorAux got;
    CHECK(u.Undo(&buf, &sel, &got), "Undo should succeed (depth 2)");
    CHECK(auxEq(got, A), "Undo returns the pre-move (A) aux");
    CHECK(u.Redo(&buf, &sel, &got), "Redo should succeed");
    CHECK(auxEq(got, B), "Redo returns the moved-to (B) aux");
}

// 2. A particle-edit capture carries the current aux, so undoing the edit
//    leaves the reference object where it is. The two snapshots carry
//    DIFFERENT particle content (one emitter vs two) but the SAME ref aux, so
//    the assertion proves the ref rode along independent of the particle edit.
static void test_particle_undo_preserves_ref()
{
    ParticleSystem ps;
    ps.addRootEmitter();
    UndoStack u;
    const UndoStack::EditorAux A = mkAux(7, 8, 9, 0, 0, 0);
    u.Capture(ps, SIZE_MAX, 0, A);   // ref at A, 1 emitter
    ps.addRootEmitter();             // a real "particle edit" (now 2 emitters)
    u.Capture(ps, SIZE_MAX, 0, A);   // ref unchanged (still A), different PS content

    const std::vector<char>* buf = nullptr; size_t sel = 0;
    UndoStack::EditorAux got;
    CHECK(u.Undo(&buf, &sel, &got), "Undo the particle edit");
    CHECK(auxEq(got, A), "ref transform preserved (object not yanked) after particle undo");
}

// 6. The per-field coalesce key (RefTransformUndoKey.h) — the only new
//    non-trivial pure logic in the bridge handler. Guards the no-op -> no
//    entry case and the per-field granularity the design depends on.
static void test_ref_transform_key()
{
    const float zero[6] = {0,0,0,0,0,0};
    // No change -> key 0 -> caller pushes no undo entry (no-op Reset).
    {
        const float same[6] = {1,2,3,4,5,6};
        CHECK(RefTransformCoalesceKey(same, same) == 0, "identical transform -> key 0 (no capture)");
    }
    // A single changed component -> a nonzero key with bit 31 set (never 0).
    {
        const float curr[6] = {0,0,0,0,0,0};
        const float incX[6] = {1,0,0,0,0,0};
        const DWORD kX = RefTransformCoalesceKey(incX, curr);
        CHECK(kX != 0 && (kX & 0x80000000u) != 0, "changed posX -> nonzero key, bit 31 set");
        // Same component, different magnitude -> SAME key (a spinner burst folds).
        const float incX2[6] = {99,0,0,0,0,0};
        CHECK(RefTransformCoalesceKey(incX2, curr) == kX, "same axis, any value -> same key (burst folds)");
        // A different component -> a DIFFERENT key (separate undo step).
        const float incY[6] = {0,1,0,0,0,0};
        CHECK(RefTransformCoalesceKey(incY, curr) != kX, "posX vs posY -> different keys (separate steps)");
        // Two components vs one -> different (XOR fold is set-sensitive).
        const float incXY[6] = {1,1,0,0,0,0};
        CHECK(RefTransformCoalesceKey(incXY, curr) != kX, "{posX,posY} differs from {posX}");
        // Discriminator occupies bits 0..15 (won't collide with an emitter id).
        CHECK((kX & 0xFFFFu) == 0xFEF0u, "discriminator in low 16 bits");
    }
    (void)zero;
}

// 3. A coalesced PRE-mutation burst keeps the SESSION-START aux.
static void test_coalesce_keeps_session_start(ParticleSystem& ps)
{
    UndoStack u;
    const DWORD K = 0x80001234u;
    const UndoStack::EditorAux Z = mkAux(0, 0, 0, 0, 0, 0);   // prior baseline
    const UndoStack::EditorAux A = mkAux(1, 0, 0, 0, 0, 0);   // burst session-start (PRE)
    const UndoStack::EditorAux B = mkAux(2, 0, 0, 0, 0, 0);   // mid-burst (should be skipped)
    const UndoStack::EditorAux C = mkAux(3, 0, 0, 0, 0, 0);   // end-burst (should be skipped)
    const UndoStack::EditorAux F = mkAux(4, 0, 0, 0, 0, 0);   // live final (auto-cap simulation)

    u.Capture(ps, SIZE_MAX, 0, Z);                 // [Z]                 cursor 1
    u.CapturePreCoalesced(ps, SIZE_MAX, K, A);     // push -> [Z,A]       cursor 2
    u.CapturePreCoalesced(ps, SIZE_MAX, K, B);     // SKIP (keep A)       cursor 2
    u.CapturePreCoalesced(ps, SIZE_MAX, K, C);     // SKIP (keep A)       cursor 2
    CHECK(u.Depth() == 2, "burst coalesced to a single entry");
    u.Capture(ps, SIZE_MAX, 0, F);                 // auto-cap live -> [Z,A,F] cursor 3

    const std::vector<char>* buf = nullptr; size_t sel = 0;
    UndoStack::EditorAux got;
    CHECK(u.Undo(&buf, &sel, &got), "Undo the whole burst");
    CHECK(auxEq(got, A), "Undo returns the SESSION-START aux (A), not the last pre-fold value");
}

// 4. Different coalesce keys do NOT fold (per-field granularity).
static void test_different_keys_dont_fold(ParticleSystem& ps)
{
    UndoStack u;
    u.Capture(ps, SIZE_MAX, 0, mkAux(0,0,0,0,0,0));        // [base]
    u.CapturePreCoalesced(ps, SIZE_MAX, 0x80001111u, mkAux(1,0,0,0,0,0));  // posX
    u.CapturePreCoalesced(ps, SIZE_MAX, 0x80002222u, mkAux(1,1,0,0,0,0));  // posY (different key)
    CHECK(u.Depth() == 3, "two different-key edits stay two separate undo steps");
}

// 5. A defaulted capture (no aux arg) yields {0,0,0} (legacy back-compat).
static void test_default_aux_is_zero(ParticleSystem& ps)
{
    UndoStack u;
    u.Capture(ps, SIZE_MAX, 0);   // no aux arg
    u.Capture(ps, SIZE_MAX, 0);   // no aux arg

    const std::vector<char>* buf = nullptr; size_t sel = 0;
    UndoStack::EditorAux got = mkAux(9,9,9,9,9,9);   // poison
    CHECK(u.Undo(&buf, &sel, &got), "Undo with defaulted captures");
    CHECK(auxEq(got, mkAux(0,0,0,0,0,0)), "defaulted aux is {0,0,0}");
}

// 6. The constructor default itself is part of the production memory policy.
// Compare the actual member against a literal value rather than the class
// constant, so changing MAX_TOTAL_BYTES cannot make this test pass tautologically.
static void test_default_byte_budget_and_override_guard(ParticleSystem& ps)
{
    UndoStack u;
    CHECK(u.MaxTotalBytes() == 256u * 1024u * 1024u,
          "default byte budget: actual stack limit is exactly 256 MiB");
    CHECK(u.SetMaxTotalBytesForTesting(1),
          "test byte budget: empty synchronized stack accepts an override");

    u.Capture(ps, SIZE_MAX, 0);
    CHECK(!u.SetMaxTotalBytesForTesting(2),
          "test byte budget: non-empty/live-ahead stack refuses reconfiguration");

    u.Clear();
    CHECK(u.SetMaxTotalBytesForTesting(2),
          "test byte budget: Clear re-enables safe reconfiguration");
}

// 7. The auto-capture exception retains exactly the immediate PRE + LIVE pair
// even when each real snapshot exceeds the entire injected budget. A following
// ordinary capture must leave the exception and resume one-entry enforcement.
static void test_byte_budget_preserves_only_immediate_pair(ParticleSystem& ps)
{
    UndoStack u(1);
    const UndoStack::EditorAux pre  = mkAux(1, 2, 3, 4, 5, 6);
    const UndoStack::EditorAux live = mkAux(7, 8, 9, 10, 11, 12);
    const UndoStack::EditorAux next = mkAux(13, 14, 15, 16, 17, 18);

    u.Capture(ps, SIZE_MAX, 0, pre);
    u.Capture(ps, SIZE_MAX, 0, live,
              UndoStack::BudgetRetention::PreserveImmediatePair);

    CHECK(u.Depth() == 2,
          "immediate-pair budget: PRE and LIVE both survive byte pressure");
    CHECK(u.Cursor() == 2,
          "immediate-pair budget: cursor remains at the two-entry tip");
    CHECK(u.TotalBytes() > u.MaxTotalBytes(),
          "immediate-pair budget: exception is soft, not a doubled hard cap");
    CHECK(u.CanUndo(),
          "immediate-pair budget: PRE remains reachable");

    const std::vector<char>* buf = nullptr; size_t sel = 0;
    UndoStack::EditorAux got;
    CHECK(u.Undo(&buf, &sel, &got),
          "immediate-pair budget: Undo reaches PRE");
    CHECK(auxEq(got, pre),
          "immediate-pair budget: Undo restores PRE aux");
    CHECK(u.Redo(&buf, &sel, &got),
          "immediate-pair budget: Redo reaches LIVE");
    CHECK(auxEq(got, live),
          "immediate-pair budget: Redo restores LIVE aux");

    u.Capture(ps, SIZE_MAX, 0, next);
    CHECK(u.Depth() == 1,
          "immediate-pair budget: following normal capture retains only one");
    CHECK(u.Cursor() == 1,
          "immediate-pair budget: normal eviction keeps cursor at surviving tip");
    CHECK(!u.CanUndo(),
          "immediate-pair budget: exception does not leak into normal captures");
}

// 8. Aggregate byte budget (2026-07 audit).
//
// MAX_ENTRIES caps HOW MANY snapshots are resident and says nothing about how
// large they are. Snapshot size scales with emitter and track-key count, both
// of which run to five figures under the .alo caps, so 100 entries had no
// memory ceiling at all.
//
// The budget is injected rather than reached: building 256 MB of genuine
// snapshots costs ~400 MB resident, which is a bad trade for a cap designed
// never to fire in normal use. The eviction path exercised here is the same
// code the production default runs.
static void test_byte_budget_evicts_oldest()
{
    ParticleSystem big;
    for (int i = 0; i < 40; ++i) big.addRootEmitter();
    const size_t one = UndoStack::Serialize(big).size();
    CHECK(one > 0, "byte budget: the fixture serializes to something");

    // Room for ~3 snapshots, so pushing 12 must evict.
    UndoStack u(one * 3 + one / 2);
    for (int i = 0; i < 12; ++i) u.Capture(big, SIZE_MAX, 0);

    // The discriminating pair. Without the budget these read ~12*one and 12 --
    // MAX_ENTRIES (100) never fires at this depth, so the entry cap alone
    // leaves every snapshot resident.
    CHECK(u.TotalBytes() <= one * 3 + one / 2,
          "byte budget: total snapshot bytes stay within the budget");
    CHECK(u.Depth() < 12, "byte budget: oldest entries were evicted");
    CHECK(u.Depth() >= 3, "byte budget: eviction is not over-eager");

    // Cursor accounting must survive eviction: the entries still present have
    // to be reachable, or the stack silently loses the user's undo.
    CHECK(u.Cursor() == u.Depth(), "byte budget: cursor tracks the surviving tip");
    const std::vector<char>* buf = nullptr; size_t sel = 0;
    CHECK(u.Undo(&buf, &sel), "byte budget: undo still works after eviction");
    CHECK(buf != nullptr && buf->size() == one,
          "byte budget: the restored snapshot is a whole entry, not a truncated one");
}

// 9. A single snapshot larger than the entire budget must still be kept -- a
// stack that evicted its only entry could not undo the edit that just
// happened, which is worse than being over budget.
static void test_byte_budget_keeps_last_entry()
{
    ParticleSystem big;
    for (int i = 0; i < 40; ++i) big.addRootEmitter();

    UndoStack u(1);   // one byte: smaller than any real snapshot
    for (int i = 0; i < 3; ++i) u.Capture(big, SIZE_MAX, 0);

    CHECK(u.Depth() == 1, "over-budget single entry: exactly one entry is kept");
    CHECK(u.TotalBytes() > 1,
          "over-budget single entry: kept even though it exceeds the budget");
}

int main()
{
    // A minimal but real ParticleSystem so Serialize/Deserialize exercise the
    // genuine write/read round-trip the UndoStack performs internally.
    ParticleSystem ps;
    ps.addRootEmitter();

    test_round_trip(ps);
    test_particle_undo_preserves_ref();
    test_coalesce_keeps_session_start(ps);
    test_different_keys_dont_fold(ps);
    test_default_aux_is_zero(ps);
    test_ref_transform_key();
    test_default_byte_budget_and_override_guard(ps);
    test_byte_budget_preserves_only_immediate_pair(ps);
    test_byte_budget_evicts_oldest();
    test_byte_budget_keeps_last_entry();

    std::printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed == 0) { std::printf("=== undo aux: ALL PASS ===\n"); return 0; }
    std::printf("=== undo aux: FAILED ===\n");
    return 1;
}
