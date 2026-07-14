// Unit test for EmitterDrawOrder.h — the pure draw-order decision behind
// ParticleSystemInstance::RenderByRank (#574, #609).
//
// Two pieces:
//   * ComputeAuthoredDrawKeys — post-order keys over the authored emitter tree:
//     a child draws BEHIND its parent (parent on top, #609), siblings/roots by
//     rank (#574).
//   * ComputeEmitterDrawOrder — heat-filtered stable sort of live instances by
//     their draw key.
// No engine/D3D dependency: the decision is pure data.

#include "EmitterDrawOrder.h"
#include <cstdio>
#include <vector>
#include <utility>

static int g_passed = 0;
static int g_failed = 0;

using Keys   = std::vector<std::pair<std::size_t, bool>>;
using Order  = std::vector<std::size_t>;
using Ranks  = std::vector<std::size_t>;
static const std::size_t ROOT = static_cast<std::size_t>(-1);

#define ASSERT_ORDER(got, ...) do {                                            \
    Order _exp = __VA_ARGS__;                                                  \
    Order _got = (got);                                                        \
    if (_got == _exp) { ++g_passed; }                                          \
    else {                                                                     \
        ++g_failed;                                                            \
        std::printf("  FAIL line %d: order mismatch\n    expected:", __LINE__);\
        for (auto v : _exp) std::printf(" %zu", v);                            \
        std::printf("\n    got:     ");                                        \
        for (auto v : _got) std::printf(" %zu", v);                            \
        std::printf("\n");                                                     \
    }                                                                          \
} while (0)

// A pass over an empty instance list yields an empty draw order.
static void test_empty()
{
    ASSERT_ORDER(ComputeEmitterDrawOrder({}, false), Order{});
    ASSERT_ORDER(ComputeEmitterDrawOrder({}, true),  Order{});
}

// Root-only, already spawned in rank order: the draw order is unchanged
// (0,1,2) — this is why root-only golden fixtures render identically.
static void test_root_only_is_noop()
{
    Keys k = { {0,false}, {1,false}, {2,false} };
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 1, 2});
}

// The sort primitive orders instances by their draw key, regardless of spawn
// order. Spawn order = instances with keys [0, 3, 1] (e.g. one appended last but
// carrying a middle key): the draw order must be by ascending key → indices
// {0, 2, 1} (keys 0, 1, 3). ComputeAuthoredDrawKeys (below) is what turns the
// authored tree into these keys; here we pin only the key sort.
static void test_child_moves_behind_higher_ranked_sibling()
{
    Keys k = { {0,false}, {3,false}, {1,false} };
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 2, 1});
}

// Only emitters matching the requested pass are included; the other pass is
// disjoint. Heat + normal together must cover every index exactly once.
static void test_filters_by_heat()
{
    Keys k = { {0,false}, {1,true}, {2,false}, {3,true} };
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 2}); // normal pass
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, true),  Order{1, 3}); // heat pass
}

// Equal ranks keep spawn order (stable): the two rank-2 instances stay in their
// original 0-before-3 order, after the rank-1 instance.
static void test_stable_among_equal_ranks()
{
    Keys k = { {2,false}, {1,false}, {2,false} };
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{1, 0, 2});
}

// ─── ComputeAuthoredDrawKeys — post-order over the authored tree (#574, #609) ──

// An empty forest yields no keys.
static void test_keys_empty()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({}), Order{});
}

// Root-only: every emitter is its own root, so the post-order sequence equals
// rank order (keys 0,1,2). This is why root-only golden fixtures render
// identically — the draw key is a no-op re-labelling of the rank.
static void test_keys_root_only_is_identity()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, ROOT, ROOT }), Order{0, 1, 2});
}

// THE #609 fix. Parent = root rank 0; child = rank 1 whose parent is rank 0.
// Post-order visits the child first (key 0) then the parent (key 1): the child's
// key is LOWER, so it draws first (behind) and the parent draws last (on top) —
// the opposite of the pre-#609 pure-rank behaviour that drew the child on top.
static void test_keys_child_draws_behind_parent()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, 0 }), Order{1, 0});
}

// Depth > 1: root(0) -> child(1) -> grandchild(2). Deepest first: grandchild
// key 0, child key 1, root key 2 — the whole lineage stacks with the root on top.
static void test_keys_grandchild_post_order()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, 0, 1 }), Order{2, 1, 0});
}

// #574 preserved: a child whose rank sits between two roots still honours the
// tree. Roots rank 0 and 1; a child rank 2 parented to root 1. Post-order:
// root0 (key 0), then root1's subtree = child2 (key 1) before root1 (key 2).
// So the child draws behind its parent root1, and root0's key stays lowest.
static void test_keys_child_between_roots_follows_parent()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, ROOT, 1 }), Order{0, 2, 1});
}

// Two families interleave by root rank, each parent on top of its own child.
// root0, root1, child-of-0 = rank 2, child-of-1 = rank 3.
// Post-order: [child2, root0, child3, root1] → keys root0=1, root1=3, c2=0, c3=2.
static void test_keys_two_families_group_by_root()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, ROOT, 0, 1 }), Order{1, 3, 0, 2});
}

// Higher-ranked SIBLING still draws on top (rank order among siblings, #574):
// parent rank 0 with two children, life=rank 1 and death=rank 2. Post-order
// visits children ascending (child1 key 0, child2 key 1) then parent (key 2),
// so child2 draws over child1 and the parent over both.
static void test_keys_sibling_children_by_rank()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ ROOT, 0, 0 }), Order{2, 0, 1});
}

// Defensive: a 2-node cycle (each names the other as parent) reaches no root.
// Both are swept in rank order at the end so every rank still gets a distinct
// key and the downstream sort stays valid (no crash, no duplicate key).
static void test_keys_cycle_falls_back_to_rank()
{
    ASSERT_ORDER(ComputeAuthoredDrawKeys({ 1, 0 }), Order{0, 1});
}

// End-to-end through both pieces: the #609 parent/child, drawn via the keys.
// authored: parent rank 0 (root), child rank 1. drawKey = {1, 0}. Two live
// instances — parent (rank 0) and child (rank 1) — must draw child then parent.
static void test_end_to_end_parent_on_top()
{
    Ranks parentRank = { ROOT, 0 };
    Order dk = ComputeAuthoredDrawKeys(parentRank); // {1, 0}
    // Instance 0 = parent (rank 0, key dk[0]=1); instance 1 = child (rank 1, key dk[1]=0).
    Keys k = { { dk[0], false }, { dk[1], false } };
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{1, 0}); // child (key 0) first
}

int main()
{
    test_empty();
    test_root_only_is_noop();
    test_child_moves_behind_higher_ranked_sibling();
    test_filters_by_heat();
    test_stable_among_equal_ranks();

    test_keys_empty();
    test_keys_root_only_is_identity();
    test_keys_child_draws_behind_parent();
    test_keys_grandchild_post_order();
    test_keys_child_between_roots_follows_parent();
    test_keys_two_families_group_by_root();
    test_keys_sibling_children_by_rank();
    test_keys_cycle_falls_back_to_rank();
    test_end_to_end_parent_on_top();

    std::printf("test_emitter_draw_order: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
