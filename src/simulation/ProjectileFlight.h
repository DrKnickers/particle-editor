#pragma once

// How the game flies a missile, bomb or grenade, frame by frame, for the spawner's
// "match projectile" mode (#967). Owns the per-frame path only; straight shots keep
// ProjectileShot.h, and rockets (a spline path) are not emulated yet.
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
// A shot dies once it has travelled Speed per frame for <Projectile_Max_Flight_Distance>
// plus the launch-to-target height difference (WeaponBehaviorClass::Fire_Projectile),
// or, for a grenade with a <Projectile_Max_Lifetime>, once that many seconds pass
// (Advance_Projectile_Position). Collisions also end it; the preview models the ground
// plane and reaching the target point.
//
// Floats only and no engine includes, so the unit test can link it bare.

#include "rendering/UnitConstants.h"

#include <cctype>
#include <cmath>
#include <string>
#include <vector>

enum class ProjectileFlightKind
{
    Direct,     // straight along the aim (ProjectileShot.h)
    Missile,    // homes on the target at a limited turn rate
    Bomb,       // flies its launch facing and falls ever faster
    Grenade,    // lobbed onto the target
};

struct ProjectileFlightParams
{
    float speedPerFrame        = 0.0f;   // raw <Max_Speed>
    float range                = 0.0f;   // <Projectile_Max_Flight_Distance>
    float turnRateDegPerFrame  = 0.0f;   // <Max_Rate_Of_Turn>
    float bombFallAccel        = 0.0f;   // <Projectile_Bomb_Fall_Accel_Rate>, units per frame per frame
    float grenadeGravity       = 0.0f;   // <Projectile_Grenade_Gravity>, units per frame per frame
    float grenadeLobMod        = 0.0f;   // <Projectile_Grenade_Gravity_Lob_Mod>
    bool  grenadeCanLobSlower  = false;  // <Projectile_Grenade_Can_Lob_Slower>
    float maxLifetimeSec       = 0.0f;   // <Projectile_Max_Lifetime>; grenades only, 0 = none
    float groundZ              = 0.0f;   // preview ground plane height
    bool  clusterBomb          = false;  // CLUSTER_BOMB launches toward the target
};

// One logic frame of a shot: where it is and which way its model faces (unit vector).
struct FlightSample
{
    float pos[3];
    float facing[3];
};

// Safety bound on a computed path: 30 game seconds. Every stock curved shot ends sooner.
constexpr int   kMaxFlightFrames         = 900;
constexpr float kDegreesPerRadian        = 57.295776f;   // the game's own constant
constexpr float kRadiansPerDegree        = 0.017453292f;
constexpr float kLobSlowerSpeedRatio     = 0.8f;         // Can_Grenade_Projectile_Lob_Hit_Target
constexpr float kLobMinHorizontalDistance = 0.0001f;     // Calc_Lobbed_Projectile_Initial_Angle

inline ProjectileFlightKind ProjectileFlightKindFor(const std::string& category)
{
    std::string upper;
    for (char c : category) upper += (char)std::toupper((unsigned char)c);
    if (upper == "MISSILE")                         return ProjectileFlightKind::Missile;
    if (upper == "BOMB" || upper == "CLUSTER_BOMB") return ProjectileFlightKind::Bomb;
    if (upper == "GRENADE")                         return ProjectileFlightKind::Grenade;
    return ProjectileFlightKind::Direct;
}

// False for the categories the preview can't fly the game's way yet: rockets (a
// spline path, ROCKET and MPTL_ROCKET) and the lightsaber throw. Everything else is a
// straight shot or one of the paths above.
inline bool IsProjectilePathEmulated(const std::string& category)
{
    std::string upper;
    for (char c : category) upper += (char)std::toupper((unsigned char)c);
    return upper.find("ROCKET") == std::string::npos && upper != "LIGHT_SABER";
}

// CLUSTER_BOMB launches toward its target; a plain BOMB keeps its launcher's facing.
inline bool IsClusterBombCategory(const std::string& category)
{
    std::string upper;
    for (char c : category) upper += (char)std::toupper((unsigned char)c);
    return upper == "CLUSTER_BOMB";
}

// True when the shot aims at the spawner's target: a missile homes on it, a grenade
// lobs onto it, a cluster bomb launches toward it. Straight shots and plain bombs fly
// the aim.
inline bool ProjectileUsesTarget(const std::string& category)
{
    const ProjectileFlightKind kind = ProjectileFlightKindFor(category);
    return kind == ProjectileFlightKind::Missile || kind == ProjectileFlightKind::Grenade ||
           IsClusterBombCategory(category);
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
        f.pitchDeg = -(std::atan2(to[2] - from[2], std::sqrt(dx * dx + dy * dy)) * kDegreesPerRadian);
    }
    return f;
}

// Rotate_Z(yaw) then Rotate_Y(pitch) applied to +X, as every path service builds it.
inline void GameFacingDirection(const GameFacing& f, float dir[3])
{
    const float pitch = f.pitchDeg * kRadiansPerDegree;
    const float yaw   = f.yawDeg * kRadiansPerDegree;
    dir[0] = std::cos(pitch) * std::cos(yaw);
    dir[1] = std::cos(pitch) * std::sin(yaw);
    dir[2] = -std::sin(pitch);
}

