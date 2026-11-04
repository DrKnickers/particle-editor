#include "ParticleReplayController.h"
#include <utility>

namespace host
{

ParticleReplayController::ParticleReplayController(const PreviewScenario& scenario, Hooks hooks)
    : m_scenario(scenario), m_hooks(std::move(hooks))
{
}

bool ParticleReplayController::Reset()
{
    if (!m_hooks.admitDevice || !m_hooks.admitDevice()) return false;
    if (!m_hooks.pinSettings || !m_hooks.pinSettings(m_scenario)) return false;
    if (!m_hooks.clearInstances) return false;
    m_hooks.clearInstances();
    if (!m_hooks.seed) return false;
    m_hooks.seed(m_scenario.seed);
    if (!m_hooks.freezeClock) return false;
    m_hooks.freezeClock(m_scenario.clockAnchor);
    if (!m_hooks.armSpawn || !m_hooks.armSpawn(m_scenario)) return false;
    if (!m_hooks.tickSpawner || !m_hooks.tickSpawner()) return false;
    if (!m_hooks.update || !m_hooks.update()) return false;
    m_frame = 0;
    return true;
}

bool ParticleReplayController::RenderAndReadback()
{
    if (!m_hooks.render || !m_hooks.render()) return false;
    return !m_hooks.readback || m_hooks.readback();
}

bool ParticleReplayController::Advance(bool renderThisFrame)
{
    if (m_frame < 0 || m_frame >= m_scenario.maxFrames) return false;
    if (!m_hooks.admitDevice || !m_hooks.admitDevice()) return false;
    if (!m_hooks.stepClock) return false;
    m_hooks.stepClock();
    if (!m_hooks.tickSpawner || !m_hooks.tickSpawner()) return false;
    if (!m_hooks.update || !m_hooks.update()) return false;
    if (renderThisFrame && !RenderAndReadback()) return false;
    ++m_frame;
    return true;
}

bool ParticleReplayController::SeekTo(int frame, bool renderIntermediate)
{
    if (frame < 0 || frame > m_scenario.maxFrames) return false;
    const int acceptedFrame = m_frame;
    bool      ok            = Reset();
    for (int i = 1; ok && i <= frame; ++i) ok = Advance(i == frame || renderIntermediate);
    if (ok && frame == 0) ok = RenderAndReadback();
    // Hooks mutate the simulation, but a failed seek has no accepted destination.
    if (!ok) m_frame = acceptedFrame;
    return ok;
}

} // namespace host
