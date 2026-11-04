#include "ParticleRandom.h"

#ifdef _DEBUG
#include <cassert>
#include <thread>
#endif

namespace
{
std::uint32_t s_seed      = ParticleRandom::kDefaultSeed;
std::uint32_t s_state     = ParticleRandom::kDefaultSeed;
std::uint64_t s_drawCount = 0;
#ifdef _DEBUG
std::thread::id s_drawingThread;
#endif
} // namespace

namespace ParticleRandom
{
std::uint32_t LaunchSeed(bool fullyInteractive, std::uint32_t entropy)
{
    return fullyInteractive ? entropy * 214013u + 2531011u : kDefaultSeed;
}

void Seed(std::uint32_t seed)
{
    s_seed      = seed;
    s_state     = seed;
    s_drawCount = 0;
#ifdef _DEBUG
    s_drawingThread = std::this_thread::get_id();
#endif
}

int NextLegacyMsvcRand()
{
#ifdef _DEBUG
    // Unseeded draws belong to the first drawing thread.
    if (s_drawingThread == std::thread::id()) s_drawingThread = std::this_thread::get_id();
    assert(s_drawingThread == std::this_thread::get_id());
#endif
    s_state = s_state * 214013u + 2531011u; // unsigned wrap is intended
    ++s_drawCount;
    return static_cast<int>((s_state >> 16) & 0x7fffu);
}

float Unit()
{
    return static_cast<float>(NextLegacyMsvcRand()) / 32767.0f;
}

float Range(float minValue, float maxValue)
{
    // Dividing the span first preserves the legacy float rounding.
    return (maxValue - minValue) / 32767.0f * NextLegacyMsvcRand() + minValue;
}

std::uint32_t SeedValue()
{
    return s_seed;
}

std::uint32_t StateValue()
{
    return s_state;
}

std::uint64_t DrawCount()
{
    return s_drawCount;
}
} // namespace ParticleRandom

float GetRandom(float min, float max)
{
    return ParticleRandom::Range(min, max);
}
