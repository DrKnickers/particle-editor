#pragma once

// GROUND_BOUNCE catch-up: advance a particle's parabola through every bounce
// that happened before time `t`.
//
// A particle's motion is stored as a parabola (position, speed, acceleration
// from `positionTime`) plus the time of its next ground contact (`bounceTime`).
// Each bounce restarts the parabola at the contact point with the vertical speed
// reflected and scaled by `bounciness`, and schedules the next contact one full
// arc later: delta = 2 * -v_z / a_z.
//
// The original loop only stopped when a bounce left v_z exactly 0, so it could
// fail to terminate for inputs the file format and the UI both allow:
//
//   - bounciness = -1 keeps the reflected speed pointing down, delta goes
//     negative and bounceTime oscillates below `t` forever;
//   - bounciness = 1 never shrinks v_z, and once delta drops under half an ulp
//     of bounceTime the `+=` is a no-op and the loop spins in place;
//   - even an ordinary 0 < bounciness < 1 decays its arcs below half an ulp of
//     bounceTime; from there the sign-alternating sub-ulp steps walk bounceTime
//     backwards and the loop spins or runs for millions of iterations (a strong
//     downward acceleration gets there within a few seconds of particle life);
//   - NaN / infinite bounciness poisoned the particle state.
//
// The rule: a bounce whose next arc is not strictly forward in time
// (`!(delta > 0)`, which also catches NaN) or too short to move bounceTime
// (`next == bounceTime`) ends the bouncing exactly the way v_z == 0 always did:
// v_z = 0 and bounceTime = FLT_MAX. Every bounce before that point is computed
// bit-for-bit as before (tests/test_bounce_catch_up.cpp pins it against the
// original loop); the particle settles exactly where the old arithmetic broke
// down. Values outside [0,1] still load, save and edit unchanged; only the
// simulation is made safe.
//
// A per-update step cap bounds the rest: bounciness at or near 1, and the
// ulp-scale tail of a decaying bounce, where rounding can lift each sub-ulp arc
// to a whole ulp and keep the particle "bouncing" one ulp at a time (seen for
// bounciness above about 1/3). Hitting the cap settles the particle the same way.
//
// Floats only and no engine includes, so the unit test can link it bare.
// pos / speed / acc are x,y,z triples (a D3DXVECTOR3 converts to float*).

#include <cfloat>
#include <cmath>

static const int kMaxBounceCatchUpSteps = 1024;

// Returns the number of bounces processed (for the unit test).
inline int BounceCatchUp(float pos[3], float speed[3], const float acc[3],
                         float& positionTime, float& bounceTime,
                         float t, float bounciness)
{
    // A non-finite coefficient behaves like 0: the particle stops at its first
    // ground contact. A non-finite time would never let the loop catch up.
    const float b = std::isfinite(bounciness) ? bounciness : 0.0f;
    if (!std::isfinite(t))
    {
        return 0;
    }

    int steps = 0;
    while (t > bounceTime)
    {
        // The particle has bounced
        float bt = bounceTime - positionTime;
        for (int i = 0; i < 3; i++)
        {
            pos[i]   = pos[i] + (speed[i] + 0.5f * acc[i] * bt) * bt;
            speed[i] = speed[i] + acc[i] * bt;
        }
        speed[2]     = -speed[2] * b;
        positionTime = bounceTime;
        steps++;

        // Calculate new bounce time
        if (acc[2] == 0 || speed[2] == 0)
        {
            // No more bounces
            bounceTime = FLT_MAX;
        }
        else
        {
            // Calculate the new parabola
            // We know x(0) is 0, so the problem becomes a lot simpler
            const float delta = 2 * -speed[2] / acc[2];
            const float next  = bounceTime + delta;
            if (!(delta > 0) || next == bounceTime ||
                (steps >= kMaxBounceCatchUpSteps && t > next))
            {
                // The next arc goes backwards, is NaN, is too short to advance
                // time, or would run past the per-update cap: settle on the
                // ground and stop bouncing.
                speed[2]   = 0.0f;
                bounceTime = FLT_MAX;
            }
            else
            {
                bounceTime = next;
            }
        }
    }
    return steps;
}
