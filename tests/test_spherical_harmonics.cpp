// Unit test for SPH_Calculate_Matrices (src/SphericalHarmonics.cpp) -- the
// directional-light -> per-channel lighting-matrix encoder. Pure D3DX math,
// no device.
//
// The source has two compile-time branches selected by USE_PROPER_SPH
// (SphericalHarmonics.cpp:7): the shipped build leaves it UNDEFINED, so the
// classical fallback (lines 63-79) runs. To execute BOTH branches without
// touching src, this test:
//   - links src\SphericalHarmonics.cpp as-is  -> global  SPH_Calculate_Matrices
//     (classical fallback), and
//   - re-includes SphericalHarmonics.cpp inside a namespace with
//     USE_PROPER_SPH defined -> propersph::SPH_Calculate_Matrices (the
//     quadratic-form path, lines 18-61, exercising D3DXSHEvalDirectionalLight).
//
// Classical expectations are derived by hand from lines 63-79:
//   matrices[ch]._41.._43 = sum_i Diffuse[ch] * -Direction.xyz; everything
//   else stays zero except _44 += ambient[ch]*ambient.w ONCE PER LIGHT --
//   including that with ZERO lights the output is all-zero regardless of
//   ambient (the += lives inside the per-light loop; pinned deliberately).
//   Diffuse.w is ignored by the classical branch.
// Proper-SPH expectations are property-based (epsilon compares, not brittle
// floats): all-zero for zero lights, finiteness, the quadratic form's
// symmetry (_12==_21 etc., _11==-_22 from lines 41-59), channel equality for
// a white light, linearity in Diffuse.w, and the same per-light ambient
// accumulation into _44.
//
// Standalone console exe; see tests/build_test_spherical_harmonics.bat.

#include "SphericalHarmonics.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// Second compilation of the SAME source with the proper-SPH branch enabled
// (see header comment). The include guard makes the .h re-include a no-op, so
// only the function body is compiled here, against the global D3DX/Engine types.
#define USE_PROPER_SPH
namespace propersph {
#include "SphericalHarmonics.cpp"
}
#undef USE_PROPER_SPH

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

static bool feq(float a, float b, float eps = 1e-5f) { return std::fabs(a - b) < eps; }

static Engine::Light makeLight(float dx, float dy, float dz,
                               float r, float g, float b, float w)
{
    Engine::Light l;
    std::memset(&l, 0, sizeof(l));
    l.Direction = D3DXVECTOR4(dx, dy, dz, 0.0f);
    l.Diffuse   = D3DXVECTOR4(r, g, b, w);
    return l;
}

static bool allZero(const D3DXMATRIX* m)
{
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (m[k](i, j) != 0.0f) return false;
    return true;
}

static bool allFinite(const D3DXMATRIX* m)
{
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!std::isfinite(m[k](i, j))) return false;
    return true;
}

// Count entries that differ from zero outside the classical branch's _41.._44
// row (row index 3) -- the fallback must never write anywhere else.
static bool onlyRow4Written(const D3DXMATRIX* m)
{
    for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i)          // rows _1x.._3x only
            for (int j = 0; j < 4; ++j)
                if (m[k](i, j) != 0.0f) return false;
    return true;
}

