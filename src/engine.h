#ifndef ENGINE_H
#define ENGINE_H

#include "managers.h"
#include "ParticleSystem.h"
#include "utils.h"
#include <memory>

class Object3D
{
    Object3D* m_parent;

protected:
	D3DXVECTOR3 m_position;
    D3DXVECTOR3 m_velocity;

public:
    const Object3D* GetParent() const { return m_parent; }
    Object3D* GetParent() { return m_parent; }

	D3DXVECTOR3 GetPosition() const
	{
        return (m_parent != NULL) ? m_parent->GetPosition() + m_position : m_position;
	}

	D3DXVECTOR3 GetVelocity() const
    {
        return (m_parent != NULL) ? m_parent->GetVelocity() + m_velocity : m_velocity;
    }

	const D3DXVECTOR3& GetRelativeVelocity() const { return m_velocity; }
	const D3DXVECTOR3& GetRelativePosition() const { return m_position; }

    bool Detached() const { return m_parent == NULL; }

    virtual void Detach()
    {
        if (!Detached())
        {
            m_position = GetPosition();
            m_parent   = NULL;
        }
    }

    Object3D(Object3D* parent, const D3DXVECTOR3& position = D3DXVECTOR3(0,0,0))
        : m_parent(parent), m_position(position), m_velocity(0,0,0)
    {
    }
};

typedef float TimeF;
TimeF GetTimeF();

// Preview pause / frame-step controls. See engine.cpp for the clock-
// offset model. State is process-local and never persisted.
void  SetPreviewPaused(bool paused);
bool  IsPreviewPaused();
void  StepPreviewFrames(int frames);  // no-op when not paused

class ParticleSystemInstance;
class EmitterInstance;

class Engine
{
public:
    enum LightType
    {
	    LT_SUN,
	    LT_FILL1,
	    LT_FILL2,
    };

    struct Light
    {
	    D3DXVECTOR4 Diffuse;
	    D3DXVECTOR4 Specular;
	    D3DXVECTOR4 Position;
	    D3DXVECTOR4 Direction;
    };

    static const int NUM_SHADERS = 14;

	// Describes a camera
	struct Camera
	{
		D3DXVECTOR3 Position;
		D3DXVECTOR3 Target;
		D3DXVECTOR3 Up;
	};

	void Update();
	bool Render();

	ParticleSystemInstance* SpawnParticleSystem(const ParticleSystem& system, Object3D* parent);
    
	void DetachParticleSystem(ParticleSystemInstance* instance);
	void KillParticleSystem(ParticleSystemInstance* instance);
	void Clear();
	
	IDirect3DTexture9* GetTexture(const std::string& name) const;

	void OnParticleSystemChanged(int track);

	const D3DXMATRIX& GetProjectionMatrix()   const { return m_projection; }
	const D3DXMATRIX& GetViewMatrix()         const { return m_view; }
	const D3DXMATRIX& GetViewRotationMatrix() const { return m_viewRotation; }
	const D3DXMATRIX& GetBillboardMatrix()    const { return m_billboard; }
	void  GetViewPort(D3DVIEWPORT9* viewport) const;

	const Camera& GetCamera() const;
	void  SetCamera(const Camera& camera);

	bool     GetGround() const		{ return m_showGround; }
	float    GetGroundZ() const		{ return m_groundZ; }
	bool     GetHeatDebug() const   { return m_debugHeat; }
	bool     GetBloom()         const { return m_bloomEnabled;  }
	float    GetBloomStrength() const { return m_bloomStrength; }
	float    GetBloomCutoff()   const { return m_bloomCutoff;   }
	float    GetBloomSize()     const { return m_bloomSize;     }
	// True iff a real `SceneBloom.fx` is loaded and its expected
	// parameter / technique surface was found. False means the
	// shader resolved to the default fallback or the file was
	// missing — UI should disable the bloom controls.
	bool     IsBloomAvailable() const { return m_bloomReady;    }
    COLORREF GetBackground() const  { return m_background; }
	const D3DXVECTOR3& GetGravity() const { return m_gravity; }
	const D3DXVECTOR3& GetWind() const    { return m_wind; }
    Effect* GetShader(int i) const        { return m_pShaders[i]; }

	// Hot-reload all shaders (the 14-element ShaderNames[] array plus the
	// distortion shader). All-or-nothing: if any of the new shaders fails to
	// load, the old set is kept alive and the call returns false.
	bool ReloadShaders();

	// Hot-reload textures by flushing the TextureManager's cache and
	// notifying every active emitter instance to re-fetch.
	void ReloadTextures();

    int GetNumEmitters()  const { return m_numEmitters;  }
    int GetNumParticles() const { return m_numParticles; }
    int GetNumInstances() const { return (int)m_instances.size(); }

    // Count of currently-alive instances that were emitted by the
    // SpawnerDriver (vs. Shift-click spawns or future sources). Used
    // to enforce the spawner's MAX_ACTIVE_INSTANCES cap.
    //
    // Note: spawner-owned instances are NOT killed when the user opens
    // a different .alo. They live until their particles die naturally,
    // and continue to count toward the cap. Same lifetime rules as
    // Shift+click spawns. If a user cranks the rate then loads a
    // different file, expect a brief throttle while the old instances
    // expire.
    int ActiveSpawnerInstanceCount() const;

    void OnEmitterCreated(int numParticles)   { m_numEmitters++; m_numParticles += numParticles; }
    void OnEmitterDestroyed() { m_numEmitters--; }

