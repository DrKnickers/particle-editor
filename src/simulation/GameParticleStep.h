#pragma once

// Game-style particle stepping for the analytic preview.
//
// The game moves a particle once per rendered frame with semi-implicit Euler
// (decompiled alParticles::UpdateParticlePosition): velocity += a * dt first,
// then position += velocity * dt. After n steps of a fixed dt that lands at
//
//   p(t) = p0 + v0 * t + 0.5 * a * t^2 + 0.5 * a * dt * t        (t = n * dt)
//
// -- the exact parabola the editor evaluates, plus a drift of 0.5 * a * dt * t.
// So the preview stays analytic and matches the game at every step boundary by
// evaluating position with an initial speed shifted by 0.5 * a * dt. Velocity
// is unchanged: the game's velocity after n steps is v0 + a * t exactly.
//
// The game's dt is the real frame time (about 25-30 fps, uneven), so no preview
// can match it frame for frame; a fixed 1/30 s step stays within that jitter.
//
// Not covered by the shift:
//   - GROUND_BOUNCE particles: the game routes terrain-colliding particles
//     through a separate path (UpdateCollidableParticlePosition) that has not
//     been decompiled, so they keep the exact parabola.
//   - Inward acceleration: the game re-aims it along the particle's current
//     offset from the emitter every step; the editor fixes the direction at
//     spawn, which no closed form can follow.
//
// Floats only and no engine includes, so the unit test can link it bare.

#include "rendering/UnitConstants.h"

// Seconds per game particle step.
constexpr float kGameParticleStepSec = 1.0f / GAME_LOGIC_FPS;

// Writes the position-speed shift (0.5 * a * dt) for acceleration `acc`.
inline void GameStepPositionSpeedShift(const float acc[3], float shift[3], float dt = kGameParticleStepSec)
{
    for (int i = 0; i < 3; i++)
    {
        shift[i] = 0.5f * acc[i] * dt;
    }
}
