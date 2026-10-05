#ifndef HOST_PARTICLE_REPLAY_CONTROLLER_H
#define HOST_PARTICLE_REPLAY_CONTROLLER_H

#include "simulation/PreviewScenario.h"
#include <functional>

namespace host {

class ParticleReplayController
{
public:
    struct Hooks
    {
        std::function<bool()> admitDevice;
        std::function<void()> clearInstances;
        std::function<bool(const PreviewScenario&)> pinSettings;
        std::function<void(std::uint32_t)> seed;
        std::function<void(float)> freezeClock;
        std::function<bool(const PreviewScenario&)> armSpawn;
        std::function<void()> stepClock;
        std::function<bool()> tickSpawner;
        std::function<bool()> update;
        std::function<bool()> render;
        std::function<bool()> readback;
    };

    ParticleReplayController(const PreviewScenario&, Hooks);

    bool Reset();
    bool Advance(bool renderThisFrame);
    bool SeekTo(int frame, bool renderIntermediate);
    int  Frame() const { return m_frame; }

private:
    bool RenderAndReadback();

    PreviewScenario m_scenario;
    Hooks m_hooks;
    int m_frame = -1;  // No frame is accepted until reset succeeds.
};

}  // namespace host

#endif  // HOST_PARTICLE_REPLAY_CONTROLLER_H
