#pragma once

// How the game flies a projectile, for the spawner's "match projectile" mode.
//
// The game's per-frame projectile rules:
//   - At launch, Speed = the raw <Max_Speed>; no game-speed, mode or combat
//     modifier applies to a projectile.
//   - Every logic frame (30 per game second, GAME_LOGIC_FPS) a direct-path shot
//     moves position += facing * Speed and adds Speed to DistanceTravelled.
//   - The shot is destroyed once DistanceTravelled >= <Projectile_Max_Flight_Distance>,
//     so it lives ceil(range / Speed) frames (at least one).
//   - Projectile acceleration per frame is 0 in this build: speed never changes.
//
// Missiles, bombs and grenades run other paths (homing, falls, lobs) that read the
// same Speed: see ProjectileFlight.h.
//
// Floats only and no engine includes, so the unit test can link it bare.

#include "rendering/UnitConstants.h"

#include <cmath>

struct ProjectileShot
{
    float velocity[3]; // world units per second
    float flightSec;   // seconds until the game destroys the shot
};

// Flies a shot along `aim` (any length; zero aims along +X). Returns false when
// `speedPerFrame` can't fly a shot (zero, negative or not finite).
inline bool ComputeProjectileShot(float speedPerFrame, float maxFlightDistance, const float aim[3], ProjectileShot& out)
{
    if (!std::isfinite(speedPerFrame) || speedPerFrame <= 0.0f) return false;

    float       dir[3] = {aim[0], aim[1], aim[2]};
    const float len    = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!std::isfinite(len) || len <= 0.0f)
    {
        dir[0] = 1.0f;
        dir[1] = 0.0f;
        dir[2] = 0.0f;
    }
    else
    {
        for (float& d : dir) d /= len;
    }

    const float speedPerSec = speedPerFrame * GAME_LOGIC_FPS;
    for (int i = 0; i < 3; i++) out.velocity[i] = dir[i] * speedPerSec;

    const float range  = (std::isfinite(maxFlightDistance) && maxFlightDistance > 0.0f) ? maxFlightDistance : 0.0f;
    float       frames = std::ceil(range / speedPerFrame);
    if (frames < 1.0f) frames = 1.0f;
    out.flightSec = frames / GAME_LOGIC_FPS;
    return true;
}

// Orthonormal frame that turns a model's +X toward `dir`, keeping its +Z as close
// to world up (+Z) as the direction allows. Rows are the model's X, Y and Z axes in
// world space (the D3D row-vector convention). A zero `dir` gives the identity.
inline void ProjectileShotBasis(const float dir[3], float x[3], float y[3], float z[3])
{
    const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!std::isfinite(len) || len <= 0.0f)
    {
        x[0] = 1;
        x[1] = 0;
        x[2] = 0;
        y[0] = 0;
        y[1] = 1;
        y[2] = 0;
        z[0] = 0;
        z[1] = 0;
        z[2] = 1;
        return;
    }
    for (int i = 0; i < 3; i++) x[i] = dir[i] / len;

    // y = up x X; a near-vertical shot has no heading, so use world +Y as its side.
    float up[3] = {0, 0, 1};
    float cy[3] = {up[1] * x[2] - up[2] * x[1], up[2] * x[0] - up[0] * x[2], up[0] * x[1] - up[1] * x[0]};
    float cyLen = std::sqrt(cy[0] * cy[0] + cy[1] * cy[1] + cy[2] * cy[2]);
    if (cyLen < 1e-4f)
    {
        cy[0] = 0;
        cy[1] = 1;
        cy[2] = 0;
        cyLen = 1.0f;
    }
    for (int i = 0; i < 3; i++) y[i] = cy[i] / cyLen;

    z[0] = x[1] * y[2] - x[2] * y[1];
    z[1] = x[2] * y[0] - x[0] * y[2];
    z[2] = x[0] * y[1] - x[1] * y[0];
}

// World axes of a shot's MODEL. The game draws every object's model turned +90
// degrees about its local Z after the facing rotations (yaw about Z, pitch about Y,
// roll about X, then another 90 degrees about Z), so the model's -Y axis points
// along the flight. A laser bolt is modelled along Y for that reason.
// Rows as in ProjectileShotBasis.
inline void ProjectileShotModelAxes(const float dir[3], float mx[3], float my[3], float mz[3])
{
    float x[3], y[3], z[3];
    ProjectileShotBasis(dir, x, y, z);
    for (int i = 0; i < 3; i++)
    {
        mx[i] = y[i];
        my[i] = -x[i];
        mz[i] = z[i];
    }
}
