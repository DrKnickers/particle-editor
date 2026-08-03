#ifndef SPAWNER_PATH_H
#define SPAWNER_PATH_H

#include <d3dx9.h>
#include <cmath>

// Shaped-path kinematics for spawner-owned instances.
//
// Pulled out of ParticleSystemInstance::Update (which is D3D-coupled and
// can't link headless) so the closed form is one source of truth and can
// be unit-tested in isolation. Pure: no engine, no rendering, no state.

struct SpawnerPathState
{
    D3DXVECTOR3 spawnPos;       // launch position (world, post-Detach absolute)
    D3DXVECTOR3 spawnVel;       // launch velocity, units/sec
    D3DXVECTOR3 accel;          // deterministic arc, units/sec²
    D3DXVECTOR3 squiggleAmp;    // per-axis peak lateral displacement, world units
    float       squiggleFreq;   // squiggle frequency, Hz (oscillations/sec)
    D3DXVECTOR3 squigglePhase;  // per-instance random phase per axis, radians
};

// Evaluate the path at elapsed time tau (seconds since spawn, >= 0):
//
//   ω        = 2π·freq
//   sqᵢ(τ)   = Aᵢ·( sin(ωτ + φᵢ) − sin(φᵢ) )         // 0 at τ=0
//   pos(τ)   = spawnPos + spawnVel·τ + ½·accel·τ² + sq(τ)
//   vel(τ)   = spawnVel + accel·τ + Aᵢ·ω·cos(ωτ + φᵢ)
//
// The −sin(φᵢ) term zeroes the squiggle offset at τ=0 so the instance
// emanates from its exact spawn point; the residual Aᵢ·ω·cos(φᵢ) initial
// lateral velocity is the desired per-instance launch divergence. vel(τ)
// is the instantaneous velocity so emitted particles inherit it correctly.
inline void EvalSpawnerPath(const SpawnerPathState& s, float tau,
                            D3DXVECTOR3& outPos, D3DXVECTOR3& outVel)
{
    const float TWO_PI = 2.0f * 3.14159265358979323846f;
    const float omega  = TWO_PI * s.squiggleFreq;

    D3DXVECTOR3 sq, sqDot;
    sq.x    = s.squiggleAmp.x * (sinf(omega * tau + s.squigglePhase.x) - sinf(s.squigglePhase.x));
    sq.y    = s.squiggleAmp.y * (sinf(omega * tau + s.squigglePhase.y) - sinf(s.squigglePhase.y));
    sq.z    = s.squiggleAmp.z * (sinf(omega * tau + s.squigglePhase.z) - sinf(s.squigglePhase.z));
    sqDot.x = s.squiggleAmp.x * omega * cosf(omega * tau + s.squigglePhase.x);
    sqDot.y = s.squiggleAmp.y * omega * cosf(omega * tau + s.squigglePhase.y);
    sqDot.z = s.squiggleAmp.z * omega * cosf(omega * tau + s.squigglePhase.z);

    outPos = s.spawnPos + s.spawnVel * tau + (0.5f * tau * tau) * s.accel + sq;
    outVel = s.spawnVel + s.accel * tau + sqDot;
}

#endif
