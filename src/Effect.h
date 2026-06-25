#ifndef EFFECT_H
#define EFFECT_H

#include "Types.h"
#include <string>
#include <vector>

typedef int Phase;
static const Phase PHASE_TERRAIN_MESH	= 0;
static const Phase PHASE_OPAQUE			= 1;
static const Phase PHASE_SHADOW			= 2;
static const Phase PHASE_TRANSPARENT	= 3;
static const Phase PHASE_OCCLUDED		= 4;
static const Phase PHASE_HEAT			= 5;
static const Phase NUM_PHASES			= 6;

class Effect : public RefCounted
{
public:
	struct Handles
	{
		// Matrices
		D3DXHANDLE hWorld;
		D3DXHANDLE hWorldInverse;
		D3DXHANDLE hWorldView;
		D3DXHANDLE hWorldViewInverse;
		D3DXHANDLE hWorldViewProjection;
		D3DXHANDLE hView;
		D3DXHANDLE hViewInverse;
		D3DXHANDLE hViewProjection;
		D3DXHANDLE hProjection;
		D3DXHANDLE hSkinMatrixArray;   // float4x3[24] bone palette (RSkin shaders, reg c0)
		D3DXHANDLE hEyePosition;
		D3DXHANDLE hEyeObjPosition;
		D3DXHANDLE hResolutionConstants;

		// Lighting
		D3DXHANDLE hGlobalAmbient;
		D3DXHANDLE hDirLightVec0;
		D3DXHANDLE hDirLightObjVec0;
		D3DXHANDLE hDirLightDiffuse;
		D3DXHANDLE hDirLightSpecular;
		D3DXHANDLE hSphLightAll;
		D3DXHANDLE hSphLightFill;
		D3DXHANDLE hLightScale;
		D3DXHANDLE hFogVals;
		D3DXHANDLE hDistanceFadeVals;

		// Time
		D3DXHANDLE hTime;
	};

	struct Parameter
	{	
		enum Type
		{
			PT_INT,
			PT_FLOAT,
			PT_FLOAT3,
			PT_FLOAT4,
			PT_TEXTURE,
		};

		std::string m_name;
		Type		m_type;
		long		m_integer;
		FLOAT       m_float;
		D3DXVECTOR3	m_float3;
		D3DXVECTOR4	m_float4;
		std::string	m_textureName;
	};

private:
	Phase	     m_phase;
	bool         m_shadowVolume;
	ID3DXEffect* m_pD3DEffect;
	Handles      m_handles;

	~Effect();
public:
	Phase              getPhase()       const { return m_phase; }
	bool               isShadowVolume() const { return m_shadowVolume; }
	const Handles&     getHandles()     const { return m_handles; }

    void OnLostDevice()  { m_pD3DEffect->OnLostDevice();  }
    void OnResetDevice() { m_pD3DEffect->OnResetDevice(); }

	ID3DXEffect*       getD3DEffect()   const { m_pD3DEffect->AddRef(); return m_pD3DEffect; }

	Effect(ID3DXEffect* pD3DEffect);
};

#endif