// FacingClass::Clamp_180: an angle difference folded into [-180, 180].
inline float GameClamp180(float deg)
{
    while (deg > 180.0f)  deg -= 360.0f;
    while (deg < -180.0f) deg += 360.0f;
    return deg;
}

// GameObjectClass::Turn_To_Face: each axis steps toward `desired` by at most `rateDeg`.
inline GameFacing GameTurnToFace(const GameFacing& current, const GameFacing& desired, float rateDeg)
{
    auto step = [rateDeg](float delta) {
        const float d = GameClamp180(delta);
        if (d > rateDeg)  return rateDeg;
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
    angleRad = 0.0f;
    const float d  = (horizontalDistance == 0.0f) ? kLobMinHorizontalDistance : horizontalDistance;
    const float v2 = speed * speed;
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
inline bool GameGrenadeLob(const ProjectileFlightParams& p, const float launch[3], const float target[3],
                           float& angleRad, float& speedPerFrame)
{
    const float dx = target[0] - launch[0];
    const float dy = target[1] - launch[1];
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
        for (int i = 0; i < 3; i++) { s.pos[i] = pos[i]; s.facing[i] = facing[i]; }
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
}

// Seconds the game keeps a computed shot alive: one logic frame per step.
inline float FlightPathSeconds(const std::vector<FlightSample>& path)
{
    return path.size() > 1 ? (float)(path.size() - 1) / GAME_LOGIC_FPS : 0.0f;
}

// The shot `tauSec` after launch: position blended between logic frames, velocity in
// units per second, and the facing it flew that frame with (the next sample's). Past
// the end it rests on the last sample. False for an empty path.
inline bool SampleFlightPath(const std::vector<FlightSample>& path, float tauSec,
                             float pos[3], float velocity[3], float facing[3])
{
    if (path.empty()) return false;
    const size_t last  = path.size() - 1;
    const float  frame = (tauSec > 0.0f) ? tauSec * GAME_LOGIC_FPS : 0.0f;
    size_t i = (size_t)frame;
    if (i >= last)
    {
        const FlightSample& end = path[last];
        for (int k = 0; k < 3; k++) { pos[k] = end.pos[k]; velocity[k] = 0.0f; facing[k] = end.facing[k]; }
        return true;
    }
    const FlightSample& a = path[i];
    const FlightSample& b = path[i + 1];
    const float blend = frame - (float)i;
    for (int k = 0; k < 3; k++)
    {
        pos[k]      = a.pos[k] + (b.pos[k] - a.pos[k]) * blend;
        velocity[k] = (b.pos[k] - a.pos[k]) * GAME_LOGIC_FPS;
        facing[k]   = b.facing[k];
    }
    return true;
}

// Computes a curved shot's path, one sample per logic frame starting at the launch.
// `aim` is the launch direction for a missile or a bomb (the spawner's velocity; zero
// aims along +X); a grenade and a cluster bomb face `target` as in game. Returns false
// when the game wouldn't fire: no usable speed, or a grenade that can't reach.
inline bool ComputeProjectileFlight(ProjectileFlightKind kind, const ProjectileFlightParams& p,
                                    const float launch[3], const float aim[3], const float target[3],
                                    std::vector<FlightSample>& out)
{
    using namespace ProjectileFlightDetail;
    out.clear();
    if (!std::isfinite(p.speedPerFrame) || p.speedPerFrame <= 0.0f) return false;

    const float origin[3] = { 0.0f, 0.0f, 0.0f };
    const bool  aimIsZero = (aim[0] == 0.0f && aim[1] == 0.0f && aim[2] == 0.0f);
    const float plusX[3]  = { 1.0f, 0.0f, 0.0f };
    GameFacing facing = GameFacingFromTo(origin, aimIsZero ? plusX : aim);

    // Fire_Projectile adds the launch-to-target height difference to the flight distance.
    const float maxTravel = p.range + std::fabs(target[2] - launch[2]);
    float pos[3] = { launch[0], launch[1], launch[2] };
    float dir[3];

    if (kind == ProjectileFlightKind::Grenade)
    {
        float lobAngle = 0.0f, speed = 0.0f;
        if (!GameGrenadeLob(p, launch, target, lobAngle, speed)) return false;
        facing = GameFacingFromTo(launch, target);
        facing.pitchDeg = -(lobAngle * kDegreesPerRadian);
        GameFacingDirection(facing, dir);
        const float gravity = p.grenadeGravity * p.grenadeLobMod;
        const bool  ageEnds = p.maxLifetimeSec > 0.0f;   // Advance_Projectile_Position
        Push(out, pos, dir);
        float travelled = 0.0f;
        for (int frame = 1; frame < kMaxFlightFrames; frame++)
        {
            const float t = (float)frame;
            float next[3];
            for (int i = 0; i < 3; i++) next[i] = launch[i] + t * (dir[i] * speed);
            next[2] += t * (0.5f * t * -gravity);

            float faceDir[3];
            GameFacingDirection(GameFacingFromTo(pos, next), faceDir);
            // Landing: the target point or the ground, on the way down.
            const float landZ = (target[2] > p.groundZ) ? target[2] : p.groundZ;
            if (next[2] < pos[2] && next[2] <= landZ)
            {
                const float f = CrossingFraction(pos, next, landZ);
                float hit[3];
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

    if (kind == ProjectileFlightKind::Bomb && p.clusterBomb)
        facing = GameFacingFromTo(launch, target);
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
                float hit[3];
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
