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

public:
    const ParticleSystem& GetParticleSystem() { return m_system; }

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