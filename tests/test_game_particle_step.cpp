// Test for the game-style particle step shift (src/simulation/GameParticleStep.h).
//
// The game advances a particle with semi-implicit Euler at the frame time
// (velocity first, then position). The preview evaluates a closed form instead,
// with the initial speed shifted by 0.5 * a * dt. This test steps the game's
// update literally at 1/30 s and checks the shifted closed form lands on it at
// every step boundary, while the unshifted form trails by 0.5 * a * dt * t.

#include "simulation/GameParticleStep.h"

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

static bool Near(double a, double b, double tol)
{
    return std::fabs(a - b) <= tol;
}

int main()
{
    std::printf("[constants]\n");
    CHECK(GAME_LOGIC_FPS == 30.0f, "game logic rate is 30 per second");
    CHECK(Near(kGameParticleStepSec, 1.0 / 30.0, 1e-7), "particle step is 1/30 s");

    std::printf("[shift]\n");
    {
        const float acc[3] = {12.0f, -300.0f, -50.0f};
        float       shift[3];
        GameStepPositionSpeedShift(acc, shift);
        CHECK(Near(shift[0], 0.2, 1e-6) && Near(shift[1], -5.0, 1e-5) && Near(shift[2], -50.0 / 60.0, 1e-6),
              "shift = 0.5 * a / 30 per axis");
        const float zero[3] = {0, 0, 0};
        GameStepPositionSpeedShift(zero, shift);
        CHECK(shift[0] == 0 && shift[1] == 0 && shift[2] == 0, "no acceleration -> no shift");
        GameStepPositionSpeedShift(acc, shift, 0.0f);
        CHECK(shift[0] == 0 && shift[1] == 0 && shift[2] == 0, "zero step -> exact curve");
    }

    std::printf("[matches the game's semi-implicit Euler]\n");
    {
        struct Case
        {
            float       p0[3], v0[3], a[3];
            const char* name;
        };
        const Case cases[] = {
            {{0, 0, 10}, {0, 0, 0}, {0, 0, -50}, "gravity from rest"},
            {{5, -3, 0}, {40, 0, 120}, {0, 0, -980}, "fast arc under strong gravity"},
            {{0, 0, 0}, {-7, 15, 2}, {300, -20, 0}, "sideways acceleration"},
        };
        for (const Case& c : cases)
        {
            double p[3] = {c.p0[0], c.p0[1], c.p0[2]};
            double v[3] = {c.v0[0], c.v0[1], c.v0[2]};
            float  shift[3];
            GameStepPositionSpeedShift(c.a, shift);
            const double dt        = 1.0 / 30.0;
            bool         shiftedOk = true, exactTrails = true;
            double       worstShifted = 0;
            for (int n = 1; n <= 90; n++) // three seconds of game steps
            {
                for (int i = 0; i < 3; i++)
                {
                    v[i] += c.a[i] * dt;
                    p[i] += v[i] * dt;
                }
                const double t = n * dt;
                for (int i = 0; i < 3; i++)
                {
                    const double exact   = c.p0[i] + (c.v0[i] + 0.5 * c.a[i] * t) * t;
                    const double shifted = c.p0[i] + (c.v0[i] + shift[i] + 0.5 * c.a[i] * t) * t;
                    const double tol     = 1e-4 * (1.0 + std::fabs(p[i]));
                    worstShifted         = std::fmax(worstShifted, std::fabs(shifted - p[i]));
                    if (!Near(shifted, p[i], tol)) shiftedOk = false;
                    if (!Near(p[i] - exact, 0.5 * c.a[i] * dt * t, tol)) exactTrails = false;
                }
            }
            char msg[160];
            std::snprintf(msg, sizeof msg, "%s: shifted closed form == stepped game (worst %.2g units)", c.name,
                          worstShifted);
            CHECK(shiftedOk, msg);
            std::snprintf(msg, sizeof msg, "%s: exact curve trails the game by 0.5*a*dt*t", c.name);
            CHECK(exactTrails, msg);
        }
    }

    std::printf(g_fail ? "\n=== GameParticleStep: %d FAILED ===\n" : "\n=== GameParticleStep: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
