// Test for the land bombing-run planner (src/simulation/BombingRun.h, #970).
//
// Checks the V formation and its rotation to the run heading, where the run starts and
// how long it lasts, that every drop is inside its bomber's attack window, the drop
// cadence for single and multi-pulse volleys, and the stock-stat fallbacks.

#include "simulation/BombingRun.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
            std::printf("  ok: %s\n", msg);                                                                            \
        else                                                                                                           \
        {                                                                                                              \
            std::printf("  FAIL: %s\n", msg);                                                                          \
            ++g_fail;                                                                                                  \
        }                                                                                                              \
    } while (0)

static const float kTolerance = 1e-3f;

static bool Near(float a, float b, float tolerance = kTolerance)
{
    return std::fabs(a - b) <= tolerance;
}

static bool NearVec(const float a[3], float x, float y, float z)
{
    return Near(a[0], x) && Near(a[1], y) && Near(a[2], z);
}

static float Distance(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static int CountDrops(const BombingRunPlan& plan, int bomberIndex)
{
    int count = 0;
    for (const BombingRunDrop& drop : plan.drops)
        if (drop.bomberIndex == bomberIndex) ++count;
    return count;
}

static BomberStats StockStats()
{
    return WithStockBomberFallbacks(BomberStats());
}

int main()
{
    const float target[3]   = {100.0f, 50.0f, 0.0f};
    const float groundZ     = 0.0f;
    const float alongX[3]   = {1.0f, 0.0f, 0.0f};
    const float stockSpeed  = kStockBomberSpeedPerFrame * GAME_LOGIC_FPS * kLandSpeedMultiplier;
    const float stockReach  = kStockBomberAttackDistance;
    const float stockHeight = kStockBomberHoverOffset;

    std::printf("[fallbacks]\n");
    {
        const BomberStats stock = StockStats();
        CHECK(stock.speedPerFrame == kStockBomberSpeedPerFrame &&
                  stock.fireRechargeSec == kStockBomberFireRechargeSec &&
                  stock.firePulseCount == kStockBomberFirePulseCount &&
                  stock.firePulseDelaySec == kStockBomberFirePulseDelaySec &&
                  stock.hoverOffset == kStockBomberHoverOffset && stock.attackDistance == kStockBomberAttackDistance,
              "absent stats take the stock bomber's");
        BomberStats own;
        own.speedPerFrame       = 9.0f;
        own.attackDistance      = -1.0f;
        const BomberStats mixed = WithStockBomberFallbacks(own);
        CHECK(mixed.speedPerFrame == 9.0f && mixed.attackDistance == kStockBomberAttackDistance,
              "a bomber's own stat is kept; an unusable one falls back");
        CHECK(Near(BomberSpeedPerSec(stock), 280.8f), "stock speed: 6 per frame x 30 frames/s x 1.56 = 280.8 u/s");
    }

    std::printf("[heading]\n");
    {
        float       heading[3];
        const float none[3] = {0.0f, 0.0f, 0.0f};
        BombingRunHeading(none, heading);
        CHECK(NearVec(heading, 1.0f, 0.0f, 0.0f), "a zero aim heads along +X");
        const float straightDown[3] = {0.0f, 0.0f, -4.0f};
        BombingRunHeading(straightDown, heading);
        CHECK(NearVec(heading, 1.0f, 0.0f, 0.0f), "a vertical aim heads along +X");
        const float climbing[3] = {0.0f, 5.0f, 3.0f};
        BombingRunHeading(climbing, heading);
        CHECK(NearVec(heading, 0.0f, 1.0f, 0.0f), "the aim is flattened onto the ground plane");
    }

    std::printf("[formation]\n");
    {
        const BombingRunPlan plan = PlanBombingRun(BomberStats(), target, alongX, groundZ);
        CHECK(NearVec(plan.formationOffset[0], 0.0f, 0.0f, 0.0f), "the leader flies on the line");
        CHECK(NearVec(plan.formationOffset[1], -70.0f, 70.0f, 0.0f), "wingman 1 sits 70 back, 70 left");
        CHECK(NearVec(plan.formationOffset[2], -70.0f, -70.0f, 0.0f), "wingman 2 sits 70 back, 70 right");
        CHECK(NearVec(plan.markers[1], 30.0f, 120.0f, 0.0f), "each marker is the target plus its bomber's offset");

        const float          alongY[3] = {0.0f, 2.0f, 0.0f};
        const BombingRunPlan north     = PlanBombingRun(BomberStats(), target, alongY, groundZ);
        CHECK(NearVec(north.formationOffset[1], -70.0f, -70.0f, 0.0f) &&
                  NearVec(north.formationOffset[2], 70.0f, -70.0f, 0.0f),
              "the V rotates with the heading");
    }

    std::printf("[run extent]\n");
    {
        const BombingRunPlan plan     = PlanBombingRun(BomberStats(), target, alongX, groundZ);
        const float          approach = stockReach + stockSpeed * kRunLeadSeconds;
        CHECK(Near(plan.speedPerSec, stockSpeed), "the plan flies at the bomber's land speed");
        CHECK(NearVec(plan.leaderStart, target[0] - approach, target[1], groundZ + stockHeight),
              "the leader starts reach + 1 s of flight before the target, at hover height");
        CHECK(Near(plan.durationSec, (2.0f * approach + kWingmanBackOffset) / stockSpeed),
              "the run lasts until the trailing wingmen are as far past their markers");
        float leaderEnd[3];
        BomberPositionAt(plan, 0, plan.durationSec, leaderEnd);
        CHECK(leaderEnd[0] > target[0] + approach, "the leader ends past the target");
    }

    std::printf("[drops]\n");
    {
        const BombingRunPlan plan    = PlanBombingRun(BomberStats(), target, alongX, groundZ);
        bool                 inReach = true, sorted = true, onPath = true;
        for (size_t i = 0; i < plan.drops.size(); ++i)
        {
            const BombingRunDrop& drop = plan.drops[i];
            if (Distance(drop.pos, plan.markers[drop.bomberIndex]) > stockReach + kTolerance) inReach = false;
            if (i > 0 && drop.timeSec < plan.drops[i - 1].timeSec) sorted = false;
            float expected[3];
            BomberPositionAt(plan, drop.bomberIndex, drop.timeSec, expected);
            if (Distance(drop.pos, expected) > kTolerance) onPath = false;
        }
        CHECK(inReach, "every drop is within attack range of its own marker");
        CHECK(sorted, "drops are sorted by time");
        CHECK(onPath, "every drop is released at its bomber's position");

        // Window: the horizontal reach at hover height, flown at bomber speed; one bomb every recharge.
        const float windowSec = 2.0f * std::sqrt(stockReach * stockReach - stockHeight * stockHeight) / stockSpeed;
        const int   expected  = (int)std::floor(windowSec / kStockBomberFireRechargeSec + kTolerance) + 1;
        CHECK(CountDrops(plan, 0) == expected && CountDrops(plan, 1) == expected && CountDrops(plan, 2) == expected,
              "each bomber drops one bomb per recharge across its window (18 for stock stats)");
        CHECK(expected == 18, "the stock window holds 18 drops");
        CHECK(plan.drops.front().timeSec > 0.0f && plan.drops.back().timeSec < plan.durationSec,
              "drops fall inside the run");
    }

    std::printf("[volleys]\n");
    {
        BomberStats pulsed;
        pulsed.firePulseCount     = 2;
        pulsed.firePulseDelaySec  = 0.05f;
        pulsed.fireRechargeSec    = 0.5f;
        const BombingRunPlan plan = PlanBombingRun(pulsed, target, alongX, groundZ);
        std::vector<float>   leader;
        for (const BombingRunDrop& drop : plan.drops)
            if (drop.bomberIndex == 0) leader.push_back(drop.timeSec);
        CHECK(leader.size() >= 4, "a multi-pulse run drops several volleys");
        CHECK(leader.size() >= 4 && Near(leader[1] - leader[0], 0.05f) && Near(leader[2] - leader[1], 0.5f) &&
                  Near(leader[3] - leader[2], 0.05f),
              "pulses are pulse-delay apart; volleys are recharge apart after the last pulse");

        BomberStats tooHigh;
        tooHigh.hoverOffset = 600.0f;
        CHECK(PlanBombingRun(tooHigh, target, alongX, groundZ).drops.empty(),
              "a bomber flying above its attack reach drops nothing");
    }

    if (g_fail == 0)
        std::printf("=== BombingRun: ALL PASS ===\n");
    else
        std::printf("=== BombingRun: %d FAILED ===\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
