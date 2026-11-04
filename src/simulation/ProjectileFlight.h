#pragma once

// How the game flies a missile, bomb, grenade or rocket, frame by frame, for the
// spawner's "match projectile" mode (#967). Owns the per-frame path only; straight
// shots keep ProjectileShot.h, the rocket's curve math lives in RocketSpline.h, and
// the lightsaber throw is not emulated.
//
// Decompiled (StarWarsI.exe debug build, ProjectileBehaviorClass::Service): every
// logic frame (GAME_LOGIC_FPS) a shot runs the service for its <Projectile_Category>:
//   - MISSILE  (Service_As_Missile_Path_Projectile): turns toward its target by at most
//     <Max_Rate_Of_Turn> degrees per frame on yaw and on pitch separately
//     (GameObjectClass::Turn_To_Face; a projectile skips the land/space speed
//     multiplier), then moves Speed along its new facing.
//   - BOMB, CLUSTER_BOMB (Service_As_Bomb_Path_Projectile): moves Speed along its
//     launch facing, then drops BombFallRate, which grows by
//     <Projectile_Bomb_Fall_Accel_Rate> every frame. The facing never changes.
//   - GRENADE (Service_As_Grenade_Path_Projectile): a closed-form lob,
//     pos(t) = P0 + t * (dir * v + t/2 * (0, 0, -Gravity * Lob_Mod)), t in frames,
//     facing the next position. The launch pitch and speed come from
//     WeaponBehaviorClass::Can_Grenade_Projectile_Lob_Hit_Target; with no solution the
//     game doesn't fire.
//   - ROCKET, MPTL_ROCKET (Service_As_Rocket_Path_Projectile): on its first frame it
//     builds a spline (Calculate_Rocket_Path_Space / _Land): a straight start along its
//     launch facing, then points every <Projectile_Rocket_Curve_Distance> x 1.1 toward the
//     target, each pushed aside by a random offset of up to <Projectile_Rocket_Curve_Offset>
//     (Construct_Rocket_Path_Segment, Add_Random_Offset), and an overshoot past the
//     target. On land the points stay above the ground (Adjust_Height). Every frame it
//     moves Speed further along the spline and turns toward the new point at
//     <Max_Rate_Of_Turn>; past the spline's end it detonates.
// A shot dies once it has travelled Speed per frame for <Projectile_Max_Flight_Distance>
// plus the launch-to-target height difference (WeaponBehaviorClass::Fire_Projectile),
// or, for a grenade with a <Projectile_Max_Lifetime>, once that many seconds pass
// (Advance_Projectile_Position). Collisions also end it; the preview models the ground
// plane and reaching the target point.
//
// Floats only and no engine includes, so the unit test can link it bare.

#include "RocketSpline.h"
#include "rendering/UnitConstants.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

enum class ProjectileFlightKind
{
    Direct,  // straight along the aim (ProjectileShot.h)
    Missile, // homes on the target at a limited turn rate
    Bomb,    // flies its launch facing and falls ever faster
    Grenade, // lobbed onto the target
    Rocket,  // follows a randomly bent spline onto the target
};

struct ProjectileFlightParams
{
    float speedPerFrame          = 0.0f;  // raw <Max_Speed>
    float range                  = 0.0f;  // <Projectile_Max_Flight_Distance>
    float turnRateDegPerFrame    = 0.0f;  // <Max_Rate_Of_Turn>
    float bombFallAccel          = 0.0f;  // <Projectile_Bomb_Fall_Accel_Rate>, units per frame per frame
    float grenadeGravity         = 0.0f;  // <Projectile_Grenade_Gravity>, units per frame per frame
    float grenadeLobMod          = 0.0f;  // <Projectile_Grenade_Gravity_Lob_Mod>
    bool  grenadeCanLobSlower    = false; // <Projectile_Grenade_Can_Lob_Slower>
    float maxLifetimeSec         = 0.0f;  // <Projectile_Max_Lifetime>; grenades only, 0 = none
    float groundZ                = 0.0f;  // preview ground plane height
    bool  clusterBomb            = false; // CLUSTER_BOMB launches toward the target
    float rocketStraightDistance = 0.0f;  // <Projectile_Rocket_Straight_Distance>, world units
    float rocketCurveDistance    = 0.0f;  // <Projectile_Rocket_Curve_Distance>, world units
    float rocketCurveOffset      = 0.0f;  // <Projectile_Rocket_Curve_Offset>, world units
    bool  rocketExplodesAtTarget = false; // <Explode_When_Reached_Target_Radius>
    bool  rocketTargetOnTerrain  = false; // <Projectile_Target_Point_On_Terrain>
    bool  inSpace                = false; // space rockets skip the land path's ground rules
    // A uniform draw in [lo, hi): the game's SyncRandom.Get_Float, for rocket offsets.
    // Required for rockets; a test passes a fixed sequence.
    std::function<float(float lo, float hi)> randomInRange;
};

