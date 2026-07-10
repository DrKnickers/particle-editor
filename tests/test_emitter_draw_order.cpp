// Unit test for ComputeEmitterDrawOrder (src/EmitterDrawOrder.h) — the pure
// draw-order decision behind ParticleSystemInstance::RenderByRank (#574).
//
// Pins the fix: emitters draw in stable ascending AUTHORED-RANK order within a
// heat/normal pass, so a lazily-spawned child (appended at the tail of the
// spawn-order list regardless of rank) draws behind a higher-ranked sibling
// that spawned earlier — instead of always drawing last (on top) just because
// it was appended last. No engine/D3D dependency: the decision is pure data.

#include "EmitterDrawOrder.h"
#include <cstdio>
#include <vector>
#include <utility>

static int g_passed = 0;
static int g_failed = 0;

using Keys  = std::vector<std::pair<std::size_t, bool>>;
using Order = std::vector<std::size_t>;

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

// THE #574 repro. Spawn order = [root(rank 0), root(rank 3), child(rank 1)]:
// the child was appended last (lazily spawned), but its rank (1) sits between
// the two roots. The draw order must place it AHEAD of the rank-3 root, i.e.
// indices {0, 2, 1} (ranks 0, 1, 3) — so the child draws behind the later root
// instead of on top of it.
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

int main()
{
    test_empty();
    test_root_only_is_noop();
    test_child_moves_behind_higher_ranked_sibling();
    test_filters_by_heat();
    test_stable_among_equal_ranks();

    std::printf("test_emitter_draw_order: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
