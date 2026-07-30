#include "ParticleSystemInstance.h"
#include "EmitterInstance.h"
#include "SpawnerPath.h"
#include "EmitterDrawOrder.h"
#include <vector>
#include <utility>
#include <unordered_map>
using namespace std;

void ParticleSystemInstance::onParticleSystemChanged(const Engine& engine, int track)
{
    for (auto& emitter : m_emitters)
	{
        emitter->onParticleSystemChanged(engine, track);
	}
}

void ParticleSystemInstance::AppendLiveParticleSamples(
    int instanceIndex, std::vector<LiveParticleSample>& samples) const
{
    for (const auto& emitter : m_emitters)
    {
        LiveParticleSample sample;
        if (!emitter->GetFirstLiveParticleSample(sample)) continue;
        sample.instanceIndex = instanceIndex;
        samples.push_back(sample);
    }
}

void ParticleSystemInstance::ReleaseDeviceTextures()
{
    for (auto& emitter : m_emitters)
	{
        emitter->ReleaseDeviceTextures();
	}
}

void ParticleSystemInstance::ReacquireDeviceTextures(const Engine& engine)
{
    for (auto& emitter : m_emitters)
	{
        emitter->ReacquireDeviceTextures(engine);
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

// Draw the emitters matching `wantHeat` back-to-front per the authored emitter
// TREE, not m_emitters spawn order: a parent draws ON TOP OF its children, and
// siblings/roots order by authored list POSITION (#574, #609). Root-only systems
// are a no-op ordering (each root's key is just its position). The pure pieces —
// post-order draw keys over the authored tree, then a heat-filtered stable sort
// by key — live in EmitterDrawOrder.h (unit-tested, tests/test_emitter_draw_order.cpp);
// this maps the authored tree + each instance to authored POSITIONS, then draws.
//
// Ordering keys off POSITION (a node's slot in getEmitters()) — never
// Emitter::index — is deliberate: index is a mutable mirror the bridge can
// overwrite (BridgeDispatch_Emitters `emit->index = ...`), and a desynced index
// could silently collapse a parent's and child's keys and put the child back on
// top (#609 review). Position is the true authored order and each Emitter* is a
// stable identity, so we map both the parent links and every live instance
// through the same Emitter*->position table.
void ParticleSystemInstance::RenderByRank(IDirect3DDevice9* pDevice, bool wantHeat)
{
    // Emitter* -> authored position, over the SMALL authored set (dozens).
    const std::vector<ParticleSystem::Emitter*>& authored = m_system.getEmitters();
    const size_t n = authored.size();
    std::unordered_map<const ParticleSystem::Emitter*, size_t> position;
    position.reserve(n * 2 + 1);
    for (size_t r = 0; r < n; ++r) position[authored[r]] = r;

    // Parent link per position, then post-order draw keys (child behind parent).
    std::vector<size_t> parentPos(n);
    for (size_t r = 0; r < n; ++r)
    {
        const ParticleSystem::Emitter* p = authored[r]->parent;
        auto it = p ? position.find(p) : position.end();
        parentPos[r] = (it != position.end()) ? it->second : static_cast<size_t>(-1);
    }
    const std::vector<size_t> drawKey = ComputeAuthoredDrawKeys(parentPos);

    std::vector<EmitterInstance*> insts;
    std::vector<std::pair<size_t, bool>> keys;
    insts.reserve(m_emitters.size());
    keys.reserve(m_emitters.size());
    for (auto& emitter : m_emitters)
    {
        // Map the instance to its source emitter's authored position, then its
        // draw key. An instance whose source isn't in the authored list (should
        // never happen — it was spawned from it) falls back to a stable
        // draw-last key so it deterministically draws on top rather than OOB.
        auto it = position.find(emitter->GetSourceEmitter());
        const size_t key = (it != position.end()) ? drawKey[it->second] : n;
        insts.push_back(emitter.get());
        keys.push_back({ key, emitter->IsHeatEmitter() });
    }
    for (size_t idx : ComputeEmitterDrawOrder(keys, wantHeat))
        insts[idx]->Render(pDevice);
}

void ParticleSystemInstance::RenderNormal(IDirect3DDevice9* pDevice)
{
    RenderByRank(pDevice, /*wantHeat=*/false);
}

void ParticleSystemInstance::RenderHeat(IDirect3DDevice9* pDevice)
{
    RenderByRank(pDevice, /*wantHeat=*/true);
}

bool ParticleSystemInstance::HasLiveHeat() const
{
    for (const auto& emitter : m_emitters)
    {
        if (emitter->HasLiveHeat()) return true;
    }
    return false;
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

ParticleSystemInstance::ParticleSystemInstance(
    Engine& engine, const ParticleSystem& system, Object3D* parent,
    std::uint64_t instanceToken)
	: Object3D(parent), m_engine(engine), m_system(system),
      m_instanceToken(instanceToken)
{		
	TimeF now  = GetTimeF();

	// Spawn all root emitters
	const vector<ParticleSystem::Emitter*>& emitters = m_system.getEmitters();
	for (size_t i = 0; i < emitters.size(); i++)
	{
		if (emitters[i]->parent == NULL)
		{
            SpawnEmitter(now, i, this);
            // Record on ATTEMPT, not on success: a spawn refused by the
            // overload guard must stay refused, or a later SyncRootEmitters
            // would make an unrelated property edit silently materialise it.
            m_spawnedRootIds.insert(emitters[i]->stableId);
		}
	}
}

void ParticleSystemInstance::SyncRootEmitters(TimeF currentTime)
{
	// A root EmitterInstance is parented directly to this system instance.
	// Reparenting changes only the authored Emitter::parent, so without this
	// reconciliation the old root-level instance survives alongside the new
	// authored child relationship (an-audit-finding).
	//
	// Collect first because RemoveEmitter mutates m_emitters and unregisters the
	// instance from its source Emitter. Child instances are parented to a
	// particle (or detached after death), so GetParent() == this keeps their
	// dynamic lifetime intact.
	vector<EmitterInstance*> staleRoots;
	for (auto& instance : m_emitters)
	{
		if (instance->GetParent() == this && !instance->IsRoot())
			staleRoots.push_back(instance.get());
	}
	for (EmitterInstance* instance : staleRoots)
		RemoveEmitter(instance);

	const vector<ParticleSystem::Emitter*>& emitters = m_system.getEmitters();
	for (size_t i = 0; i < emitters.size(); i++)
	{
		if (emitters[i] == NULL || emitters[i]->parent != NULL) continue;
		// insert() tells us whether this root is new to the instance in one
		// step — no separate find(). Roots seen at construction, and roots
		// added by an earlier sync, are both already present.
		if (!m_spawnedRootIds.insert(emitters[i]->stableId).second) continue;
		SpawnEmitter(currentTime, i, this);
	}
}

ParticleSystemInstance::~ParticleSystemInstance()
{
}