// One logic frame of a shot: where it is and which way its model faces (unit vector).
struct FlightSample
{
    float pos[3];
    float facing[3];
};

// Safety bound on a computed path: 30 game seconds. Every stock curved shot ends sooner.
constexpr int   kMaxFlightFrames          = 900;
constexpr float kDegreesPerRadian         = 57.295776f; // the game's own constant
constexpr float kRadiansPerDegree         = 0.017453292f;
constexpr float kLobSlowerSpeedRatio      = 0.8f;    // Can_Grenade_Projectile_Lob_Hit_Target
constexpr float kLobMinHorizontalDistance = 0.0001f; // Calc_Lobbed_Projectile_Initial_Angle

// Rocket path constants (Calculate_Rocket_Path_Space / _Land, Construct_Rocket_Path_Segment).
constexpr float kRocketCurveDistanceScale       = 1.1f;  // control-point spacing x Curve_Distance
constexpr float kRocketStraightOffsetScale      = 0.1f;  // offset x Curve_Offset on the straight start
constexpr float kRocketTargetOffsetScale        = 1.0f;  // ... on the way to the target
constexpr float kRocketOvershootOffsetScale     = 0.3f;  // ... past the target
constexpr float kRocketRateOfDescent            = 0.3f;  // Adjust_Height's RATE_OF_DESCENT
constexpr float kRocketEvalSpacing              = 10.0f; // SplineClass::Evaluate(10)
constexpr float kRocketLandOvershootEvalSpacing = 20.0f; // Evaluate(0x14) before the land overshoot
constexpr float kRocketOffsetPlaneBias          = 1.0f;  // added to each axis of the segment midpoint

inline std::string ToUpperAscii(const std::string& text)
{
    std::string upper;
    for (char c : text) upper += (char)std::toupper((unsigned char)c);
    return upper;
}

inline ProjectileFlightKind ProjectileFlightKindFor(const std::string& category)
{
    const std::string upper = ToUpperAscii(category);
    if (upper == "MISSILE") return ProjectileFlightKind::Missile;
    if (upper == "BOMB" || upper == "CLUSTER_BOMB") return ProjectileFlightKind::Bomb;
    if (upper == "GRENADE") return ProjectileFlightKind::Grenade;
    if (upper == "ROCKET" || upper == "MPTL_ROCKET") return ProjectileFlightKind::Rocket;
    return ProjectileFlightKind::Direct;
}

// False for the one category the preview can't fly the game's way: the lightsaber
// throw. Everything else is a straight shot or one of the paths above.
inline bool IsProjectilePathEmulated(const std::string& category)
{
    return ToUpperAscii(category) != "LIGHT_SABER";
}

// CLUSTER_BOMB launches toward its target; a plain BOMB keeps its launcher's facing.
inline bool IsClusterBombCategory(const std::string& category)
{
    return ToUpperAscii(category) == "CLUSTER_BOMB";
}

// True when the shot aims at the spawner's target: a missile homes on it, a grenade
// lobs onto it, a rocket curves onto it, a cluster bomb launches toward it. Straight
// shots and plain bombs fly the aim.
inline bool ProjectileUsesTarget(const std::string& category)
{
    const ProjectileFlightKind kind = ProjectileFlightKindFor(category);
    return kind == ProjectileFlightKind::Missile || kind == ProjectileFlightKind::Grenade ||
           kind == ProjectileFlightKind::Rocket || IsClusterBombCategory(category);
}

