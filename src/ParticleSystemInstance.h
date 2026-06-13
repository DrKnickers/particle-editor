#ifndef PARTICLESYSTEMINSTANCE_H
#define PARTICLESYSTEMINSTANCE_H

#include "Engine.h"
#include <list>
#include <memory>

class ParticleSystemInstance : public Object3D
{
	Engine&				     m_engine;
	const ParticleSystem&    m_system;
	std::list<std::unique_ptr<EmitterInstance>> m_emitters;
    float                    m_zDistance;
    bool                     m_spawnerOwned   = false;

    // Spawner-owned motion + lifetime. Active iff m_spawnerOwned is true
    // AND m_spawnTime >= 0 (set on first Update so we have a real
    // currentTime baseline). On each Update tick, position advances by
    // velocity·dt; once (currentTime - spawnTime) >= maxLifetime,
    // StopSpawning is called (existing particles fade rather than pop).
    float                    m_maxLifetime    = 0.0f;   // 0 = no spawner-imposed cap
    TimeF                    m_spawnTime      = -1.0f;
    TimeF                    m_lastUpdateTime = -1.0f;
    bool                     m_lifetimeExpired = false;

    // Path-shaping (). Captured at the first-Update baseline /
    // SetPathShape; the instance position is then computed analytically
    // from elapsed time τ = currentTime - m_spawnTime:
    //
    //   pos(τ) = m_spawnPos + m_spawnVel·τ + ½·m_accel·τ²
    //            + Aᵢ·( sin(ωτ + φᵢ) − sin(φᵢ) )          // ω = 2π·freq
    //   vel(τ) = m_spawnVel + m_accel·τ + Aᵢ·ω·cos(ωτ + φᵢ)
    //
    // The −sin(φᵢ) term zeroes the squiggle at τ=0 so the instance still
    // emanates from its exact spawn point. vel(τ) is written back to
    // m_velocity each tick so emitted particles inherit the correct
    // instantaneous velocity (EmitterInstance.cpp parentLinkStrength).
    D3DXVECTOR3              m_spawnPos       = D3DXVECTOR3(0, 0, 0);
    D3DXVECTOR3              m_spawnVel       = D3DXVECTOR3(0, 0, 0);
    D3DXVECTOR3              m_accel          = D3DXVECTOR3(0, 0, 0);
    D3DXVECTOR3              m_squiggleAmp    = D3DXVECTOR3(0, 0, 0);
    float                    m_squiggleFreq   = 0.0f;
    D3DXVECTOR3              m_squigglePhase  = D3DXVECTOR3(0, 0, 0);

public:
    const ParticleSystem& GetParticleSystem() { return m_system; }

    // Tag set by the SpawnerDriver after a spawn so the engine can
    // count just spawner-emitted instances toward the 50-instance cap
    // without including Shift+click spawns or other future sources.
    //
    // Also captures the parent-inherited velocity (anchor.velocity +
    // self) into the instance's own m_velocity so it survives the
    // subsequent Detach() — Object3D::Detach captures absolute
    // position but not velocity, since the legacy mouseCursor flow
    // intentionally drops velocity on Shift-release. Spawner-owned
    // instances need to keep moving, so we capture eagerly here.
    void MarkSpawnerOwned()
    {
        m_spawnerOwned = true;
        m_velocity     = GetVelocity();
    }
    bool IsSpawnerOwned() const     { return m_spawnerOwned; }

    // Set by the SpawnerDriver after spawn. 0 means "no cap"; > 0 means
    // call StopSpawning when (currentTime - spawnTime) reaches it.
    void SetMaxLifetime(float seconds) { m_maxLifetime = seconds; }

    // Set by the SpawnerDriver after spawn (). Stamps the
    // path-shaping parameters; the per-instance squiggle phase makes
    // sibling instances in a burst diverge. All-zero ⇒ a straight line.
    void SetPathShape(const D3DXVECTOR3& accel,
                      const D3DXVECTOR3& squiggleAmp,
                      float squiggleFreq,
                      const D3DXVECTOR3& squigglePhase)
    {
        m_accel         = accel;
        m_squiggleAmp   = squiggleAmp;
        m_squiggleFreq  = squiggleFreq;
        m_squigglePhase = squigglePhase;
    }

    void SetPosition(const D3DXVECTOR3& position);

    float GetZDistance() const { return m_zDistance; }

	bool IsDead() const
	{
		return m_emitters.empty();
	}

    int Kill();
    void onParticleSystemChanged(const Engine& engine, int track);
	int  Update(TimeF currentTime);
	void RenderNormal(IDirect3DDevice9* pDevice);
	void RenderHeat(IDirect3DDevice9* pDevice);
	void StopSpawning();
	EmitterInstance* SpawnEmitter(TimeF currentTime, size_t idxEmitter, Object3D* parent);

	// Remove a specific emitter instance, deleting it via the owning unique_ptr.
	// Used by ParticleSystem::Emitter::~Emitter so an Emitter being deleted
	// doesn't leave a dangling pointer in m_emitters (raw `delete` of a
	// pointer owned elsewhere = use-after-free on next render/update).
	void RemoveEmitter(EmitterInstance* instance);

	ParticleSystemInstance(Engine& engine, const ParticleSystem& system, Object3D* parent);
	~ParticleSystemInstance();
};

#endif
