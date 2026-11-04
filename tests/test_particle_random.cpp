// Pinned sequences and float bits are the compatibility authority; live CRT
// comparisons also catch a toolchain whose legacy recurrence has changed.

#include "simulation/ParticleRandom.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

static std::uint32_t FloatBits(float value)
{
    std::uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "fixtures require 32-bit floats");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

int main()
{
    using namespace ParticleRandom;

    // This must precede every Seed call to exercise the module's default state.
    CHECK(kAlgorithmVersion == 1 && kLegacyRandMax == 32767 && kDefaultSeed == 1 && kAutomationSeed == 0x5EEDu,
          "pinned constants");
    CHECK(SeedValue() == 1 && StateValue() == 1 && DrawCount() == 0, "unseeded state");
    CHECK(NextLegacyMsvcRand() == 41, "unseeded first draw is 41");
    CHECK(StateValue() == 0x0029E2C0u && DrawCount() == 1, "unseeded first step state and count");

    const std::uint32_t seeds[]         = {0, 1, 0x5EEDu, 0xFFFFFFFFu};
    const int           sequences[][16] = {
        {38, 7719, 21238, 2437, 8855, 11797, 8365, 32285, 10450, 30612, 5853, 28100, 1142, 281, 20537, 15921},
        {41, 18467, 6334, 26500, 19169, 15724, 11478, 29358, 26962, 24464, 5705, 28145, 23281, 16827, 9961, 491},
        {13859, 11015, 28005, 20064, 20896, 14843, 15493, 3138, 14446, 15730, 16065, 17134, 12936, 29191, 16532, 17922},
        {35, 29739, 3374, 11141, 31308, 7870, 5253, 2445, 26706, 3992, 6001, 28056, 11771, 16503, 31113, 31351}};
    for (int s = 0; s < 4; ++s)
    {
        Seed(seeds[s]);
        for (int i = 0; i < 16; ++i) CHECK(NextLegacyMsvcRand() == sequences[s][i], "appendix B raw sequence");

        Seed(seeds[s]);
        std::srand(seeds[s]);
        bool equal = true;
        for (int i = 0; i < 10000; ++i)
        {
            const int raw = NextLegacyMsvcRand();
            const int crt = std::rand();
            if (raw != crt) equal = false;
        }
        CHECK(equal, "10000 draws equal the live CRT");
        CHECK(DrawCount() == 10000, "CRT comparison draw count");
    }

    const std::uint32_t launchStates[] = {0x00269EC3u, 0x0029E2C0u, 0x362375FCu, 0x00235AC6u};
    Seed(kAutomationSeed);
    (void)NextLegacyMsvcRand();
    const std::uint32_t savedState = StateValue();
    for (int s = 0; s < 4; ++s)
    {
        CHECK(LaunchSeed(true, seeds[s]) == launchStates[s], "pinned interactive launch state");
        CHECK(LaunchSeed(false, seeds[s]) == kDefaultSeed, "automation launch keeps default seed");
        CHECK(SeedValue() == kAutomationSeed && StateValue() == savedState && DrawCount() == 1,
              "launch policy leaves seed, state and count unchanged");
    }
    for (std::uint32_t entropy : seeds)
    {
        Seed(LaunchSeed(true, entropy));
        CHECK(DrawCount() == 0, "launch step is not counted as a draw");
        std::srand(entropy);
        (void)std::rand();
        bool equal = true;
        for (int i = 0; i < 10000; ++i)
        {
            const int raw = NextLegacyMsvcRand();
            const int crt = std::rand();
            if (raw != crt) equal = false;
        }
        CHECK(equal, "interactive launch equals CRT after one discarded draw");
    }
    Seed(LaunchSeed(true, 0x5EEDu));
    const int stepped[] = {11015, 28005, 20064, 20896};
    for (int raw : stepped) CHECK(NextLegacyMsvcRand() == raw, "stepped automation-seed sequence");

    const std::uint32_t unitBits[] = {0x3ED88DB1u, 0x3EAC1D58u, 0x3F5ACBB6u, 0x3F1CC13Au, 0x3F234147u};
    Seed(kAutomationSeed);
    for (int i = 0; i < 5; ++i)
    {
        const std::uint32_t bits = FloatBits(Unit());
        CHECK(bits == unitBits[i], i == 4 ? "Unit draw 5 division bits" : "Unit draws 1-4 bits");
    }
    const std::uint32_t rangeBits[] = {0x3ED88DB1u, 0x3EAC1D58u, 0x3F5ACBB6u, 0x3F1CC13Au, 0x3F234146u};
    Seed(kAutomationSeed);
    for (int i = 0; i < 5; ++i)
    {
        const std::uint32_t bits = FloatBits(Range(0.0f, 1.0f));
        CHECK(bits == rangeBits[i], i == 4 ? "Range draw 5 expression-order bits" : "Range draws 1-4 bits");
    }
    const std::uint32_t signedRangeBits[] = {0xBE1DC93Cu, 0xBEA7C550u, 0x3F35976Cu, 0x3E6609D0u};
    Seed(kAutomationSeed);
    for (std::uint32_t bits : signedRangeBits) CHECK(FloatBits(Range(-1.0f, 1.0f)) == bits, "Range(-1, 1) bits");

    Seed(kAutomationSeed);
    CHECK(SeedValue() == kAutomationSeed && StateValue() == kAutomationSeed && DrawCount() == 0,
          "Seed restarts state and zeroes count");
    (void)NextLegacyMsvcRand();
    CHECK(StateValue() == 0x362375FCu && DrawCount() == 1, "raw draw advances state and count once");
    (void)Unit();
    CHECK(DrawCount() == 2, "Unit adds one draw");
    (void)Range(-1.0f, 1.0f);
    CHECK(DrawCount() == 3, "Range adds one draw");
    (void)GetRandom(-1.0f, 1.0f);
    CHECK(DrawCount() == 4, "GetRandom adds one draw");
    Seed(0);
    CHECK(SeedValue() == 0 && StateValue() == 0 && DrawCount() == 0, "reseed zeroes count");

    // CRT users cannot perturb the emitter-facing stream.
    const int hostileDraws[] = {0, 1, 7, 1000};
    for (int draws : hostileDraws)
    {
        Seed(kAutomationSeed);
        CHECK(FloatBits(GetRandom(0.0f, 1.0f)) == 0x3ED88DB1u, "GetRandom first draw bits");
        std::srand(12345);
        for (int i = 0; i < draws; ++i) (void)std::rand();
        CHECK(FloatBits(GetRandom(0.0f, 1.0f)) == 0x3EAC1D58u, "hostile CRT leaves GetRandom second draw unchanged");
        CHECK(DrawCount() == 2, "hostile CRT adds no particle draws");
    }

    int firstRun[16];
    Seed(kAutomationSeed);
    for (int& raw : firstRun) raw = NextLegacyMsvcRand();
    Seed(kAutomationSeed);
    bool reproduced = true;
    for (int raw : firstRun)
        if (NextLegacyMsvcRand() != raw) reproduced = false;
    CHECK(reproduced, "reseeding reproduces the sequence");
    Seed(kDefaultSeed);
    bool diverged = false;
    for (int raw : firstRun)
        if (NextLegacyMsvcRand() != raw) diverged = true;
    CHECK(diverged, "different seeds diverge");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
