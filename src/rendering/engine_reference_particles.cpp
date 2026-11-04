// Reference-object particles (#964).
//
// Some projectiles are drawn entirely by particle effects: their model is a
// particle-system .alo, or a skeleton whose bones carry particle proxies. Mesh
// projectiles can carry proxies too. This file runs those effects as part of the
// reference object, beside the edited effect but never part of it:
//   - Sources are read-only ParticleSystems loaded from the game's files, each with
//     its object-space position on the model (a proxy's bone, or the origin).
//   - Runs are live instances of a source, kept in their own list so edits, the
//     spawner cap and the observation tooling never see them. Each run follows an
//     anchor it owns, so nothing ever points at a shot that has been destroyed.
//   - A resting run sits at the reference placement and restarts when it finishes.
//     In match-projectile mode, each live shot gets its own runs, which stop emitting
//     (and fade) when the shot ends, as the game's do when it destroys the shot.
// Particles carry no orientation in the preview, so only positions are followed.
// Rendering uses the same particle path as the edited effect (the game's shaders).

#include "engine.h"
#include "common/exceptions.h"
#include "common/files.h"
#include "gamedata/managers.h"
#include "ReferenceObjectMesh.h"                 // ReadReferenceParticleProxies
#include "simulation/ParticleSystemInstance.h"
#include "simulation/ProjectileShot.h"           // ProjectileShotModelAxes

namespace
{
    // One read-only particle system from the game's files; null on a miss, a file
    // that isn't a valid particle system, or one with no emitters (a resting run of
    // it would finish at once and restart every frame).
    std::unique_ptr<ParticleSystem> LoadReadOnlyParticleSystem(IFileManager& fm, const std::string& aloPath)
    {
        IFile* file = fm.getFile(aloPath);
        if (file == nullptr) return nullptr;
        std::unique_ptr<ParticleSystem> system;
        try
        {
            system = std::make_unique<ParticleSystem>(file);
        }
        catch (const wexception&)
        {
            system.reset();
        }
        file->Release();
        if (system && system->getEmitters().empty()) system.reset();
        return system;
    }

    D3DXVECTOR3 TransformPoint(const D3DXVECTOR3& p, const D3DXMATRIX& world)
    {
        D3DXVECTOR3 out;
        D3DXVec3TransformCoord(&out, &p, &world);
        return out;
    }
}

// Loads the particle sources for the selected projectile: the model itself when it
// is a particle system, otherwise its visible particle proxies. Non-projectiles get
// none (unit proxies are out of scope for now).
void Engine::LoadReferenceParticleSources(const GameObjectRef& ref, const std::string& aloPath)
{
    ClearReferenceParticles(/*keepSources=*/false);
    if (ref.role != ObjRole::Projectile) return;

    if (ref.modelContent == ModelContent::ParticleSystem)
    {
        std::unique_ptr<ParticleSystem> system = LoadReadOnlyParticleSystem(m_fileManager, aloPath);
        if (system)
            m_referenceParticleSources.push_back({ std::move(system), D3DXVECTOR3(0, 0, 0) });
        return;
    }

    for (const ReferenceParticleProxy& proxy : ReadReferenceParticleProxies(m_fileManager, aloPath))
    {
        std::unique_ptr<ParticleSystem> system = LoadReadOnlyParticleSystem(m_fileManager, proxy.aloPath);
        if (!system)
        {
#ifndef NDEBUG
            fprintf(stderr, "[RefParticles] proxy '%s' failed to load -- skipped\n", proxy.aloPath.c_str());
#endif
            continue;
        }
        m_referenceParticleSources.push_back({ std::move(system), proxy.offset });
    }
}

// Destroys every run, releasing its emitters and particles from the engine counts.
// `keepSources` keeps the loaded systems so resting runs restart on the next update
// (Engine::Clear); false also drops the sources (a new or cleared reference).
void Engine::ClearReferenceParticles(bool keepSources)
{
    for (ReferenceParticleRun& run : m_referenceParticleRuns)
    {
        m_numParticles += run.instance->Kill();
        m_numEmitters  -= (int)run.instance->NumLiveEmitters();
    }
    m_referenceParticleRuns.clear();
    m_referenceShotTokens.clear();
    if (!keepSources) m_referenceParticleSources.clear();
}

