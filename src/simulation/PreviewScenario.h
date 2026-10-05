#ifndef PREVIEW_SCENARIO_H
#define PREVIEW_SCENARIO_H

#include "ParticleRandom.h"
#include <d3dx9math.h>
#include <cmath>
#include <string>

struct PreviewScenario
{
    int           schemaVersion = 1;
    std::wstring  aloPath;
    std::uint32_t seed = ParticleRandom::kAutomationSeed;
    std::uint32_t algorithmVersion = ParticleRandom::kAlgorithmVersion;
    float         clockAnchor = 0.0f;
    D3DXVECTOR3   spawnPosition{0, 0, 0};
    D3DXVECTOR3   spawnVelocity{0, 0, 0};
    int           maxFrames = 600;
    bool          overloadGuardEnabled = true;
    int           overloadGuardLimit = 10000;
    double        estimatedParticlesPerInstance = 0.0;
};

// Empty string when valid; otherwise the first problem, in plain words.
inline std::string ValidatePreviewScenario(const PreviewScenario& scenario)
{
    if (scenario.schemaVersion != 1) return "schemaVersion must be 1";
    if (scenario.algorithmVersion != ParticleRandom::kAlgorithmVersion)
        return "algorithmVersion must match ParticleRandom::kAlgorithmVersion";
    if (scenario.aloPath.empty()) return "aloPath must not be empty";
    if (!std::isfinite(scenario.clockAnchor) || scenario.clockAnchor != 0.0f)
        return "clockAnchor must be finite and exactly zero";
    if (scenario.maxFrames < 1 || scenario.maxFrames > 36000)
        return "maxFrames must be within [1, 36000]";
    // Match Engine's guard clamp without pulling the device-dependent host in.
    if (scenario.overloadGuardLimit < 1000 || scenario.overloadGuardLimit > 1000000)
        return "overloadGuardLimit must be within [1000, 1000000]";
    if (!std::isfinite(scenario.estimatedParticlesPerInstance) ||
        scenario.estimatedParticlesPerInstance < 0.0)
        return "estimatedParticlesPerInstance must be finite and not negative";
    const D3DXVECTOR3 vectors[] = { scenario.spawnPosition, scenario.spawnVelocity };
    const char* fields[] = { "spawnPosition", "spawnVelocity" };
    const char* axes[] = { ".x", ".y", ".z" };
    for (int v = 0; v < 2; ++v)
    {
        const float components[] = { vectors[v].x, vectors[v].y, vectors[v].z };
        for (int axis = 0; axis < 3; ++axis)
        {
            if (!std::isfinite(components[axis]) ||
                components[axis] < -10000.0f || components[axis] > 10000.0f)
                return std::string(fields[v]) + axes[axis] + " must be finite and within [-10000, 10000]";
        }
    }
    return "";
}

#endif