// The game's facing: degrees, pitch on Y (negative = nose up), yaw on Z. Roll is unused.
struct GameFacing
{
    float pitchDeg = 0.0f;
    float yawDeg   = 0.0f;
};

// FacingClass::FacingClass(from, to), literally. The pitch test compares X and Z
// (not X and Y) in the game too.
inline GameFacing GameFacingFromTo(const float from[3], const float to[3])
{
    GameFacing f;
    if (!(from[0] == to[0] && from[1] == to[1]))
    {
        f.yawDeg = std::atan2(to[1] - from[1], to[0] - from[0]) * kDegreesPerRadian;
        if (f.yawDeg < 0.0f) f.yawDeg += 360.0f;
    }
    if (!(from[2] == to[2] && from[0] == to[0]))
    {
        const float dx = to[0] - from[0];
        const float dy = to[1] - from[1];
        f.pitchDeg     = -(std::atan2(to[2] - from[2], std::sqrt(dx * dx + dy * dy)) * kDegreesPerRadian);
    }
    return f;
}

// Rotate_Z(yaw) then Rotate_Y(pitch) applied to +X, as every path service builds it.
inline void GameFacingDirection(const GameFacing& f, float dir[3])
{
    const float pitch = f.pitchDeg * kRadiansPerDegree;
    const float yaw   = f.yawDeg * kRadiansPerDegree;
    dir[0]            = std::cos(pitch) * std::cos(yaw);
    dir[1]            = std::cos(pitch) * std::sin(yaw);
    dir[2]            = -std::sin(pitch);
}

