// Test for the match-projectile shot rule (src/simulation/ProjectileShot.h).
//
// The game moves a shot <Max_Speed> world units each logic frame (30 per second)
// and destroys it once it has flown <Projectile_Max_Flight_Distance>. This test
// steps that literally and checks the spawner's velocity and lifetime reproduce
// it, then checks the category flag and the model-orientation frame (the game
// draws a model with its -Y along the flight).

#include "simulation/ProjectileShot.h"

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

// The game's loop: advance, add to the distance, destroy once it reaches range.
static int GameFramesAlive(float speed, float range)
{
    float travelled = 0.0f;
    int   frames    = 0;
    do
    {
        travelled += speed;
        ++frames;
    } while (travelled < range);
    return frames;
}

int main()
{
    std::printf("[flight]\n");
    {
        const float    aim[3] = {0, 0, 0};
        ProjectileShot s;
        CHECK(ComputeProjectileShot(11.0f, 500.0f, aim, s), "stock laser flies");
        CHECK(Near(s.velocity[0], 330.0, 1e-3) && s.velocity[1] == 0 && s.velocity[2] == 0,
              "zero aim -> +X at 30 x Max_Speed (330 u/s)");
        CHECK(Near(s.flightSec * 30.0, GameFramesAlive(11.0f, 500.0f), 1e-3),
              "lifetime = the game's frames alive (46 for 500 / 11)");

        const float speeds[] = {1.0f, 7.5f, 11.0f, 25.0f, 400.0f};
        const float ranges[] = {0.0f, 10.0f, 50.0f, 500.0f, 4000.0f};
        bool        allMatch = true;
        for (float sp : speeds)
            for (float rg : ranges)
            {
                ComputeProjectileShot(sp, rg, aim, s);
                if (!Near(s.flightSec * 30.0, GameFramesAlive(sp, rg), 1e-3)) allMatch = false;
            }
        CHECK(allMatch, "lifetime matches the stepped game across speeds and ranges (one frame minimum)");

        const float diag[3] = {3, 4, 0};
        ComputeProjectileShot(10.0f, 100.0f, diag, s);
        CHECK(Near(s.velocity[0], 180.0, 1e-3) && Near(s.velocity[1], 240.0, 1e-3),
              "aim only sets direction; speed comes from Max_Speed");

        CHECK(!ComputeProjectileShot(0.0f, 100.0f, aim, s), "zero speed can't fly");
        CHECK(!ComputeProjectileShot(-2.0f, 100.0f, aim, s), "negative speed can't fly");
        CHECK(!ComputeProjectileShot(NAN, 100.0f, aim, s), "NaN speed can't fly");
    }

    std::printf("[orientation]\n");
    {
        auto orthonormal = [](const float x[3], const float y[3], const float z[3])
        {
            auto dot = [](const float a[3], const float b[3])
            {
                return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            };
            const float cz[3] = {x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0]};
            return Near(dot(x, x), 1, 1e-5) && Near(dot(y, y), 1, 1e-5) && Near(dot(z, z), 1, 1e-5) &&
                   Near(dot(x, y), 0, 1e-5) && Near(dot(y, z), 0, 1e-5) && Near(dot(x, z), 0, 1e-5) &&
                   Near(cz[0], z[0], 1e-5) && Near(cz[1], z[1], 1e-5) && Near(cz[2], z[2], 1e-5);
        };
        float x[3], y[3], z[3];

        const float plusX[3] = {5, 0, 0};
        ProjectileShotBasis(plusX, x, y, z);
        CHECK(x[0] == 1 && y[1] == 1 && z[2] == 1, "+X flight -> identity flight frame");

        const float slant[3] = {1, 2, 0.5f};
        ProjectileShotBasis(slant, x, y, z);
        const float len = std::sqrt(1 + 4 + 0.25f);
        CHECK(Near(x[0], 1 / len, 1e-5) && Near(x[1], 2 / len, 1e-5) && Near(x[2], 0.5 / len, 1e-5),
              "flight frame +X points along the flight");
        CHECK(orthonormal(x, y, z) && z[2] > 0, "right-handed flight frame with its up kept upward");

        const float up[3] = {0, 0, 3};
        ProjectileShotBasis(up, x, y, z);
        CHECK(x[2] == 1 && orthonormal(x, y, z), "straight-up flight still gets a valid frame");

        const float none[3] = {0, 0, 0};
        ProjectileShotBasis(none, x, y, z);
        CHECK(x[0] == 1 && y[1] == 1 && z[2] == 1, "zero direction -> identity");

        // The game turns every model +90 deg about Z after facing, so model -Y flies forward.
        ProjectileShotModelAxes(plusX, x, y, z);
        CHECK(x[1] == 1 && y[0] == -1 && z[2] == 1, "+X flight: model X -> world +Y, model Y -> world -X");
        ProjectileShotModelAxes(slant, x, y, z);
        CHECK(Near(-y[0], 1 / len, 1e-5) && Near(-y[1], 2 / len, 1e-5) && Near(-y[2], 0.5 / len, 1e-5),
              "model -Y points along the flight");
        CHECK(orthonormal(x, y, z), "model axes stay right-handed");
    }

    std::printf(g_fail ? "\n=== ProjectileShot: %d FAILED ===\n" : "\n=== ProjectileShot: ALL PASS ===\n", g_fail);
    return g_fail ? 1 : 0;
}
