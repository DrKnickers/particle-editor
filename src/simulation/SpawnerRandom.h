#ifndef SPAWNER_RANDOM_H
#define SPAWNER_RANDOM_H

#include "ParticleRandom.h"
#include <d3dx9math.h>

namespace SpawnerRandom
{
    inline D3DXVECTOR3 Jitter(const D3DXVECTOR3& r)
    {
        const auto axis = [](float radius) {
            if (radius <= 0.0f) return 0.0f; // a zero axis draws nothing
            float u = ParticleRandom::Unit();
            return (u * 2.0f - 1.0f) * radius; // preserve legacy rounding
        };
        // The shipped compiler evaluated the constructor arguments z, y, x.
        const float z = axis(r.z);
        const float y = axis(r.y);
        const float x = axis(r.x);
        return D3DXVECTOR3(x, y, z);
    }

    inline D3DXVECTOR3 SquigglePhase()
    {
        const auto phase = []() {
            float u = ParticleRandom::Unit();
            return u * (2.0f * 3.14159265358979323846f);
        };
        const float z = phase();
        const float y = phase();
        const float x = phase();
        return D3DXVECTOR3(x, y, z);
    }
}

#endif
