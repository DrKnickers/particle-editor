#include "host/ReplayHostileCrt.h"
#include "simulation/ParticleRandom.h"
#include <cstdio>
#include <initializer_list>

int main()
{
    int      failed = 0;
    uint64_t calls  = 99;
    if (RunReplayHostileCrt(0, calls) || calls != 0) ++failed;
    for (uint32_t iterations : {1u, 37u, 301u})
    {
        ParticleRandom::Seed(0x5EEDu);
        const auto state = ParticleRandom::StateValue();
        if (!RunReplayHostileCrt(iterations, calls) || calls != iterations + 1) ++failed;
        if (ParticleRandom::DrawCount() != 0 || ParticleRandom::StateValue() != state) ++failed;
    }
    printf("replay_hostile_crt: %s\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
