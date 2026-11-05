#pragma once

// A land bombing run as the game flies one (#970; decompiled StarWarsI.exe debug build,
// BombingRunManagerClass and LandBomberLocomotorBehaviorClass): a V of three bombers
// on a straight line through the target, each dropping on its own marker while that
// marker is within attack range. Owns the run's geometry and drop schedule only;
// SpawnerDriver spawns the drops as shots. The preview has no map, so the run starts
// and ends a short lead outside attack range instead of at the map edges.

#include "rendering/UnitConstants.h" // GAME_LOGIC_FPS

#include <algorithm>
#include <cmath>
#include <vector>

// One bomber type's stats, from its <LandBombingUnit> (GameObjectRef::bomber*).
struct BomberStats
{
    float speedPerFrame     = 0.0f; // raw <Max_Speed>, world units per logic frame
    float fireRechargeSec   = 0.0f; // <Projectile_Fire_Recharge_Seconds>, between volleys
    int   firePulseCount    = 0;    // <Projectile_Fire_Pulse_Count>, bombs per volley
    float firePulseDelaySec = 0.0f; // <Projectile_Fire_Pulse_Delay_Seconds>, between a volley's bombs
    float hoverOffset       = 0.0f; // <Hover_Offset>, flight height above the ground
    float attackDistance    = 0.0f; // <Targeting_Max_Attack_Distance>, 3D reach to the marker
};

// The stats all three stock bombers share (LandBombingRunUnits.xml). They stand in for
// a bomb no bomber names, and for any stat a bomber leaves absent or unusable.
constexpr float kStockBomberSpeedPerFrame     = 6.0f;
constexpr float kStockBomberFireRechargeSec   = 0.2f;
constexpr int   kStockBomberFirePulseCount    = 1;
constexpr float kStockBomberFirePulseDelaySec = 0.2f;
constexpr float kStockBomberHoverOffset       = 100.0f; // world units
constexpr float kStockBomberAttackDistance    = 500.0f; // world units

// Stock <Object_Max_Speed_Multiplier_Land> (GameConstants.xml). Bombers are live units,
// so their speed takes the land multiplier, unlike a projectile's.
constexpr float kLandSpeedMultiplier = 1.56f;

constexpr int kBombingRunBomberCount = 3;
// Wingmen sit 70 world units behind the leader and 70 to either side, in run-heading space
// (Get_Bomber_Path); bomber 0 is the leader.
constexpr float kWingmanBackOffset = 70.0f;
constexpr float kWingmanSideOffset = 70.0f;
// The run starts this long, at bomber speed, before the bombers come within attack range
// and ends the same distance past the target.
constexpr float kRunLeadSeconds = 1.0f;
// A ground-plane aim shorter than this has no direction, so the run heads along +X.
constexpr float kMinAimLength = 1e-6f;

// One bomb release: when, by which bomber, and from where.
struct BombingRunDrop
{
    float timeSec     = 0.0f;
    int   bomberIndex = 0;
    float pos[3]      = {0.0f, 0.0f, 0.0f};
};

struct BombingRunPlan
{
    float heading[3]     = {1.0f, 0.0f, 0.0f}; // unit run direction on the ground plane
    float leaderStart[3] = {0.0f, 0.0f, 0.0f};
    float speedPerSec    = 0.0f;
    float durationSec    = 0.0f;
    // Each bomber's world-space offset from the leader (bomber 0's is zero).
    float formationOffset[kBombingRunBomberCount][3] = {};
    // Each bomber's target marker: the target plus that bomber's formation offset.
    float                       markers[kBombingRunBomberCount][3] = {};
    std::vector<BombingRunDrop> drops; // sorted by time
};

// `stats` with every absent or unusable value replaced by the stock bomber's.
inline BomberStats WithStockBomberFallbacks(BomberStats stats)
{
    if (!(stats.speedPerFrame > 0.0f)) stats.speedPerFrame = kStockBomberSpeedPerFrame;
    if (!(stats.fireRechargeSec > 0.0f)) stats.fireRechargeSec = kStockBomberFireRechargeSec;
    if (stats.firePulseCount < 1) stats.firePulseCount = kStockBomberFirePulseCount;
    if (!(stats.firePulseDelaySec > 0.0f)) stats.firePulseDelaySec = kStockBomberFirePulseDelaySec;
    if (!(stats.hoverOffset > 0.0f)) stats.hoverOffset = kStockBomberHoverOffset;
    if (!(stats.attackDistance > 0.0f)) stats.attackDistance = kStockBomberAttackDistance;
    return stats;
}

// Bomber speed in world units per second, with the land multiplier.
inline float BomberSpeedPerSec(const BomberStats& stats)
{
    return stats.speedPerFrame * GAME_LOGIC_FPS * kLandSpeedMultiplier;
}

// Where `bomberIndex` is `timeSec` into the run.
inline void BomberPositionAt(const BombingRunPlan& plan, int bomberIndex, float timeSec, float out[3])
{
    const float travelled = plan.speedPerSec * timeSec;
    for (int axis = 0; axis < 3; ++axis)
        out[axis] = plan.leaderStart[axis] + plan.formationOffset[bomberIndex][axis] + plan.heading[axis] * travelled;
}

