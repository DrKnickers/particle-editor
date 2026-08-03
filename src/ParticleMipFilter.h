#pragma once
#include <cstdlib>
#include <cstring>
#include <cmath>

// #481: ALO_PARTICLE_MIPFILTER env-override parser for the particle draw
// bracket's mip sampling (engine.cpp, non-heat particle loop).
//
//   unset / "" / unrecognized -> MODE_NONE   (mip 0 only — the default)
//   "linear"                  -> MODE_LINEAR (pre-#481 trilinear, the rollback/A-B path)
//   "bias:<f>" (finite float) -> MODE_BIAS   (trilinear + LOD bias <f>)
//
// Pure + header-only so tests/test_particle_mipfilter.cpp can unit-test it
// without a D3D device. `recognized` is false only for a non-empty value that
// matched no form (caller logs it once — a typo'd override silently behaving
// like the default was review finding #3 on the #481 plan).
struct ParticleMipFilterMode
{
    enum Mode { MODE_NONE = 0, MODE_LINEAR = 1, MODE_BIAS = 2 };
    int   mode       = MODE_NONE;
    float bias       = 0.0f;
    bool  recognized = true;
};

inline ParticleMipFilterMode ParseParticleMipFilter(const char* value)
{
    ParticleMipFilterMode out;
    if (value == nullptr || value[0] == '\0') return out;          // unset -> default
    if (_stricmp(value, "linear") == 0)
    {
        out.mode = ParticleMipFilterMode::MODE_LINEAR;
        return out;
    }
    if (_strnicmp(value, "bias:", 5) == 0)
    {
        char* end = nullptr;
        const float f = strtof(value + 5, &end);
        // Require the whole suffix consumed and a finite value — "bias:garbage"
        // (strtof -> 0, end == start) and "bias:1x" both fall through to the
        // unrecognized default rather than silently biasing by 0/1.
        if (end != value + 5 && *end == '\0' && std::isfinite(f))
        {
            out.mode = ParticleMipFilterMode::MODE_BIAS;
            out.bias = f;
            return out;
        }
    }
    out.recognized = false;                                        // typo -> default + log
    return out;
}