void Engine::UpdateReferenceParticles(TimeF currentTime)
{
    if (m_referenceParticleSources.empty()) return;

    const bool visible = m_referenceObjectVisible;
    const bool resting = visible && ReferenceRestsInScene();
    const D3DXMATRIX restWorld = ReferenceObjectDisplayWorld();

    auto startRun = [&](size_t sourceIdx, const D3DXVECTOR3& at, ParticleSystemInstanceHandle shot) {
        ReferenceParticleRun run;
        run.sourceIdx = sourceIdx;
        run.anchor   = std::make_unique<Object3D>(nullptr, at);
        run.instance = std::make_unique<ParticleSystemInstance>(
            *this, *m_referenceParticleSources[sourceIdx].system, run.anchor.get(), /*instanceToken=*/0);
        run.shot     = shot;
        m_referenceParticleRuns.push_back(std::move(run));
    };

    // Shots fired since the last update get their runs. A shot carries the runs at
    // its model's proxy positions, turned and scaled the way its model is drawn.
    if (visible && !ReferenceRestsInScene())
    {
        for (const auto& inst : m_instances)
        {
            if (!inst || !inst->IsProjectileShotLive()) continue;
            if (!m_referenceShotTokens.insert(inst->GetInstanceToken()).second) continue;
            const ParticleSystemInstanceHandle shot = m_instanceBorrows.Make(inst.get());
            for (size_t s = 0; s < m_referenceParticleSources.size(); ++s)
                startRun(s, inst->GetPosition(), shot);
        }
    }

    // Each run follows its anchor: the shot's model frame, or the resting placement.
    for (ReferenceParticleRun& run : m_referenceParticleRuns)
    {
        const D3DXVECTOR3& offset = m_referenceParticleSources[run.sourceIdx].offset;
        if (run.shot)
        {
            ParticleSystemInstance* shot = m_instanceBorrows.Resolve(run.shot);
            if (shot == nullptr || !shot->IsProjectileShotLive() || !visible)
            {
                // The game destroyed the shot: its particles stop and fade out.
                run.shot.Reset();
                run.instance->StopSpawning();
                run.instance->Detach();
                continue;
            }
            const D3DXVECTOR3& d = shot->GetShotDirection();
            const float dir[3] = { d.x, d.y, d.z };
            float mx[3], my[3], mz[3];
            ProjectileShotModelAxes(dir, mx, my, mz);
            const float s = m_referenceScaleFactor;
            const D3DXVECTOR3 turned(
                s * (offset.x * mx[0] + offset.y * my[0] + offset.z * mz[0]),
                s * (offset.x * mx[1] + offset.y * my[1] + offset.z * mz[1]),
                s * (offset.x * mx[2] + offset.y * my[2] + offset.z * mz[2]));
            run.anchor->MoveTo(shot->GetPosition() + turned, shot->GetVelocity());
        }
        else if (!run.instance->Detached())
        {
            if (!resting)
            {
                // Hidden, or now flying with the shots: the resting effect fades out.
                run.instance->StopSpawning();
                run.instance->Detach();
                continue;
            }
            run.anchor->MoveTo(TransformPoint(offset, restWorld));
        }
    }

    for (ReferenceParticleRun& run : m_referenceParticleRuns)
        m_numParticles += run.instance->Update(currentTime);

    // Retire finished runs. A resting run that finished is restarted below, so a
    // one-shot effect keeps replaying at the placement.
    for (auto it = m_referenceParticleRuns.begin(); it != m_referenceParticleRuns.end();)
    {
        if (it->instance->IsDead())
            it = m_referenceParticleRuns.erase(it);
        else
            ++it;
    }
    if (resting)
    {
        for (size_t s = 0; s < m_referenceParticleSources.size(); ++s)
        {
            bool hasRestingRun = false;
            for (const ReferenceParticleRun& run : m_referenceParticleRuns)
                if (run.sourceIdx == s && !run.shot && !run.instance->Detached()) { hasRestingRun = true; break; }
            if (!hasRestingRun)
                startRun(s, TransformPoint(m_referenceParticleSources[s].offset, restWorld),
                         ParticleSystemInstanceHandle());
        }
    }

    // Forget shots that no longer exist, so the token set stays small.
    for (auto it = m_referenceShotTokens.begin(); it != m_referenceShotTokens.end();)
    {
        bool alive = false;
        for (const auto& inst : m_instances)
            if (inst && inst->GetInstanceToken() == *it) { alive = true; break; }
        it = alive ? std::next(it) : m_referenceShotTokens.erase(it);
    }
}

void Engine::RenderReferenceParticles(IDirect3DDevice9* pDevice, bool heat)
{
    for (ReferenceParticleRun& run : m_referenceParticleRuns)
    {
        if (heat) run.instance->RenderHeat(pDevice);
        else      run.instance->RenderNormal(pDevice);
    }
}

bool Engine::ReferenceParticlesHaveLiveHeat() const
{
    for (const ReferenceParticleRun& run : m_referenceParticleRuns)
        if (run.instance->HasLiveHeat()) return true;
    return false;
}

void Engine::ReleaseReferenceParticleTextures()
{
    for (ReferenceParticleRun& run : m_referenceParticleRuns)
        run.instance->ReleaseDeviceTextures();
}

void Engine::ReacquireReferenceParticleTextures()
{
    for (ReferenceParticleRun& run : m_referenceParticleRuns)
        run.instance->ReacquireDeviceTextures(*this);
}
