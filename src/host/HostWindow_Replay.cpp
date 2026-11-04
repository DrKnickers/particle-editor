// Replay hooks: the proof and observation runs (golden profile) and the
// editor's timeline (interactive pin) build every replay from these.
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "HostWindowImpl.h"
#include "simulation/ParticleRandom.h"

namespace host
{

ParticleReplayController::Hooks HostWindowImpl::BuildReplayHooks(const std::wstring& readbackPath,
                                                                 SpawnPolicy         policy,
                                                                 PinMode             pinMode)
{
    ParticleReplayController::Hooks hooks;
    auto                            spawnPending = std::make_shared<bool>(false);
    hooks.admitDevice                            = [this]()
    {
        return engine && engine->PrepareDeviceForFrame();
    };
    hooks.pinSettings = [this, pinMode](const PreviewScenario& scenario)
    {
        if (!engine || (pinMode == PinMode::Golden && !m_fixedCaptureView)) return false;
        engine->SetOverloadGuard(scenario.overloadGuardEnabled, scenario.overloadGuardLimit);
        engine->SetEstimatedLoad(scenario.estimatedParticlesPerInstance);
        // The timeline keeps the user's view. The camera is still a replay
        // input (weather placement, billboard baking), so the timeline
        // re-replays when it moves instead of pinning it here.
        if (pinMode == PinMode::Interactive) return !engine->DeviceCallsBlocked();
        if (!m_fixedCaptureView()) return false;
        engine->SetMsaaLevel(0);
        engine->SetSkydomeEnvironment(engine->GetSkydomeContext(), "", "");
        const bool skydomeOk = engine->SetEmbeddedSkydomeSlotForCapture(1);
        engine->SetGround(false);
        return skydomeOk && !engine->GetGround() && engine->GetSkydomeSlot() == 1 &&
               engine->IsSkydomeUsingEmbeddedResource() && engine->GetSkydomePrimaryName().empty() &&
               engine->GetSkydomeSecondaryName().empty() && !engine->SkydomePrimaryHasGpuBuffers() &&
               !engine->SkydomeSecondaryHasGpuBuffers() && engine->SkydomeMeshHasGpuBuffers() &&
               !engine->DeviceCallsBlocked();
    };
    hooks.clearInstances = [this]()
    {
        engine->Clear();
        engine->InvalidatePausedIdleSkip();
        engine->TakeSpawnRefusal(nullptr);
    };
    hooks.seed = [](std::uint32_t seed)
    {
        ParticleRandom::Seed(seed);
    };
    hooks.freezeClock = [](float anchor)
    {
        FreezePreviewClockAt(anchor);
    };
    hooks.armSpawn = [this, spawnPending](const PreviewScenario& scenario)
    {
        // Undo replaces this unique_ptr: never retain a document pointer across hooks.
        if (!engine || !spawnerDriver || !particleSystem) return false;
        spawnerDriver->Reset();
        SpawnerConfig config;
        config.mode              = SpawnerConfig::Mode::Manual;
        config.burstSize         = 1;
        config.position          = scenario.spawnPosition;
        config.velocity          = scenario.spawnVelocity;
        config.jitterPosition    = D3DXVECTOR3(0, 0, 0);
        config.acceleration      = D3DXVECTOR3(0, 0, 0);
        config.squiggleAmplitude = D3DXVECTOR3(0, 0, 0);
        config.squiggleFrequency = 0.0f;
        config.maxLifetimeSec    = 0.0f;
        spawnerDriver->SetConfig(config);
        spawnerDriver->Trigger(particleSystem.get(), engine.get());
        *spawnPending = true;
        return !engine->DeviceCallsBlocked();
    };
    hooks.stepClock = []()
    {
        StepPreviewFrames(1);
    };
    hooks.tickSpawner = [this, spawnPending, policy]()
    {
        if (!engine || !spawnerDriver || !particleSystem || engine->DeviceCallsBlocked()) return false;
        spawnerDriver->Tick(1.0f / 60.0f, particleSystem.get(), engine.get());
        if (*spawnPending)
        {
            *spawnPending = false;
            if (policy == SpawnPolicy::Strict)
            {
                const bool refused = engine->TakeSpawnRefusal(nullptr);
                if (refused || engine->ActiveSpawnerInstanceCount() != 1 || engine->GetNumInstances() != 1)
                    return false;
            }
            // Observe leaves an empty/refused spawn to the counters and quiet tail.
        }
        return !engine->DeviceCallsBlocked();
    };
    hooks.update = [this]()
    {
        if (!engine) return false;
        const uint64_t before = engine->CompletedFullUpdateCountForObservation();
        engine->Update();
        return engine->CompletedFullUpdateCountForObservation() == before + 1;
    };
    hooks.render = [this]()
    {
        return RenderComposedFrame(false);
    };
    if (!readbackPath.empty())
    {
        hooks.readback = [this, readbackPath]()
        {
            return engine && !engine->DeviceCallsBlocked() && alphaCompositor &&
                   alphaCompositor->CaptureSnapshotToFile(readbackPath);
        };
    }
    return hooks;
}

bool HostWindowImpl::TimelineOwnsSimulation() const
{
    return dispatcher && dispatcher->TimelineActive();
}

} // namespace host