// FacingClass::Clamp_180: an angle difference folded into [-180, 180].
inline float GameClamp180(float deg)
{
    while (deg > 180.0f) deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

// GameObjectClass::Turn_To_Face: each axis steps toward `desired` by at most `rateDeg`.
inline GameFacing GameTurnToFace(const GameFacing& current, const GameFacing& desired, float rateDeg)
{
    auto step = [rateDeg](float delta)
    {
        const float d = GameClamp180(delta);
        if (d > rateDeg) return rateDeg;
        if (d < -rateDeg) return -rateDeg;
        return d;
    };
    GameFacing turned;
    turned.pitchDeg = current.pitchDeg + step(desired.pitchDeg - current.pitchDeg);
    turned.yawDeg   = current.yawDeg + step(desired.yawDeg - current.yawDeg);
    return turned;
}

// WeaponBehaviorClass::Calc_Lobbed_Projectile_Initial_Angle: the high-lob launch angle
// (radians above horizontal) that lands `horizontalDistance` away and `heightDelta`
// higher at `speed`, under `gravity`. False when the speed can't reach.
inline bool GameLobAngle(float horizontalDistance, float heightDelta, float speed, float gravity, float& angleRad)
{
    angleRad                 = 0.0f;
    const float d            = (horizontalDistance == 0.0f) ? kLobMinHorizontalDistance : horizontalDistance;
    const float v2           = speed * speed;
    const float discriminant = (v2 * v2 - gravity * 2.0f * heightDelta * v2) - gravity * gravity * d * d;
    const float denominator  = ((heightDelta * heightDelta) / (d * d) + 1.0f) * 2.0f;
    if (discriminant < 0.0f || denominator == 0.0f) return false;
    const float horizontalSpeedSq = ((v2 - gravity * heightDelta) - std::sqrt(discriminant)) / denominator;
    if (horizontalSpeedSq < 0.0f) return false;
    angleRad = std::acos(std::sqrt(horizontalSpeedSq) / speed);
    return true;
}

// Can_Grenade_Projectile_Lob_Hit_Target: tries <Max_Speed>, then (when the grenade can
// lob slower) 0.8x that speed while a lob still reaches, keeping the slowest that does.
inline bool GameGrenadeLob(const ProjectileFlightParams& p,
                           const float                   launch[3],
                           const float                   target[3],
                           float&                        angleRad,
                           float&                        speedPerFrame)
{
    const float dx                 = target[0] - launch[0];
    const float dy                 = target[1] - launch[1];
    const float horizontalDistance = std::sqrt(dx * dx + dy * dy);
    const float heightDelta        = target[2] - launch[2];
    const float gravity            = p.grenadeGravity * p.grenadeLobMod;
    if (!(p.speedPerFrame > 0.0f) || !(gravity > 0.0f)) return false;

    bool  found = false;
    float speed = p.speedPerFrame;
    for (;;)
    {
        float angle;
        if (!GameLobAngle(horizontalDistance, heightDelta, speed, gravity, angle)) break;
        found         = true;
        angleRad      = angle;
        speedPerFrame = speed;
        if (!p.grenadeCanLobSlower) break;
        speed *= kLobSlowerSpeedRatio;
    }
    return found;
}

namespace ProjectileFlightDetail
{
inline void Push(std::vector<FlightSample>& out, const float pos[3], const float facing[3])
{
    FlightSample s;
    for (int i = 0; i < 3; i++)
    {
        s.pos[i]    = pos[i];
        s.facing[i] = facing[i];
    }
    out.push_back(s);
}

// Where the segment a -> b crosses height z, as a fraction of the way along it.
inline float CrossingFraction(const float a[3], const float b[3], float z)
{
    const float dz = b[2] - a[2];
    if (dz == 0.0f) return 1.0f;
    const float t = (z - a[2]) / dz;
    return (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
}
} // namespace ProjectileFlightDetail

// Seconds the game keeps a computed shot alive: one logic frame per step.
inline float FlightPathSeconds(const std::vector<FlightSample>& path)
{
    return path.size() > 1 ? (float)(path.size() - 1) / GAME_LOGIC_FPS : 0.0f;
}

// The shot `tauSec` after launch: position blended between logic frames, velocity in
// units per second, and the facing it flew that frame with (the next sample's). Past
// the end it rests on the last sample. False for an empty path.
inline bool SampleFlightPath(
    const std::vector<FlightSample>& path, float tauSec, float pos[3], float velocity[3], float facing[3])
{
    if (path.empty()) return false;
    const size_t last  = path.size() - 1;
    const float  frame = (tauSec > 0.0f) ? tauSec * GAME_LOGIC_FPS : 0.0f;
    size_t       i     = (size_t)frame;
    if (i >= last)
    {
        const FlightSample& end = path[last];
        for (int k = 0; k < 3; k++)
        {
            pos[k]      = end.pos[k];
            velocity[k] = 0.0f;
            facing[k]   = end.facing[k];
        }
        return true;
    }
    const FlightSample& a     = path[i];
    const FlightSample& b     = path[i + 1];
    const float         blend = frame - (float)i;
    for (int k = 0; k < 3; k++)
    {
        pos[k]      = a.pos[k] + (b.pos[k] - a.pos[k]) * blend;
        velocity[k] = (b.pos[k] - a.pos[k]) * GAME_LOGIC_FPS;
        facing[k]   = b.facing[k];
    }
    return true;
}

namespace RocketPathDetail
{
constexpr float kTwoPi = 6.2831855f; // Add_Random_Offset's angle range

inline void Cross(const float a[3], const float b[3], float out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

// Leaves a zero vector alone (assumed of Vector3::Normalize; only a degenerate
// segment produces one).
inline void Normalize(float v[3])
{
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 0.0f)
        for (int i = 0; i < 3; i++) v[i] /= length;
}

// Add_Random_Offset: a random direction in the plane of `u` and `v`, a random length
// up to `maxOffset`. The angle is drawn first, then the length.
inline void AddRandomOffset(
    float point[3], float maxOffset, const float u[3], const float v[3], const ProjectileFlightParams& p)
{
    const float angle  = p.randomInRange(0.0f, kTwoPi);
    const float length = p.randomInRange(0.0f, maxOffset);
    for (int i = 0; i < 3; i++) point[i] += (u[i] * std::cos(angle) + v[i] * std::sin(angle)) * length;
}

// Adjust_Height: at least the launch's clearance above the ground, and no lower than
// (1 - RATE_OF_DESCENT) times the previous point's height.
inline void AdjustHeight(float point[3], const float previous[3], float clearance, float groundZ)
{
    const float lowestAboveGround = groundZ + clearance;
    const float z                 = (point[2] <= lowestAboveGround) ? lowestAboveGround : point[2];
    const float lowestDescent     = previous[2] * (1.0f - kRocketRateOfDescent);
    point[2]                      = (z < lowestDescent) ? lowestDescent : z;
}

// Construct_Rocket_Path_Segment's alignment and flags.
struct SegmentRules
{
    float offsetScale    = 1.0f;  // times <Projectile_Rocket_Curve_Offset>
    bool  alignToGround  = false; // ALIGN_TO_TERRAIN (land) rather than ALIGN_NONE (space)
    bool  endOnGround    = false; // land: the final point drops onto the ground
    bool  isFinalSegment = false; // land: the final point takes no offset
};

// The rules each builder passes, named so the flags read at the call.
constexpr SegmentRules kSpaceStraight  = {kRocketStraightOffsetScale};
constexpr SegmentRules kSpaceToTarget  = {kRocketTargetOffsetScale};
constexpr SegmentRules kSpaceOvershoot = {kRocketOvershootOffsetScale};
constexpr SegmentRules kLandStraight   = {kRocketStraightOffsetScale, /*alignToGround=*/true,
                                          /*endOnGround=*/false, /*isFinalSegment=*/false};
constexpr SegmentRules kLandOvershoot  = {kRocketOvershootOffsetScale, /*alignToGround=*/true,
                                          /*endOnGround=*/true, /*isFinalSegment=*/true};
inline SegmentRules    LandToTarget(bool targetOnTerrain)
{
    return {kRocketTargetOffsetScale, /*alignToGround=*/true, /*endOnGround=*/targetOnTerrain,
            /*isFinalSegment=*/true};
}

// Construct_Rocket_Path_Segment: control points every Curve_Distance x 1.1 from
// `start` to `end`, each pushed aside in the plane across the segment. Space leaves
// the end point exact; land keeps the points above the ground. The preview's ground
// has no edge, so the game's map-bounds test never drops a point here.
inline void AddRocketSegment(RocketSpline&                 spline,
                             const float                   start[3],
                             const float                   end[3],
                             const SegmentRules&           rules,
                             const ProjectileFlightParams& p,
                             float                         clearance)
{
    const float spacing   = p.rocketCurveDistance * kRocketCurveDistanceScale;
    const float maxOffset = p.rocketCurveOffset * rules.offsetScale;
    float       back[3], bias[3], across[3], across2[3];
    for (int i = 0; i < 3; i++)
    {
        back[i] = start[i] - end[i];
        bias[i] = (end[i] + start[i]) * 0.5f + kRocketOffsetPlaneBias;
    }
    Cross(bias, back, across);
    Cross(across, back, across2);
    Normalize(across);
    Normalize(across2);

    const float length = std::sqrt(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
    // A zero spacing makes the x64 build's point count INT_MIN: no points at all.
    const int count = (spacing > 0.0f) ? (int)std::ceil(length / spacing) : 0;
    if (count <= 0) return;

    const float  step        = 1.0f / (float)count;
    const float* lastPoint   = spline.ControlPoint(spline.ControlPointCount() - 1);
    float        previous[3] = {lastPoint[0], lastPoint[1], lastPoint[2]};
    float        t           = step;
    for (int i = 0; i < count; i++, t += step)
    {
        float point[3];
        for (int k = 0; k < 3; k++) point[k] = start[k] * (1.0f - t) + end[k] * t;
        const bool isLast = (i == count - 1);
        if (!rules.alignToGround)
        {
            if (!isLast) AddRandomOffset(point, maxOffset, across, across2, p);
        }
        else if (isLast && rules.isFinalSegment)
        {
            if (rules.endOnGround) point[2] = p.groundZ;
        }
        else
        {
            AddRandomOffset(point, maxOffset, across, across2, p);
            AdjustHeight(point, previous, clearance, p.groundZ);
        }
        spline.AddControlPoint(point);
        for (int k = 0; k < 3; k++) previous[k] = point[k];
    }
}

// Cuts the path at `maxLength`: the control points from the segment holding that
// distance onward give way to the point at it.
inline void TrimRocketPath(RocketSpline& spline, float maxLength)
{
    if (!(maxLength < spline.Length())) return;
    float cut[3];
    int   segment = 0;
    if (!spline.PointFromLength(maxLength, cut, segment)) return;
    spline.RemoveControlPointsFrom(segment);
    spline.AddControlPoint(cut);
}

inline float FlatDistance(const float a[3], const float b[3])
{
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    return std::sqrt(dx * dx + dy * dy);
}

// Both builders ignore Evaluate's and LastEvaluatedStep's results, as the game does.
// A spline with too few points keeps its previous curve, so a path trimmed to two
// points still flies its untrimmed curve, capped by the flight distance; one that
// never evaluated has no length, and ComputeRocketFlight then reports no flight.
// Calculate_Rocket_Path_Space. Returns the distance along the path at which a rocket
// with <Explode_When_Reached_Target_Radius> stops: its flight distance.
inline float BuildSpaceRocketPath(RocketSpline&                 spline,
                                  const ProjectileFlightParams& p,
                                  const float                   launch[3],
                                  const float                   launchDir[3],
                                  const float                   target[3])
{
    spline.AddControlPoint(launch);
    float       start[3]     = {launch[0], launch[1], launch[2]};
    const float halfDistance = FlatDistance(launch, target) * 0.5f;
    const float straight     = (halfDistance < p.rocketStraightDistance) ? halfDistance : p.rocketStraightDistance;
    if (straight != 0.0f)
    {
        float end[3];
        for (int i = 0; i < 3; i++) end[i] = start[i] + launchDir[i] * straight;
        AddRocketSegment(spline, start, end, kSpaceStraight, p, 0.0f);
        for (int i = 0; i < 3; i++) start[i] = end[i];
    }
    AddRocketSegment(spline, start, target, kSpaceToTarget, p, 0.0f);

    if (!p.rocketExplodesAtTarget)
    {
        spline.Evaluate(kRocketEvalSpacing);
        // The game scales the step between the last two samples, not a unit vector,
        // by the distance left to fly.
        float lastStep[3] = {0.0f, 0.0f, 0.0f};
        spline.LastEvaluatedStep(lastStep);
        const float remaining = p.range - spline.Length();
        float       overshootEnd[3];
        for (int i = 0; i < 3; i++) overshootEnd[i] = target[i] + lastStep[i] * remaining;
        AddRocketSegment(spline, target, overshootEnd, kSpaceOvershoot, p, 0.0f);
        spline.Evaluate(kRocketEvalSpacing);
        TrimRocketPath(spline, p.range);
    }
    spline.Evaluate(kRocketEvalSpacing);
    return p.range;
}

// Calculate_Rocket_Path_Land. Returns the distance along the path at which a rocket
// with <Explode_When_Reached_Target_Radius> stops: the length up to its target.
inline float BuildLandRocketPath(RocketSpline&                 spline,
                                 const ProjectileFlightParams& p,
                                 const float                   launch[3],
                                 const float                   launchDir[3],
                                 const float                   target[3])
{
    spline.AddControlPoint(launch);
    float       start[3]  = {launch[0], launch[1], launch[2]};
    const float clearance = launch[2] - p.groundZ; // the launch's height above the ground
    // The land path ignores <Projectile_Rocket_Straight_Distance>: its straight start
    // is always half the horizontal distance.
    const float straight = FlatDistance(launch, target) * 0.5f;
    if (straight != 0.0f)
    {
        float end[3];
        for (int i = 0; i < 3; i++) end[i] = start[i] + launchDir[i] * straight;
        AddRocketSegment(spline, start, end, kLandStraight, p, clearance);
        for (int i = 0; i < 3; i++) start[i] = end[i];
    }
    AddRocketSegment(spline, start, target, LandToTarget(p.rocketTargetOnTerrain), p, clearance);

    float explodeDistance = p.range;
    if (p.rocketExplodesAtTarget)
    {
        spline.Evaluate(kRocketEvalSpacing);
        explodeDistance = spline.Length();
    }
    if (!p.rocketTargetOnTerrain)
    {
        spline.Evaluate(kRocketLandOvershootEvalSpacing);
        const float remaining  = p.range - spline.Length();
        float       flatDir[3] = {target[0] - start[0], target[1] - start[1], 0.0f};
        Normalize(flatDir);
        float overshootEnd[3];
        for (int i = 0; i < 3; i++) overshootEnd[i] = target[i] + flatDir[i] * remaining;
        AddRocketSegment(spline, target, overshootEnd, kLandOvershoot, p, clearance);
        spline.Evaluate(kRocketEvalSpacing);
        TrimRocketPath(spline, p.range);
    }
    spline.Evaluate(kRocketEvalSpacing);
    return explodeDistance;
}
} // namespace RocketPathDetail

// Flies a rocket: builds its spline once, then moves Speed further along it each frame,
// turning toward each new point at <Max_Rate_Of_Turn>. It ends past the spline's end
// (the game detonates it there), at its explode distance, at the flight-distance limit,
// on the ground (land), or within one step of the target. False when it would detonate
// on its first frame: a path too short for the spline to evaluate.
inline bool ComputeRocketFlight(const ProjectileFlightParams& p,
                                const float                   launch[3],
                                GameFacing                    facing,
                                const float                   target[3],
                                std::vector<FlightSample>&    out)
{
    using namespace ProjectileFlightDetail;
    using namespace RocketPathDetail;
    assert(p.randomInRange && "a rocket's path needs a random source");

    float dir[3];
    GameFacingDirection(facing, dir);
    RocketSpline spline;
    const float  explodeDistance = p.inSpace ? BuildSpaceRocketPath(spline, p, launch, dir, target)
                                             : BuildLandRocketPath(spline, p, launch, dir, target);
    const float  maxTravel       = p.range + std::fabs(target[2] - launch[2]);

    float pos[3] = {launch[0], launch[1], launch[2]};
    Push(out, pos, dir);
    float alongSpline = 0.0f;
    float travelled   = 0.0f;
    for (int frame = 1; frame < kMaxFlightFrames; frame++)
    {
        alongSpline += p.speedPerFrame;
        float next[3];
        int   segment = 0;
        if (!spline.PointFromLength(alongSpline, next, segment)) break;
        facing = GameTurnToFace(facing, GameFacingFromTo(pos, next), p.turnRateDegPerFrame);
        GameFacingDirection(facing, dir);
        // A point held exactly at ground level (a launch with no clearance) isn't a hit.
        if (!p.inSpace && next[2] < p.groundZ)
        {
            const float f = CrossingFraction(pos, next, p.groundZ);
            float       hit[3];
            for (int i = 0; i < 3; i++) hit[i] = pos[i] + (next[i] - pos[i]) * f;
            Push(out, hit, dir);
            break;
        }
        for (int i = 0; i < 3; i++) pos[i] = next[i];
        Push(out, pos, dir);

        travelled += p.speedPerFrame;
        if (travelled >= maxTravel) break;
        if (p.rocketExplodesAtTarget && explodeDistance <= alongSpline) break;
        // Within one step of the target: the game's shot would hit the target object.
        const float tx = target[0] - pos[0], ty = target[1] - pos[1], tz = target[2] - pos[2];
        if (std::sqrt(tx * tx + ty * ty + tz * tz) <= p.speedPerFrame) break;
    }
    return out.size() > 1;
}

// Computes a curved shot's path, one sample per logic frame starting at the launch.
// `aim` is the launch direction for a missile or a bomb (the spawner's velocity; zero
// aims along +X); a grenade and a cluster bomb face `target` as in game. Returns false
// when the game wouldn't fire: no usable speed, or a grenade that can't reach.
inline bool ComputeProjectileFlight(ProjectileFlightKind          kind,
                                    const ProjectileFlightParams& p,
                                    const float                   launch[3],
                                    const float                   aim[3],
                                    const float                   target[3],
                                    std::vector<FlightSample>&    out)
{
    using namespace ProjectileFlightDetail;
    out.clear();
    if (!std::isfinite(p.speedPerFrame) || p.speedPerFrame <= 0.0f) return false;

    const float origin[3] = {0.0f, 0.0f, 0.0f};
    const bool  aimIsZero = (aim[0] == 0.0f && aim[1] == 0.0f && aim[2] == 0.0f);
    const float plusX[3]  = {1.0f, 0.0f, 0.0f};
    GameFacing  facing    = GameFacingFromTo(origin, aimIsZero ? plusX : aim);
    if (kind == ProjectileFlightKind::Rocket) return ComputeRocketFlight(p, launch, facing, target, out);

    // Fire_Projectile adds the launch-to-target height difference to the flight distance.
    const float maxTravel = p.range + std::fabs(target[2] - launch[2]);
    float       pos[3]    = {launch[0], launch[1], launch[2]};
    float       dir[3];

    if (kind == ProjectileFlightKind::Grenade)
    {
        float lobAngle = 0.0f, speed = 0.0f;
        if (!GameGrenadeLob(p, launch, target, lobAngle, speed)) return false;
        facing          = GameFacingFromTo(launch, target);
        facing.pitchDeg = -(lobAngle * kDegreesPerRadian);
        GameFacingDirection(facing, dir);
        const float gravity = p.grenadeGravity * p.grenadeLobMod;
        const bool  ageEnds = p.maxLifetimeSec > 0.0f; // Advance_Projectile_Position
        Push(out, pos, dir);
        float travelled = 0.0f;
        for (int frame = 1; frame < kMaxFlightFrames; frame++)
        {
            const float t = (float)frame;
            float       next[3];
            for (int i = 0; i < 3; i++) next[i] = launch[i] + t * (dir[i] * speed);
            next[2] += t * (0.5f * t * -gravity);

            float faceDir[3];
            GameFacingDirection(GameFacingFromTo(pos, next), faceDir);
            // Landing: the target point or the ground, on the way down.
            const float landZ = (target[2] > p.groundZ) ? target[2] : p.groundZ;
            if (next[2] < pos[2] && next[2] <= landZ)
            {
                const float f = CrossingFraction(pos, next, landZ);
                float       hit[3];
                for (int i = 0; i < 3; i++) hit[i] = pos[i] + (next[i] - pos[i]) * f;
                Push(out, hit, faceDir);
                return true;
            }
            for (int i = 0; i < 3; i++) pos[i] = next[i];
            Push(out, pos, faceDir);
            travelled += speed;
            const bool expired = ageEnds ? (t / GAME_LOGIC_FPS > p.maxLifetimeSec) : (travelled >= maxTravel);
            if (expired) return true;
        }
        return true;
    }

    if (kind == ProjectileFlightKind::Bomb && p.clusterBomb) facing = GameFacingFromTo(launch, target);
    GameFacingDirection(facing, dir);
    Push(out, pos, dir);

    float travelled    = 0.0f;
    float bombFallRate = 0.0f;
    for (int frame = 1; frame < kMaxFlightFrames; frame++)
    {
        if (kind == ProjectileFlightKind::Missile)
        {
            facing = GameTurnToFace(facing, GameFacingFromTo(pos, target), p.turnRateDegPerFrame);
            GameFacingDirection(facing, dir);
        }

        float next[3];
        for (int i = 0; i < 3; i++) next[i] = pos[i] + dir[i] * p.speedPerFrame;
        if (kind == ProjectileFlightKind::Bomb)
        {
            next[2] -= bombFallRate;
            bombFallRate += p.bombFallAccel;
            if (next[2] <= p.groundZ)
            {
                const float f = CrossingFraction(pos, next, p.groundZ);
                float       hit[3];
                for (int i = 0; i < 3; i++) hit[i] = pos[i] + (next[i] - pos[i]) * f;
                Push(out, hit, dir);
                return true;
            }
        }
        for (int i = 0; i < 3; i++) pos[i] = next[i];
        Push(out, pos, dir);

        travelled += p.speedPerFrame;
        if (travelled >= maxTravel) return true;
        if (kind == ProjectileFlightKind::Missile)
        {
            // Within one step of the target: the game's shot would hit the target object.
            const float tx = target[0] - pos[0], ty = target[1] - pos[1], tz = target[2] - pos[2];
            if (std::sqrt(tx * tx + ty * ty + tz * tz) <= p.speedPerFrame) return true;
        }
    }
    return true;
}
