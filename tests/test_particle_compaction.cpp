// Regression test for the EmitterInstance death-pass compaction helper.
//
// The primitive vector's order is draw order, so every kill pattern must compact
// survivors stably while keeping the parallel particle-index vector aligned.

#include "ParticleCompaction.h"
#include <cstdio>
#include <set>
#include <vector>

struct Primitive
{
    int id;
};

struct Particle
{
    int id;
    size_t m_indicesIndex;
};

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, msg) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; std::printf("  FAIL line %d: %s\n", __LINE__, msg); } \
} while (0)

static std::vector<int> survivorsFor(size_t count, const std::set<size_t>& kills)
{
    std::vector<int> survivors;
    for (size_t i = 0; i < count; ++i)
    {
        if (kills.find(i) == kills.end())
        {
            survivors.push_back(static_cast<int>(i));
        }
    }
    return survivors;
}

static void runCase(const char* name, size_t count, const std::set<size_t>& kills)
{
    std::vector<Primitive> primitives;
    std::vector<Particle> particles;
    std::vector<Particle*> particleIndex;
    primitives.reserve(count);
    particles.reserve(count);
    particleIndex.reserve(count);

    for (size_t i = 0; i < count; ++i)
    {
        primitives.push_back(Primitive{ static_cast<int>(i) });
        particles.push_back(Particle{ static_cast<int>(i), i });
        particleIndex.push_back(&particles.back());
    }

    const std::vector<int> expected = survivorsFor(count, kills);
    const size_t removed = CompactKilledParticleSlots(primitives, particleIndex, kills);

    CHECK(removed == kills.size(), name);
    CHECK(primitives.size() == expected.size(), name);
    CHECK(particleIndex.size() == expected.size(), name);

    for (size_t i = 0; i < expected.size(); ++i)
    {
        CHECK(primitives[i].id == expected[i], name);
        CHECK(particleIndex[i]->id == expected[i], name);
        CHECK(particleIndex[i]->m_indicesIndex == i, name);
    }
}

static void test_none()
{
    runCase("none", 6, {});
}

static void test_all()
{
    runCase("all", 6, {0, 1, 2, 3, 4, 5});
}

static void test_head_run()
{
    runCase("head-run", 7, {0, 1, 2});
}

static void test_tail_run()
{
    runCase("tail-run", 7, {4, 5, 6});
}

static void test_alternating()
{
    runCase("alternating", 8, {1, 3, 5, 7});
}

static void test_random_mid()
{
    runCase("random-mid", 10, {2, 4, 7});
}

int main()
{
    test_none();
    test_all();
    test_head_run();
    test_tail_run();
    test_alternating();
    test_random_mid();

    std::printf("\nResults: %d passed, %d failed\n", g_passed, g_failed);
    if (g_failed == 0) { std::printf("=== particle compaction: ALL PASS ===\n"); return 0; }
    std::printf("=== particle compaction: FAILED ===\n");
    return 1;
}
