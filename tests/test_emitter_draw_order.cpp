// Unit test for EmitterDrawOrder.h — the pure draw-order decision behind
// ParticleSystemInstance::RenderByRank: a heat-filtered stable sort of live
// emitter instances by their source emitter's authored rank, whatever the
// parent/child tree (the game's rule, #949).
// No engine/D3D dependency: the decision is pure data.

#include "simulation/EmitterDrawOrder.h"
#include <cstdio>
#include <vector>
#include <utility>

static int g_passed = 0;
static int g_failed = 0;

using Keys  = std::vector<std::pair<std::size_t, bool>>;
using Order = std::vector<std::size_t>;

#define ASSERT_ORDER(got, ...)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        Order _exp = __VA_ARGS__;                                                                                      \
        Order _got = (got);                                                                                            \
        if (_got == _exp)                                                                                              \
        {                                                                                                              \
            ++g_passed;                                                                                                \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL line %d: order mismatch\n    expected:", __LINE__);                                    \
            for (auto v : _exp) std::printf(" %zu", v);                                                                \
            std::printf("\n    got:     ");                                                                            \
            for (auto v : _got) std::printf(" %zu", v);                                                                \
            std::printf("\n");                                                                                         \
        }                                                                                                              \
    } while (0)

// A pass over an empty instance list yields an empty draw order.
static void test_empty()
{
    ASSERT_ORDER(ComputeEmitterDrawOrder({}, false), Order{});
    ASSERT_ORDER(ComputeEmitterDrawOrder({}, true), Order{});
}

// Root-only, already spawned in rank order: the draw order is unchanged
// (0,1,2) — this is why root-only golden fixtures render identically.
static void test_root_only_is_noop()
{
    Keys k = {{0, false}, {1, false}, {2, false}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 1, 2});
}

// The sort orders instances by rank, regardless of spawn order. Spawn order =
// instances with ranks [0, 3, 1] (e.g. one appended last but carrying a middle
// rank): the draw order must be by ascending rank → indices {0, 2, 1}.
static void test_child_moves_behind_higher_ranked_sibling()
{
    Keys k = {{0, false}, {3, false}, {1, false}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 2, 1});
}

// Only emitters matching the requested pass are included; the other pass is
// disjoint. Heat + normal together must cover every index exactly once.
static void test_filters_by_heat()
{
    Keys k = {{0, false}, {1, true}, {2, false}, {3, true}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 2}); // normal pass
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, true), Order{1, 3});  // heat pass
}

// Equal ranks keep spawn order (stable): the two rank-2 instances stay in their
// original 0-before-3 order, after the rank-1 instance.
static void test_stable_among_equal_ranks()
{
    Keys k = {{2, false}, {1, false}, {2, false}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{1, 0, 2});
}

// A child ranked after its parent draws on top of it: the child spawns later and
// sits after the parent in spawn order, and its higher rank keeps it last.
// Instances: parent (rank 0), child (rank 1).
static void test_child_ranked_after_parent_draws_on_top()
{
    Keys k = {{0, false}, {1, false}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{0, 1});
}

// A child ranked BEFORE its parent draws under it, even though it spawned after
// the parent: rank, not spawn order or tree depth, decides.
// Instances in spawn order: parent (rank 1), child (rank 0).
static void test_child_ranked_before_parent_draws_under()
{
    Keys k = {{1, false}, {0, false}};
    ASSERT_ORDER(ComputeEmitterDrawOrder(k, false), Order{1, 0});
}

int main()
{
    test_empty();
    test_root_only_is_noop();
    test_child_moves_behind_higher_ranked_sibling();
    test_filters_by_heat();
    test_stable_among_equal_ranks();

    test_child_ranked_after_parent_draws_on_top();
    test_child_ranked_before_parent_draws_under();

    std::printf("test_emitter_draw_order: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
