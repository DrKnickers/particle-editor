// Unit test for ParseParticleMipFilter (src/ParticleMipFilter.h) — the
// ALO_PARTICLE_MIPFILTER override parser behind the #481 particle mip-sampling
// bracket. Pure header, no D3D. Contract under test:
//   unset/empty        -> MODE_NONE, recognized
//   "linear" (any case)-> MODE_LINEAR, recognized
//   "bias:<finite f>"  -> MODE_BIAS + value, recognized
//   anything else      -> MODE_NONE, NOT recognized (caller logs the typo)
#include "ParticleMipFilter.h"
#include <cstdio>
#include <cmath>

static int g_fail = 0;
static void check(const char* input, int mode, float bias, bool recognized)
{
    const ParticleMipFilterMode m = ParseParticleMipFilter(input);
    const bool ok = m.mode == mode
                 && std::fabs(m.bias - bias) < 1e-6f
                 && m.recognized == recognized;
    if (!ok)
    {
        printf("  FAIL: ParseParticleMipFilter(%s) = {mode=%d bias=%g rec=%d}, "
               "expected {mode=%d bias=%g rec=%d}\n",
               input ? input : "(null)", m.mode, m.bias, (int)m.recognized,
               mode, bias, (int)recognized);
        ++g_fail;
    }
}

int main()
{
    // default forms
    check(nullptr,      ParticleMipFilterMode::MODE_NONE,   0.0f,  true);
    check("",           ParticleMipFilterMode::MODE_NONE,   0.0f,  true);
    // linear (rollback path), case-insensitive, EXACT match only
    check("linear",     ParticleMipFilterMode::MODE_LINEAR, 0.0f,  true);
    check("LINEAR",     ParticleMipFilterMode::MODE_LINEAR, 0.0f,  true);
    check("linearx",    ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    // bias with a finite float, full-suffix consumption required
    check("bias:-2",    ParticleMipFilterMode::MODE_BIAS,  -2.0f,  true);
    check("bias:1.5",   ParticleMipFilterMode::MODE_BIAS,   1.5f,  true);
    check("BIAS:0",     ParticleMipFilterMode::MODE_BIAS,   0.0f,  true);
    check("bias:",      ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("bias:abc",   ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("bias:1x",    ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("bias:inf",   ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("bias:nan",   ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("bias:1e40",  ParticleMipFilterMode::MODE_NONE,   0.0f,  false);  // strtof overflow -> inf -> rejected
    // legacy investigation spelling ("none") and garbage -> default, flagged
    check("none",       ParticleMipFilterMode::MODE_NONE,   0.0f,  false);
    check("trilinear",  ParticleMipFilterMode::MODE_NONE,   0.0f,  false);

    if (g_fail == 0) { printf("particle-mipfilter parse: ALL PASS\n"); return 0; }
    printf("particle-mipfilter parse: %d FAILED\n", g_fail);
    return 1;
}
