#include "host/ReplayHostileCrt.h"
#include <cstdlib>

bool RunReplayHostileCrt(std::uint32_t iterations, std::uint64_t& calls)
{
    calls = 0;
    if (iterations == 0) return false;
    std::srand(12345);
    ++calls;
    for (std::uint32_t i = 0; i < iterations; ++i)
    {
        volatile int value = std::rand();
        (void)value;
        ++calls;
    }
    return true;
}
