#include "ParticleSystemInstance.h"
#include "EmitterInstance.h"
#include "SpawnerPath.h"
using namespace std;

void ParticleSystemInstance::onParticleSystemChanged(const Engine& engine, int track)
{
    for (auto& emitter : m_emitters)
	{
        emitter->onParticleSystemChanged(engine, track);
	}
}

int ParticleSystemInstance::Update(TimeF currentTime)
{
    // Spawner-owned: drive the shaped path (arc + squiggle) and enforce
    // the optional max-lifetime cap. Non-spawner-owned instances skip
    // this and behave as before (parent-tracked or static-after-detach).
    if (m_spawnerOwned)
    {
        if (m_spawnTime < 0.0f)
        {
            // First Update after spawn — establish the baseline. Capture
            // the launch position + velocity (post-Detach absolutes) so
            // the path is computed analytically from them, not Euler-
            // integrated (exact arc, frame-rate independent).
            m_spawnTime      = currentTime;
            m_lastUpdateTime = currentTime;
            m_spawnPos       = m_position;
            m_spawnVel       = m_velocity;
        }
        else
        {
            // Analytic position + instantaneous velocity at τ via the
            // shared closed form (one source of truth, unit-tested in
            // tests/test_spawner_path.cpp). Writing the live velocity back
            // each tick keeps emitted-particle inheritance correct
            // (EmitterInstance parentLinkStrength).
            float tau = (float)(currentTime - m_spawnTime);
            SpawnerPathState st = { m_spawnPos, m_spawnVel, m_accel,
                                    m_squiggleAmp, m_squiggleFreq, m_squigglePhase };
            EvalSpawnerPath(st, tau, m_position, m_velocity);
            m_lastUpdateTime = currentTime;
        }

        if (!m_lifetimeExpired
            && m_maxLifetime > 0.0f
            && (currentTime - m_spawnTime) >= m_maxLifetime)
        {
            // Stop new particles from being emitted; existing particles
            // fade out naturally over their own track lifetimes. Soft
            // cap rather than a hard Kill — looks better for typical
            // testing (tail puffs / smoke decay rather than pop-out).
            StopSpawning();
            m_lifetimeExpired = true;
        }
    }

    // Calculate Z-Distance
    const D3DXMATRIX& view = m_engine.GetViewMatrix();
    D3DXVECTOR3 pos = GetPosition();
    m_zDistance = (pos.x * view._13 + pos.y * view._23 + pos.z * view._33 + view._43) /     // Z
                  (pos.x * view._14 + pos.y * view._24 + pos.z * view._34 + view._44);      // W

    // Update emitters
    int nParticles = 0;
    for (auto it = m_emitters.begin(); it != m_emitters.end();)
	{
		nParticles += (*it)->Update(currentTime);

		// If it's dead and no longer needed (either detached, or we're its parent), then remove it
		if ((*it)->IsDead() && ((*it)->Detached() || (*it)->GetParent() == this))
		{
			it = m_emitters.erase(it);
			m_engine.OnEmitterDestroyed();
		}
		else
		{
			++it;
		}
	}
    return nParticles;
}

void ParticleSystemInstance::RenderNormal(IDirect3DDevice9* pDevice)
{
    for (auto& emitter : m_emitters)
	{
        if (!emitter->IsHeatEmitter())
        {
            emitter->Render(pDevice);
        }
	}
}

void ParticleSystemInstance::RenderHeat(IDirect3DDevice9* pDevice)
{
    for (auto& emitter : m_emitters)
	{
        if (emitter->IsHeatEmitter())
        {
            emitter->Render(pDevice);
        }
	}
}

void ParticleSystemInstance::SetPosition(const D3DXVECTOR3& position)
{
    m_position = position;
}

void ParticleSystemInstance::StopSpawning()
{
	for (auto& emitter : m_emitters)
	{
		if (emitter->IsRoot())
		{
			emitter->StopSpawning();
		}
	}
}

int ParticleSystemInstance::Kill()
{
	int numParticles = 0;
	for (auto& emitter : m_emitters)
	{
		numParticles += emitter->Kill();
	}
	return numParticles;
}

EmitterInstance* ParticleSystemInstance::SpawnEmitter(TimeF currentTime, size_t idxEmitter, Object3D* parent)
{
    // Overload guard: refuse new instances past the engine-wide cap
    // (see engine.h kDefaultMaxPreviewParticles) — chain multiplication
    // allocates a whole EmitterInstance per spawned particle, so this
    // is the second OOM choke point besides the particle budget. Every
    // caller tolerates nullptr (child links are null-checked; the ctor
    // ignores the return).
    if (!m_engine.TryConsumeInstanceBudget())
        return nullptr;

    int numParticles;
	ParticleSystem::Emitter* emitter = m_system.getEmitters()[idxEmitter];
    auto instance = std::make_unique<EmitterInstance>(currentTime, *this, m_engine, *emitter, parent, &numParticles);
	m_emitters.push_back(std::move(instance));
    m_engine.OnEmitterCreated(numParticles);
#ifndef NDEBUG
    fprintf(stdout,
            "[Spawn] idx=%zu name='%s' parent=%p useBursts=%d nBursts=%lu pps=%lu particlesPerBurst=%lu lifetime=%.2f initialDelay=%.2f emitFromMesh=%d linkToSystem=%d -> numParticles=%d\n",
            idxEmitter, emitter->name.c_str(), (void*)emitter->parent,
            (int)emitter->useBursts, (unsigned long)emitter->nBursts,
            (unsigned long)emitter->nParticlesPerSecond,
            (unsigned long)emitter->nParticlesPerBurst,
            emitter->lifetime, emitter->initialDelay,
            (int)emitter->emitFromMesh, (int)emitter->linkToSystem,
            numParticles);
    fflush(stdout);
#endif
	return m_emitters.back().get();
}

void ParticleSystemInstance::RemoveEmitter(EmitterInstance* instance)
{
	for (auto it = m_emitters.begin(); it != m_emitters.end(); ++it)
	{
		if (it->get() == instance)
		{
			// Budget-leak fix: this path can destroy an instance that
			// still holds live particles (e.g. deleting an emitter while
			// it previews). Without subtracting them the engine's
			// m_numParticles stays inflated until Clear(), permanently
			// shrinking the overload guard's spawn budget. Kill() returns
			// the negative live-particle delta — mirror
			// Engine::KillParticleSystem's accounting.
			const int particleDelta = instance->Kill();
			// erase() destroys the unique_ptr, which calls ~EmitterInstance.
			// That dtor unregisters from its Emitter::m_instances, so the
			// caller's iteration over m_instances stays consistent.
			m_emitters.erase(it);
			m_engine.OnEmitterDestroyed(particleDelta);
			return;
		}
	}
}

ParticleSystemInstance::ParticleSystemInstance(Engine& engine, const ParticleSystem& system, Object3D* parent)
	: Object3D(parent), m_engine(engine), m_system(system)
{		
	TimeF now  = GetTimeF();

	// Spawn all root emitters
	const vector<ParticleSystem::Emitter*>& emitters = m_system.getEmitters();
	for (size_t i = 0; i < emitters.size(); i++)
	{
		if (emitters[i]->parent == NULL)
		{
            SpawnEmitter(now, i, this);
		}
	}
}

ParticleSystemInstance::~ParticleSystemInstance()
{
}