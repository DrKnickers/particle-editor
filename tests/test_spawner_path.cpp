// Regression test for the spawner shaped-path kinematics.
//
// Exercises EvalSpawnerPath (src/SpawnerPath.h), the closed form that
// drives spawner-owned instance motion: a deterministic arc
// (acceleration) plus a smooth per-axis sinusoidal squiggle with a
// per-instance phase. Header-only + pure, so this links nothing from the
// D3D rendering stack — just the DirectX libs for D3DXVECTOR3.
//
// Build:  tests\build_test_spawner_path.bat
// Run:    expects "Results: N passed, 0 failed".

#include "SpawnerPath.h"
#include <cstdio>
#include <cmath>
#include <initializer_list>

static int g_passed = 0;
static int g_failed = 0;

static void Check(bool ok, const char* expr, int line)
{
    if (ok) { ++g_passed; }
    else    { ++g_failed; std::printf("  FAIL line %d: %s\n", line, expr); }
}
#define CHECK(cond) Check((cond), #cond, __LINE__)

static bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
static bool NearV(const D3DXVECTOR3& a, const D3DXVECTOR3& b, float eps = 1e-3f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

int main()
{
    const D3DXVECTOR3 spawn(10.0f, 20.0f, -5.0f);
    const D3DXVECTOR3 v0(1.0f, 2.0f, 3.0f);

    // ── τ=0 emanates from the exact spawn point, even with
    //    nonzero amplitude + phase + accel (the −sin(φ) zeroing term). ──
    {
        SpawnerPathState s = { spawn, v0, D3DXVECTOR3(0, -9.8f, 0),
                               D3DXVECTOR3(3, 1, 2), 2.5f,
                               D3DXVECTOR3(0.7f, 1.9f, 4.1f) };
        D3DXVECTOR3 pos, vel;
        EvalSpawnerPath(s, 0.0f, pos, vel);
        CHECK(NearV(pos, spawn));                 // no frame-1 teleport
        // Initial velocity = v0 + per-instance lateral kick Aᵢ·ω·cos(φᵢ).
        float w = 2.0f * 3.14159265358979323846f * 2.5f;
        D3DXVECTOR3 kick(3 * w * cosf(0.7f), 1 * w * cosf(1.9f), 2 * w * cosf(4.1f));
        CHECK(NearV(vel, v0 + kick, 1e-2f));
    }

    // ── Accel-only is an exact parabola; velocity is live
    //    (v0 + a·τ), so emitted particles inherit the right value. ──
    {
        D3DXVECTOR3 a(0, -9.8f, 0);
        SpawnerPathState s = { spawn, v0, a,
                               D3DXVECTOR3(0, 0, 0), 1.0f,
                               D3DXVECTOR3(0, 0, 0) };
        for (float tau : { 0.5f, 1.0f, 3.0f })
        {
            D3DXVECTOR3 pos, vel;
            EvalSpawnerPath(s, tau, pos, vel);
            D3DXVECTOR3 expectPos = spawn + v0 * tau + (0.5f * tau * tau) * a;
            D3DXVECTOR3 expectVel = v0 + a * tau;
            CHECK(NearV(pos, expectPos, 1e-2f));
            CHECK(NearV(vel, expectVel, 1e-2f));
        }
    }

    // ── Squiggle-only: bounded by amplitude, and returns to the base
    //    (straight) path after a whole period (sin is periodic). ──
    {
        D3DXVECTOR3 amp(4, 0, 0);
        float freq = 2.0f;                         // period = 0.5 s
        SpawnerPathState s = { spawn, v0, D3DXVECTOR3(0, 0, 0),
                               amp, freq, D3DXVECTOR3(1.3f, 0, 0) };
        // After exactly one period the squiggle term is back to 0 ⇒ pos
        // equals the pure straight-line base at that τ.
        float period = 1.0f / freq;
        D3DXVECTOR3 pos, vel;
        EvalSpawnerPath(s, period, pos, vel);
        CHECK(NearV(pos, spawn + v0 * period, 1e-2f));
        // Lateral excursion never exceeds 2·amp (sin spans [−1,1] twice).
        bool bounded = true;
        for (int i = 1; i <= 200; ++i)
        {
            float tau = i * 0.01f;
            D3DXVECTOR3 p, vv;
            EvalSpawnerPath(s, tau, p, vv);
            float lateral = p.x - (spawn.x + v0.x * tau);   // deviation from base
            if (std::fabs(lateral) > 2.0f * amp.x + 1e-3f) bounded = false;
        }
        CHECK(bounded);
    }

    // ── All-zero shaping ⇒ a plain straight line, constant velocity. ──
    {
        SpawnerPathState s = { spawn, v0, D3DXVECTOR3(0, 0, 0),
                               D3DXVECTOR3(0, 0, 0), 5.0f,
                               D3DXVECTOR3(0, 0, 0) };
        D3DXVECTOR3 pos, vel;
        EvalSpawnerPath(s, 2.0f, pos, vel);
        CHECK(NearV(pos, spawn + v0 * 2.0f));
        CHECK(NearV(vel, v0));                      // velocity unchanged
    }

    std::printf("Results: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