int main()
{
    std::printf("test_spherical_harmonics\n");

    // =====================================================================
    // Classical fallback branch (global SPH_Calculate_Matrices)
    // =====================================================================
    std::printf("[classical]\n");

    // ---- zero lights: all-zero even with a non-zero ambient ----------------
    // (the ambient += sits INSIDE the per-light loop, lines 63,76-78).
    {
        D3DXMATRIX m[3];
        SPH_Calculate_Matrices(m, NULL, 0, D3DXVECTOR4(1.0f, 2.0f, 3.0f, 1.0f));
        CHECK(allZero(m), "0 lights: matrices all-zero, ambient contributes nothing");
    }

    // ---- one white light from +Z --------------------------------------------
    {
        Engine::Light l = makeLight(0, 0, 1, 1, 1, 1, 1);
        D3DXMATRIX m[3];
        SPH_Calculate_Matrices(m, &l, 1, D3DXVECTOR4(0.25f, 0.5f, 0.75f, 2.0f));
        CHECK(onlyRow4Written(m), "white +Z light: only the _4x row is written");
        bool rowOk = true;
        for (int k = 0; k < 3; ++k)
            rowOk = rowOk && m[k](3, 0) == 0.0f && m[k](3, 1) == 0.0f && m[k](3, 2) == -1.0f;
        CHECK(rowOk, "white +Z light: _41=_42=0, _43 == Diffuse * -Direction.z == -1 (all channels)");
        CHECK(m[0](3, 3) == 0.5f && m[1](3, 3) == 1.0f && m[2](3, 3) == 1.5f,
              "_44 == ambient[ch] * ambient.w per channel");
    }

    // ---- per-channel color x arbitrary direction ----------------------------
    // _4j = Diffuse[ch] * -Direction[j]; exact for these binary-exact inputs.
    {
        Engine::Light l = makeLight(1.0f, -2.0f, 3.0f, 0.25f, 0.5f, 1.0f, 123.0f);
        D3DXMATRIX m[3];
        SPH_Calculate_Matrices(m, &l, 1, D3DXVECTOR4(0, 0, 0, 0));
        CHECK(m[0](3, 0) == -0.25f && m[0](3, 1) == 0.5f && m[0](3, 2) == -0.75f,
              "red channel: (-0.25, 0.5, -0.75)");
        CHECK(m[1](3, 0) == -0.5f && m[1](3, 1) == 1.0f && m[1](3, 2) == -1.5f,
              "green channel: (-0.5, 1.0, -1.5)");
        CHECK(m[2](3, 0) == -1.0f && m[2](3, 1) == 2.0f && m[2](3, 2) == -3.0f,
              "blue channel: (-1.0, 2.0, -3.0)");

        // Diffuse.w must be ignored by the classical branch.
        Engine::Light l2 = makeLight(1.0f, -2.0f, 3.0f, 0.25f, 0.5f, 1.0f, 0.0f);
        D3DXMATRIX m2[3];
        SPH_Calculate_Matrices(m2, &l2, 1, D3DXVECTOR4(0, 0, 0, 0));
        CHECK(std::memcmp(m, m2, sizeof(m)) == 0, "classical branch ignores Diffuse.w");
    }

    // ---- linearity in Diffuse ------------------------------------------------
    {
        Engine::Light a = makeLight(0.5f, 0.25f, -1.0f, 0.5f, 0.25f, 0.125f, 1.0f);
        Engine::Light b = a; b.Diffuse.x *= 2; b.Diffuse.y *= 2; b.Diffuse.z *= 2;
        D3DXMATRIX ma[3], mb[3];
        SPH_Calculate_Matrices(ma, &a, 1, D3DXVECTOR4(0, 0, 0, 0));
        SPH_Calculate_Matrices(mb, &b, 1, D3DXVECTOR4(0, 0, 0, 0));
        bool lin = true;
        for (int k = 0; k < 3 && lin; ++k)
            for (int j = 0; j < 3; ++j)
                lin = lin && mb[k](3, j) == 2.0f * ma[k](3, j);
        CHECK(lin, "doubling Diffuse doubles every direction term (exact)");
    }

    // ---- two lights: sums, and ambient accumulates PER LIGHT ----------------
    {
        Engine::Light l[2] = {
            makeLight(1, 0, 0, 1, 1, 1, 1),
            makeLight(0, 1, 0, 0.5f, 0.5f, 0.5f, 1),
        };
        D3DXMATRIX m[3];
        SPH_Calculate_Matrices(m, l, 2, D3DXVECTOR4(0.5f, 0.25f, 0.125f, 1.0f));
        CHECK(m[0](3, 0) == -1.0f && m[0](3, 1) == -0.5f && m[0](3, 2) == 0.0f,
              "two lights: direction terms sum");
        CHECK(m[0](3, 3) == 1.0f && m[1](3, 3) == 0.5f && m[2](3, 3) == 0.25f,
              "two lights: ambient added once per light (2x) -- pins the loop quirk");
    }

    // =====================================================================
    // Proper-SPH branch (propersph::SPH_Calculate_Matrices)
    // =====================================================================
    std::printf("[proper-sph]\n");

    // ---- zero lights: all-zero -----------------------------------------------
    {
        D3DXMATRIX m[3];
        propersph::SPH_Calculate_Matrices(m, NULL, 0, D3DXVECTOR4(1, 1, 1, 1));
        CHECK(allZero(m), "proper: 0 lights -> all-zero (ambient loop never runs)");
    }

    // ---- one white light: finite, symmetric quadratic form, equal channels --
    {
        Engine::Light l = makeLight(0.0f, 0.0f, -1.0f, 1, 1, 1, 1);
        D3DXMATRIX m[3];
        propersph::SPH_Calculate_Matrices(m, &l, 1, D3DXVECTOR4(0, 0, 0, 0));
        CHECK(allFinite(m), "proper: all entries finite");
        CHECK(!allZero(m), "proper: a real light produces a non-zero form");
        bool sym = true;
        for (int k = 0; k < 3 && sym; ++k)
        {
            const D3DXMATRIX& q = m[k];
            sym = q(0, 1) == q(1, 0) && q(0, 2) == q(2, 0) && q(1, 2) == q(2, 1)
               && q(0, 3) == q(3, 0) && q(1, 3) == q(3, 1) && q(2, 3) == q(3, 2);
        }
        CHECK(sym, "proper: quadratic form symmetric (built from shared coeffs, lines 41-59)");
        CHECK(m[0](0, 0) == -m[0](1, 1), "proper: _11 == -_22 (c1*L22 vs -c1*L22)");
        CHECK(std::memcmp(&m[0], &m[1], sizeof(D3DXMATRIX)) == 0
           && std::memcmp(&m[1], &m[2], sizeof(D3DXMATRIX)) == 0,
              "proper: white light -> identical R/G/B matrices");
        CHECK(m[0](3, 3) != 0.0f, "proper: DC/L20 term reaches _44");
        CHECK(m[0](2, 3) != 0.0f, "proper: z-aligned light populates the L10 (_34/_43) terms");
    }

    // ---- linearity in the intensity term (Diffuse.w scales the SH input) ----
    {
        Engine::Light a = makeLight(0.25f, 0.5f, -1.0f, 0.75f, 0.5f, 0.25f, 1.0f);
        Engine::Light b = a; b.Diffuse.w = 2.0f;
        D3DXMATRIX ma[3], mb[3];
        propersph::SPH_Calculate_Matrices(ma, &a, 1, D3DXVECTOR4(0, 0, 0, 0));
        propersph::SPH_Calculate_Matrices(mb, &b, 1, D3DXVECTOR4(0, 0, 0, 0));
        bool lin = true;
        for (int k = 0; k < 3 && lin; ++k)
            for (int i = 0; i < 4 && lin; ++i)
                for (int j = 0; j < 4; ++j)
                    lin = lin && feq(mb[k](i, j), 2.0f * ma[k](i, j),
                                     1e-4f * (1.0f + std::fabs(ma[k](i, j))));
        CHECK(lin, "proper: doubling Diffuse.w doubles the whole form (epsilon)");
    }

    // ---- ambient still accumulates into _44 once per light ------------------
    {
        Engine::Light l = makeLight(0, 1, 0, 0.5f, 0.5f, 0.5f, 1.0f);
        D3DXMATRIX base[3], amb[3];
        propersph::SPH_Calculate_Matrices(base, &l, 1, D3DXVECTOR4(0, 0, 0, 0));
        propersph::SPH_Calculate_Matrices(amb,  &l, 1, D3DXVECTOR4(0.5f, 0.25f, 0.0f, 2.0f));
        CHECK(feq(amb[0](3, 3), base[0](3, 3) + 1.0f)
           && feq(amb[1](3, 3), base[1](3, 3) + 0.5f)
           && feq(amb[2](3, 3), base[2](3, 3)),
              "proper: _44 gains ambient[ch]*ambient.w on top of the SH term");
        // Every other entry must be untouched by ambient.
        bool rest = true;
        for (int k = 0; k < 3 && rest; ++k)
            for (int i = 0; i < 4 && rest; ++i)
                for (int j = 0; j < 4; ++j)
                    if (i != 3 || j != 3) rest = rest && amb[k](i, j) == base[k](i, j);
        CHECK(rest, "proper: ambient touches only _44");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
