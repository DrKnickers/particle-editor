#ifndef PARTICLE_RANDOM_H
#define PARTICLE_RANDOM_H

#include <cstdint>

// A pinned legacy MSVC stream keeps seeded particle output compatible without
// sharing random state with anything else in the process.
namespace ParticleRandom
{
    inline constexpr std::uint32_t kAlgorithmVersion = 1;
    inline constexpr int           kLegacyRandMax    = 32767;
    inline constexpr std::uint32_t kDefaultSeed      = 1;        // the CRT's unseeded state
    inline constexpr std::uint32_t kAutomationSeed   = 0x5EEDu;  // --capture and --record

    // Interactive launches start after one legacy step; automation keeps seed 1.
    std::uint32_t LaunchSeed(bool fullyInteractive, std::uint32_t entropy);

    // Restart the stream and zero its draw count.
    void Seed(std::uint32_t seed);

    // Each call draws once; raw values are in [0, 32767].
    int NextLegacyMsvcRand();
    float Unit(); // float(raw) / 32767.0f
    float Range(float minValue, float maxValue); // span / 32767.0f * raw + minValue

    std::uint32_t SeedValue();
    std::uint32_t StateValue();
    std::uint64_t DrawCount();
}

// The emitter-facing seam, unchanged in name and signature.
float GetRandom(float min, float max);

#endif