	void SetBackground(COLORREF color);
	void SetLight(LightType which, const Light& light);
	void SetAmbient(const D3DXVECTOR4& color);
	void SetShadow(const D3DXVECTOR4& color);
	void SetWind(const D3DXVECTOR3& wind);
	void SetGravity(const D3DXVECTOR3& gravity);
	void SetGround(bool enable);
	void SetGroundZ(float z);
	void SetHeatDebug(bool debug);
	void SetBloom(bool enable);
	void SetBloomStrength(float v);
	void SetBloomCutoff(float v);
	void SetBloomSize(float v);

	void				Reset();
	Engine(HWND hFocus, HWND hDevice, ITextureManager& textureManager, IShaderManager& shaderManager);
	~Engine();

private:
	D3DMULTISAMPLE_TYPE GetMultiSampleType(DWORD* MultiSampleQuality, D3DFORMAT DisplayFormat, D3DFORMAT DepthStencilFormat, BOOL Windowed);
	D3DFORMAT           GetDepthStencilFormat(D3DFORMAT AdapterFormat, bool withStencilBuffer);
	void				ResetParameters();

	// Helper used by both the constructor and ReloadShaders(): scans the
	// freshly-loaded shader's parameters for "texture_filename" annotations
	// and binds the named textures.
	void				BindShaderTextures(Effect* shader);

	// Introspects the freshly-loaded SceneBloom effect to (a) verify it
	// isn't the ShaderManager default fallback, (b) cache D3DXHANDLEs
	// for the parameters we drive each frame, and (c) classify each
	// technique by name pattern. Sets m_bloomReady on success.
	void				InitBloomEffect();

	// Releases any half-resolution bloom RTs. Called from Reset() and
	// from ResetParameters() before reallocation.
	void				ReleaseBloomTargets();

	//
	// Data members
	//

	// Particle management
    std::vector<std::unique_ptr<ParticleSystemInstance>> m_instances;
    int m_numParticles;
    int m_numEmitters;

	// Viewing
	Camera		m_eye;
	D3DXMATRIX	m_view;
    D3DXMATRIX	m_viewInverse;
	D3DXMATRIX  m_viewRotation;
	D3DXMATRIX	m_billboard;
	D3DXMATRIX	m_projection;
	D3DXMATRIX	m_viewProjection;

    COLORREF    m_background;
	bool		m_showGround;
	float		m_groundZ;
	bool		m_debugHeat;
	// Bloom post-process state. Shader, RTs, and parameter handles
	// live in the Resources block below. Master enable + three
	// tunables here so they survive shader reload.
	bool		m_bloomEnabled;
	bool		m_bloomReady;       // shader loaded + introspection passed
	float		m_bloomStrength;
	float		m_bloomCutoff;
	float		m_bloomSize;
	D3DXVECTOR3 m_wind;
	D3DXVECTOR3	m_gravity;
    D3DXVECTOR4 m_ambient;
    Light       m_lights[3];
    D3DXMATRIX  m_sphLightFill[3];
    D3DXMATRIX  m_sphLightAll[3];

	// Resources
	IDirect3DTexture9*	m_pGroundTexture;
	IDirect3DTexture9*	m_pSceneTexture;
    IDirect3DSurface9*  m_pDepthStencilSurface;
	IDirect3DTexture9*	m_pDistortTexture;
	Effect*             m_pDistortShader;
    Effect*             m_pShaders[NUM_SHADERS];

	// Bloom resources. m_pBloomEffect is owned (AddRef'd by
	// ShaderManager::getShader and SAFE_RELEASE'd on destroy /
	// reload). The two half-resolution RTs ping-pong during blur.
	Effect*             m_pBloomEffect;
	IDirect3DTexture9*  m_pBloomPing;
	IDirect3DTexture9*  m_pBloomPong;
	// D3DXHANDLEs cached by InitBloomEffect. They reference handles
	// owned by m_pBloomEffect's underlying ID3DXEffect, so they're
	// invalidated whenever the effect is released. The game's
	// SceneBloom.fx exposes a single technique with three passes
	// (bright filter, blur, combine) — we cache one technique
	// handle and step through its passes during Render.
	D3DXHANDLE          m_hBloomStrength;
	D3DXHANDLE          m_hBloomCutoff;
	D3DXHANDLE          m_hBloomSize;
	D3DXHANDLE          m_hBloomIteration;
	D3DXHANDLE          m_hBloomSceneTextureParam;
	// Engine-globals the shader reads via its AlamoEngine.fxh
	// include. m_resolutionConstants packs (1/w, 1/h, 0.5/w, 0.5/h)
	// where w,h is the source RT being sampled. The .zw is read by
	// every VS as the half-pixel offset AND by the blur VS as the
	// per-tap base spacing — if it stays at the default zero, the
	// blur kernel collapses and no blooming happens.
	D3DXHANDLE          m_hBloomResolutionConstants;
	D3DXHANDLE          m_hBloomTechnique;
	UINT                m_bloomPassCount;

	ITextureManager&				m_textureManager;
	IShaderManager&					m_shaderManager;
	IDirect3D9*						m_pDirect3D;
	D3DPRESENT_PARAMETERS			m_presentationParameters;
	IDirect3DDevice9*				m_pDevice;
	IDirect3DVertexDeclaration9*	m_pDeclaration;

	static D3DVERTEXELEMENT9 ParticleElements[];
    
	// Shader
	D3DXHANDLE	 p_worldViewProjection;
};

#endif