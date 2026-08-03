// Audit fix C — regression test for the root-reorder spawn-field rewrite.
//
// Bug: reorderManyRootsToIndex / moveEmitterToRootIndex / moveEmitter
// rewrote a moved emitter's parent spawn-field (spawnDuringLife /
// spawnOnDeath) IN PLACE while comparing the already-half-rewritten live
// field against the emitter's old index. When one child's NEW index aliases
// a sibling's OLD index, the read-modify-write mis-fires and SWAPS the
// parent's life/death child slots — silently corrupting the .alo on save.
//
// These tests build the minimal aliasing repro (a parent with a life child
// and a death child, plus a spare root) and reorder it so a child's new
// index equals the other child's old index, then assert each slot still
// points at the SAME child emitter it did before the move. They exercise
// both bridge-reachable entry points (reorderManyRootsToIndex behind
// emitters/reorder-many, and moveEmitter behind Move Up/Down) since the
// rewrite loop is documented KEEP-IN-SYNC across all three functions.
//
// Build (from repo root, in a VS x64 dev shell) — see build-and-run at the
// bottom of this file's companion command; links only the data-model TUs.
// Run: expects "Results: N passed, 0 failed".

#include "ParticleSystem.h"
#include "ParticleSystemInstance.h"
#include <cstdio>
#include <vector>

// Link stub. ~Emitter calls ParticleSystemInstance::RemoveEmitter, whose real
// body lives in ParticleSystemInstance.cpp (D3D-coupled). These tests never
// register an EmitterInstance, so that call is never reached with live state;
// a no-op stub keeps the link graph to the pure data-model TUs and off the
// rendering stack.
void ParticleSystemInstance::RemoveEmitter(EmitterInstance*) {}

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: ASSERT_EQ(%s, %s) -> %zu != %zu\n", \
        __LINE__, #a, #b, (size_t)_a, (size_t)_b); } \
} while (0)

using Emitter = ParticleSystem::Emitter;

// Build: root P with life child C1 and death child C2, plus a spare root Q.
// m_emitters = [P, C1, C2, Q]; P.spawnDuringLife=1 (C1), P.spawnOnDeath=2 (C2).
// Pointers are stable across a reorder (the vector is permuted, emitters are
// not reallocated), so we compare each slot against the child's live index.
struct Repro { ParticleSystem ps; Emitter *P, *C1, *C2, *Q; };

static void buildRepro(Repro& r)
{
    r.P  = r.ps.addRootEmitter();
    r.C1 = r.ps.addLifetimeEmitter(r.P);   // P.spawnDuringLife = C1->index
    r.C2 = r.ps.addDeathEmitter(r.P);      // P.spawnOnDeath    = C2->index
    r.Q  = r.ps.addRootEmitter();
    // Sanity: the repro is wired as expected before any reorder.
    ASSERT_EQ(r.P->spawnDuringLife, r.C1->index);
    ASSERT_EQ(r.P->spawnOnDeath,    r.C2->index);
}

// reorder-many: drag P below Q. New layout [Q, P, C1, C2] aliases C2's new
// index (3) with... actually aliases C1.new(2)==C2.old(2): the exact trigger.
static void test_reorder_many_preserves_slots()
{
    Repro r; buildRepro(r);
    std::vector<Emitter*> sel = { r.P };
    std::vector<size_t> out;
    bool ok = r.ps.reorderManyRootsToIndex(sel, 2 /* after last root */, out);
    ASSERT_EQ(ok, true);
    // Slots must still point at the SAME children, not be swapped.
    ASSERT_EQ(r.P->spawnDuringLife, r.C1->index);  // life still C1
    ASSERT_EQ(r.P->spawnOnDeath,    r.C2->index);   // death still C2
}

// moveEmitter (adjacent swap, +1 = down): same aliasing, Move Up/Down path.
static void test_move_emitter_preserves_slots()
{
    Repro r; buildRepro(r);
    bool ok = r.ps.moveEmitter(r.P, +1 /* down, past Q */);
    ASSERT_EQ(ok, true);
    ASSERT_EQ(r.P->spawnDuringLife, r.C1->index);
    ASSERT_EQ(r.P->spawnOnDeath,    r.C2->index);
}

// moveEmitterToRootIndex (absolute target): the third KEEP-IN-SYNC copy.
static void test_move_to_root_index_preserves_slots()
{
    Repro r; buildRepro(r);
    bool ok = r.ps.moveEmitterToRootIndex(r.P, 2 /* after last root */);
    ASSERT_EQ(ok, true);
    ASSERT_EQ(r.P->spawnDuringLife, r.C1->index);
    ASSERT_EQ(r.P->spawnOnDeath,    r.C2->index);
}

int main()
{
    test_reorder_many_preserves_slots();
    test_move_emitter_preserves_slots();
    test_move_to_root_index_preserves_slots();
    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