// The run's heading: `aim` flattened onto the ground plane, +X when that leaves nothing.
// The game heads the run along the line from the map centre to the target; with no map,
// the spawner's aim stands in for it.
inline void BombingRunHeading(const float aim[3], float out[3])
{
    const float length = std::sqrt(aim[0] * aim[0] + aim[1] * aim[1]);
    if (!(length > kMinAimLength))
    {
        out[0] = 1.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        return;
    }
    out[0] = aim[0] / length;
    out[1] = aim[1] / length;
    out[2] = 0.0f;
}

// Appends one bomber's drops; `stats` carries the stock fallbacks. It fires while its marker is within attack range
// (3D): a volley of firePulseCount bombs firePulseDelaySec apart, then fireRechargeSec before the next volley.
inline void AppendBomberDrops(const BombingRunPlan&        plan,
                              const BomberStats&           stats,
                              int                          bomberIndex,
                              float                        flightHeightAboveMarker,
                              std::vector<BombingRunDrop>& drops)
{
    const float reachSquared  = stats.attackDistance * stats.attackDistance;
    const float heightSquared = flightHeightAboveMarker * flightHeightAboveMarker;
    if (heightSquared > reachSquared) return; // flies too high ever to reach its marker

    // Each bomber's line passes straight over its own marker, so the in-range window is
    // centred on the moment it is overhead.
    const float halfWindowDistance = std::sqrt(reachSquared - heightSquared);
    float       start[3];
    BomberPositionAt(plan, bomberIndex, 0.0f, start);
    float alongToMarker = 0.0f;
    for (int axis = 0; axis < 3; ++axis)
        alongToMarker += (plan.markers[bomberIndex][axis] - start[axis]) * plan.heading[axis];
    const float overheadSec   = alongToMarker / plan.speedPerSec;
    const float windowHalfSec = halfWindowDistance / plan.speedPerSec;
    const float windowEndSec  = overheadSec + windowHalfSec;

    for (float volleySec = overheadSec - windowHalfSec; volleySec <= windowEndSec;)
    {
        float pulseSec = volleySec;
        for (int pulse = 0; pulse < stats.firePulseCount && pulseSec <= windowEndSec; ++pulse)
        {
            BombingRunDrop drop;
            drop.timeSec     = pulseSec;
            drop.bomberIndex = bomberIndex;
            BomberPositionAt(plan, bomberIndex, pulseSec, drop.pos);
            drops.push_back(drop);
            if (pulse + 1 < stats.firePulseCount) pulseSec += stats.firePulseDelaySec;
        }
        volleySec = pulseSec + stats.fireRechargeSec;
    }
}

// Plans a run past `target` on ground at `groundZ`, heading along `aim`. Absent or
// unusable stats take the stock bomber's.
inline BombingRunPlan PlanBombingRun(const BomberStats& bomberStats,
                                     const float        target[3],
                                     const float        aim[3],
                                     float              groundZ)
{
    const BomberStats stats = WithStockBomberFallbacks(bomberStats);
    BombingRunPlan    plan;
    BombingRunHeading(aim, plan.heading);
    plan.speedPerSec = BomberSpeedPerSec(stats);

    const float side[3]                          = {-plan.heading[1], plan.heading[0], 0.0f};
    const float sideSign[kBombingRunBomberCount] = {0.0f, 1.0f, -1.0f};
    for (int bomber = 0; bomber < kBombingRunBomberCount; ++bomber)
    {
        const float back = (bomber == 0) ? 0.0f : kWingmanBackOffset;
        for (int axis = 0; axis < 3; ++axis)
        {
            plan.formationOffset[bomber][axis] =
                -plan.heading[axis] * back + side[axis] * sideSign[bomber] * kWingmanSideOffset;
            plan.markers[bomber][axis] = target[axis] + plan.formationOffset[bomber][axis];
        }
    }

    const float flightZ      = groundZ + stats.hoverOffset;
    const float leadDistance = plan.speedPerSec * kRunLeadSeconds;
    const float approach     = stats.attackDistance + leadDistance;
    plan.leaderStart[0]      = target[0] - plan.heading[0] * approach;
    plan.leaderStart[1]      = target[1] - plan.heading[1] * approach;
    plan.leaderStart[2]      = flightZ;
    // The trailing wingmen reach their markers kWingmanBackOffset later than the leader.
    plan.durationSec = (2.0f * approach + kWingmanBackOffset) / plan.speedPerSec;

    for (int bomber = 0; bomber < kBombingRunBomberCount; ++bomber)
        AppendBomberDrops(plan, stats, bomber, flightZ - plan.markers[bomber][2], plan.drops);
    std::stable_sort(plan.drops.begin(), plan.drops.end(),
                     [](const BombingRunDrop& a, const BombingRunDrop& b) { return a.timeSec < b.timeSec; });
    return plan;
}
