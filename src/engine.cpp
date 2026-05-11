#include <algorithm>
#include <assert.h>
#include "engine.h"
#include "exceptions.h"
#include "resource.h"
#include "ParticleSystemInstance.h"
#include "EmitterInstance.h"
#include "SphericalHarmonics.h"
using namespace std;

static const char* ShaderNames[Engine::NUM_SHADERS] = {
    "Engine\\PrimOpaque.fx",
    "Engine\\PrimAdditive.fx",
    "Engine\\PrimAlpha.fx",
    "Engine\\PrimModulate.fx",
    "Engine\\PrimDepthSpriteAdditive.fx",
    "Engine\\PrimDepthSpriteAlpha.fx",
    "Engine\\PrimDepthSpriteModulate.fx",
    "Engine\\PrimDiffuseAlpha.fx",
    "Engine\\StencilDarken.fx",
    "Engine\\StencilDarkenFinalBlur.fx",
    "Engine\\PrimHeat.fx",
    "Engine\\PrimParticleBumpAlpha.fx",
    "Engine\\PrimDecalBumpAlpha.fx",
    "Engine\\PrimAlphaScanlines.fx",
};

D3DVERTEXELEMENT9 Engine::ParticleElements[] = {
	{0, offsetof(EmitterInstance::Vertex, Position),  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, 
	{0, offsetof(EmitterInstance::Vertex, Normal),    D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0}, 
	{0, offsetof(EmitterInstance::Vertex, TexCoord0), D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, 
	{0, offsetof(EmitterInstance::Vertex, TexCoord1), D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1}, 
	{0, offsetof(EmitterInstance::Vertex, Color),     D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0}, 
	D3DDECL_END()
};

TimeF GetTimeF()
{
    static auto start = GetTickCount();
    return (GetTickCount() - start) / 1000.0f;
}

ParticleSystemInstance* Engine::SpawnParticleSystem(const ParticleSystem& system, Object3D* parent)
{
	auto instance = std::make_unique<ParticleSystemInstance>(*this, system, parent);
    m_instances.push_back(std::move(instance));
	return m_instances.back().get();
}

void Engine::DetachParticleSystem(ParticleSystemInstance* instance)
{
    instance->Detach();
}

void Engine::KillParticleSystem(ParticleSystemInstance* instance)
{
	if (instance->GetParticleSystem().getLeaveParticles())
	{
		// Leave particles to finish; just disable it
		instance->StopSpawning();
	}
	else
	{
    	// Don't leave particles, kill the thing now
		m_numParticles += instance->Kill();
	}

	instance->Detach();
}

void Engine::Clear()
{
	m_instances.clear();
    m_numParticles = 0;
    m_numEmitters  = 0;
}

int Engine::ActiveSpawnerInstanceCount() const
{
    int n = 0;
    for (const auto& inst : m_instances)
    {
        if (inst && inst->IsSpawnerOwned()) ++n;
    }
    return n;
}

// Helper: scan a freshly-loaded effect for parameters annotated with
// "texture_filename" and bind the named textures from the texture manager.
// Same logic that used to live inline in the constructor's load loop.
void Engine::BindShaderTextures(Effect* shader)
{
	if (shader == NULL) return;
	ID3DXEffect* pEffect = shader->getD3DEffect();
	if (pEffect == NULL) return;

	D3DXEFFECT_DESC effectDesc;
	pEffect->GetDesc(&effectDesc);
	for (UINT i = 0; i < effectDesc.Parameters; i++)
	{
		D3DXHANDLE hParam = pEffect->GetParameter(NULL, i);
		D3DXPARAMETER_DESC paramDesc;
		pEffect->GetParameterDesc(hParam, &paramDesc);
		if (paramDesc.Type == D3DXPT_TEXTURE)
		{
			D3DXHANDLE hAnnon = pEffect->GetAnnotationByName(hParam, "texture_filename");
			D3DXPARAMETER_DESC annonDesc;
			pEffect->GetParameterDesc(hAnnon, &annonDesc);
			LPCSTR value = NULL;
			if (SUCCEEDED(pEffect->GetString(hAnnon, &value)) && value != NULL)
			{
				IDirect3DTexture9* pTexture = m_textureManager.getTexture(m_pDevice, value);
				pEffect->SetTexture(hParam, pTexture);
				SAFE_RELEASE(pTexture);
			}
		}
	}
	SAFE_RELEASE(pEffect);
}

// Helper: appends a line to both the diagnostic file and the debug
// output stream. Always-on (no NDEBUG gate) so a user reporting
// "bloom is greyed out" has a paper trail.
static void BloomLog(FILE* f, const char* line)
{
	if (f != NULL) { fputs(line, f); }
	OutputDebugStringA(line);
	printf("%s", line);
}

// Returns the .exe's directory with trailing backslash, e.g.
// "<path> Editor\". Used to place the bloom diagnostic
// file next to the executable where the user is most likely to look.
static std::wstring ExeDirectory()
{
	wchar_t path[MAX_PATH] = {0};
	GetModuleFileNameW(NULL, path, MAX_PATH);
	std::wstring s(path);
	size_t pos = s.find_last_of(L"\\/");
	if (pos != std::wstring::npos) s.resize(pos + 1);
	return s;
}

// Introspect the freshly-loaded SceneBloom effect. Confirms the shader
// isn't the ShaderManager default fallback (we'd render garbage through
// it), caches the parameter / technique handles we drive each frame,
// and flips m_bloomReady to true on success.
//
// Writes a diagnostic file `bloom-diagnostic.log` next to the editor
// .exe on every run, dumping every parameter and technique name the
// loaded effect exposes. If bloom comes up greyed for a user, they
// can read that file to see what's actually in their game's shader —
// and the matcher strings below can be updated to whatever the game
// uses without guessing.
void Engine::InitBloomEffect()
{
	m_bloomReady                = false;
	m_hBloomStrength            = NULL;
	m_hBloomCutoff              = NULL;
	m_hBloomSize                = NULL;
	m_hBloomIteration           = NULL;
	m_hBloomSceneTextureParam   = NULL;
	m_hBloomResolutionConstants = NULL;
	m_hBloomTechnique           = NULL;
	m_bloomPassCount            = 0;

	// Open the diagnostic file. Failure here is non-fatal; we'll
	// still try to introspect and just skip the file output.
	std::wstring logPath = ExeDirectory() + L"bloom-diagnostic.log";
	FILE* f = NULL;
	_wfopen_s(&f, logPath.c_str(), L"w");

	auto logf = [&](const char* fmt, ...)
	{
		char buf[1024];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		BloomLog(f, buf);
	};

	logf("[bloom] InitBloomEffect — Engine\\SceneBloom.fx via ShaderManager\n");

	if (m_pBloomEffect == NULL)
	{
		logf("[bloom]   getShader returned NULL — no shader was loaded.\n");
		logf("[bloom]   Verify game install path is configured (Mods menu) and that\n");
		logf("[bloom]   Data\\Art\\Shaders\\Engine\\SceneBloom.fx (.fxo) exists either\n");
		logf("[bloom]   loose on disk or in a Shaders MEG archive.\n");
		if (f) fclose(f);
		return;
	}

	ID3DXEffect* pFx = m_pBloomEffect->getD3DEffect();
	if (pFx == NULL)
	{
		logf("[bloom]   ID3DXEffect pointer is NULL inside the Effect wrapper.\n");
		if (f) fclose(f);
		return;
	}

	D3DXEFFECT_DESC desc;
	if (FAILED(pFx->GetDesc(&desc)))
	{
		logf("[bloom]   GetDesc failed — the effect is in a bad state.\n");
		SAFE_RELEASE(pFx);
		if (f) fclose(f);
		return;
	}

	logf("[bloom]   Effect loaded: %u parameters, %u techniques, %u functions\n",
	     desc.Parameters, desc.Techniques, desc.Functions);

	// Enumerate every parameter — names + types, so a future bloom-
	// matcher tweak knows exactly what the shader actually exposes.
	logf("[bloom]   Parameters:\n");
	for (UINT i = 0; i < desc.Parameters; ++i)
	{
		D3DXHANDLE hParam = pFx->GetParameter(NULL, i);
		D3DXPARAMETER_DESC pd;
		if (FAILED(pFx->GetParameterDesc(hParam, &pd)) || pd.Name == NULL) continue;

		const char* className = "?";
		switch (pd.Class)
		{
			case D3DXPC_SCALAR:        className = "scalar";        break;
			case D3DXPC_VECTOR:        className = "vector";        break;
			case D3DXPC_MATRIX_ROWS:   className = "matrix_rows";   break;
			case D3DXPC_MATRIX_COLUMNS:className = "matrix_cols";   break;
			case D3DXPC_OBJECT:        className = "object";        break;
			case D3DXPC_STRUCT:        className = "struct";        break;
			default:                   className = "?";             break;
		}
		const char* typeName = "?";
		switch (pd.Type)
		{
			case D3DXPT_BOOL:    typeName = "bool";    break;
			case D3DXPT_INT:     typeName = "int";     break;
			case D3DXPT_FLOAT:   typeName = "float";   break;
			case D3DXPT_STRING:  typeName = "string";  break;
			case D3DXPT_TEXTURE: typeName = "texture"; break;
			case D3DXPT_TEXTURE1D:
			case D3DXPT_TEXTURE2D:
			case D3DXPT_TEXTURE3D:
			case D3DXPT_TEXTURECUBE: typeName = "tex_nD"; break;
			case D3DXPT_SAMPLER:
			case D3DXPT_SAMPLER1D:
			case D3DXPT_SAMPLER2D:
			case D3DXPT_SAMPLER3D:
			case D3DXPT_SAMPLERCUBE: typeName = "sampler"; break;
			default: typeName = "?"; break;
		}
		logf("[bloom]     [%u] %s %s %s\n", i, className, typeName, pd.Name);
	}

	// Enumerate every technique by name.
	logf("[bloom]   Techniques:\n");
	for (UINT i = 0; i < desc.Techniques; ++i)
	{
		D3DXHANDLE hTech = pFx->GetTechnique(i);
		D3DXTECHNIQUE_DESC td;
		if (FAILED(pFx->GetTechniqueDesc(hTech, &td)) || td.Name == NULL) continue;
		// ValidateTechnique returns S_OK only if the technique works
		// on the current hardware. A "valid name, invalid for this
		// device" technique tells us the shader compiles but the
		// hardware can't run the bloom pass.
		BOOL valid = SUCCEEDED(pFx->ValidateTechnique(hTech)) ? TRUE : FALSE;
		logf("[bloom]     [%u] %s (%u passes, %s)\n",
		     i, td.Name, td.Passes, valid ? "valid on this device" : "NOT valid on this device");
	}

	// Now run the actual matching to bind handles. The game's
	// SceneBloom.fx exposes BloomStrength / BloomCutoff / BloomSize
	// as float scalars, BloomIteration as a per-pass control float,
	// and SceneTexture as the single input that's rebound between
	// passes (bright filter reads scene, blur reads bright-pass
	// output, combine reads blurred output and additively writes
	// onto the existing scene RT).
	m_hBloomStrength            = pFx->GetParameterByName(NULL, "BloomStrength");
	m_hBloomCutoff              = pFx->GetParameterByName(NULL, "BloomCutoff");
	m_hBloomSize                = pFx->GetParameterByName(NULL, "BloomSize");
	m_hBloomIteration           = pFx->GetParameterByName(NULL, "BloomIteration");
	m_hBloomSceneTextureParam   = pFx->GetParameterByName(NULL, "SceneTexture");
	m_hBloomResolutionConstants = pFx->GetParameterByName(NULL, "m_resolutionConstants");

	// Pick the first technique that validates on this device. The
	// game's shader has a single technique with three passes —
	// bright filter (0), blur (1), combine (2) — driven by the
	// vs_/ps_bright_filter_bin / vs_/ps_bloom_bin /
	// vs_/ps_combine_bin precompiled blobs the .fx ships with.
	for (UINT i = 0; i < desc.Techniques; ++i)
	{
		D3DXHANDLE hTech = pFx->GetTechnique(i);
		D3DXTECHNIQUE_DESC td;
		if (FAILED(pFx->GetTechniqueDesc(hTech, &td))) continue;

		if (SUCCEEDED(pFx->ValidateTechnique(hTech)))
		{
			m_hBloomTechnique = hTech;
			m_bloomPassCount  = td.Passes;
			break;
		}
	}

	SAFE_RELEASE(pFx);

	logf("[bloom]   Matcher results:\n");
	logf("[bloom]     BloomStrength          -> %s\n", m_hBloomStrength            ? "found" : "MISSING");
	logf("[bloom]     BloomCutoff            -> %s\n", m_hBloomCutoff              ? "found" : "MISSING");
	logf("[bloom]     BloomSize              -> %s\n", m_hBloomSize                ? "found" : "MISSING");
	logf("[bloom]     BloomIteration         -> %s\n", m_hBloomIteration           ? "found" : "missing (optional)");
	logf("[bloom]     SceneTexture           -> %s\n", m_hBloomSceneTextureParam   ? "found" : "MISSING");
	logf("[bloom]     m_resolutionConstants  -> %s\n", m_hBloomResolutionConstants ? "found" : "MISSING");
	logf("[bloom]     Active technique       -> %s (%u passes)\n",
	     m_hBloomTechnique ? "found" : "MISSING", m_bloomPassCount);

	// We need the technique, the scene-texture param, the three
	// tunable scalars, and at least 3 passes (bright + blur +
	// combine). BloomIteration is optional — set when present.
	m_bloomReady = (m_hBloomStrength            != NULL)
	            && (m_hBloomCutoff              != NULL)
	            && (m_hBloomSize                != NULL)
	            && (m_hBloomSceneTextureParam   != NULL)
	            && (m_hBloomResolutionConstants != NULL)
	            && (m_hBloomTechnique           != NULL)
	            && (m_bloomPassCount            >= 3);

	logf("[bloom]   Verdict: bloom is %s.\n",
	     m_bloomReady ? "READY — UI enabled" : "UNAVAILABLE — UI greyed");

	if (f) fclose(f);
}

void Engine::ReleaseBloomTargets()
{
	SAFE_RELEASE(m_pBloomPing);
	SAFE_RELEASE(m_pBloomPong);
}

// Hot-reload all 14 entries from ShaderNames[]. All-or-nothing: load every
// new shader into a temporary array first, only swap into m_pShaders[] once
// every one succeeds. On failure the previous set stays alive untouched, so
// a busted mod shader can't brick a running session.
bool Engine::ReloadShaders()
{
	printf("[Shaders] Reload begin\n"); fflush(stdout);

	// Flush the shader manager's cache so getShader() re-resolves from disk
	// (otherwise it just hands back the same Effect* we already have).
	m_shaderManager.Clear();

	Effect* tmp[NUM_SHADERS] = { NULL };

	for (int i = 0; i < NUM_SHADERS; i++)
	{
		tmp[i] = m_shaderManager.getShader(m_pDevice, ShaderNames[i]);
		if (tmp[i] == NULL)
		{
			printf("[Shaders] FAILED at %s — keeping previous shader set\n",
			       ShaderNames[i]); fflush(stdout);
			for (int j = 0; j < i; j++) SAFE_RELEASE(tmp[j]);
			return false;
		}
	}

	// Commit: release old, install new, re-bind annotated textures.
	for (int i = 0; i < NUM_SHADERS; i++)
	{
		SAFE_RELEASE(m_pShaders[i]);
		m_pShaders[i] = tmp[i];
		BindShaderTextures(m_pShaders[i]);
	}

	// Bloom shader is optional — loaded separately so a missing
	// SceneBloom.fx never blocks particle rendering. ShaderManager
	// resolution chain (mod path → game roots → MEG archives) does
	// the work; we just call getShader. On failure or fallback-to-
	// default, InitBloomEffect detects it and disables bloom.
	SAFE_RELEASE(m_pBloomEffect);
	m_pBloomEffect = m_shaderManager.getShader(m_pDevice, "Engine\\SceneBloom.fx");
	InitBloomEffect();

	printf("[Shaders] Reload complete: %d ok\n", NUM_SHADERS); fflush(stdout);
	return true;
}

// Hot-reload textures: flush the texture manager's cache so the next lookup
// re-resolves from disk, then notify every active emitter instance to drop
// its current texture handles and re-fetch. Cheap & safe — texture loads
// can't really fail (missing files fall through to the placeholder).
void Engine::ReloadTextures()
{
	m_textureManager.Clear();
	int n = (int)m_instances.size();
	OnParticleSystemChanged(-1);
	printf("[Textures] Reload: cache cleared, %d instance(s) notified\n", n); fflush(stdout);
}

void Engine::Update()
{
	TimeF currentTime = GetTimeF();

    // Update existing instances
    for (auto it = m_instances.begin(); it != m_instances.end();)
    {
        m_numParticles += (*it)->Update(currentTime);

		// Check if the instance is dead and nobody's referring to it anymore
		if ((*it)->IsDead() && (*it)->Detached())
		{
			it = m_instances.erase(it);
		}
		else
		{
			++it;
		}
    }
}

bool Engine::Render()
{
	static const D3DXMATRIX Identity(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);

	// See if we can render
	switch (m_pDevice->TestCooperativeLevel())
	{
		case D3DERR_DEVICELOST:
			return false;

		case D3DERR_DEVICENOTRESET:
			Reset();
			break;
	}

    // Set all effect parameters
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        static D3DXMATRIX Identity(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);

        const Effect::Handles& handles = m_pShaders[i]->getHandles();
        ID3DXEffect* pEffect = m_pShaders[i]->getD3DEffect();

        // World, View, Projection Transforms
        pEffect->SetMatrix(handles.hWorld,               &Identity);
        pEffect->SetMatrix(handles.hWorldInverse,        &Identity);
        pEffect->SetMatrix(handles.hProjection,          &m_projection);
        pEffect->SetMatrix(handles.hViewProjection,      &m_viewProjection);
        pEffect->SetMatrix(handles.hViewInverse,         &m_viewInverse);
        pEffect->SetMatrix(handles.hView,                &m_view);
        pEffect->SetMatrix(handles.hWorldViewProjection, &m_viewProjection);
        pEffect->SetMatrix(handles.hWorldViewInverse,    &m_viewInverse);
        pEffect->SetMatrix(handles.hWorldView,           &m_view);
        pEffect->SetVector(handles.hEyePosition,         &D3DXVECTOR4(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z, 1));

        // Lighting
        pEffect->SetVector(handles.hGlobalAmbient,    &m_ambient);
        pEffect->SetVector(handles.hDirLightVec0,     &m_lights[0].Position);
        pEffect->SetVector(handles.hDirLightObjVec0,  &m_lights[0].Position);
        pEffect->SetVector(handles.hDirLightDiffuse,  &m_lights[0].Diffuse);
        pEffect->SetVector(handles.hDirLightSpecular, &m_lights[0].Specular);
        pEffect->SetMatrixArray(handles.hSphLightAll,  m_sphLightAll,  3);
        pEffect->SetMatrixArray(handles.hSphLightFill, m_sphLightFill, 3);

        // Time
        pEffect->SetFloat(handles.hTime, GetTimeF());
        SAFE_RELEASE(pEffect);
    }

    // Sort the particle systems on distance from camera
    // Negative Z is further away, thus drawn first.
    // Therefore we need a normal ascending sort.
	sort(m_instances.begin(), m_instances.end(), [](const auto& p1, const auto& p2) {
		return p1->GetZDistance() < p2->GetZDistance();
	});
	
	m_pDevice->BeginScene();

	IDirect3DSurface9* pScreenSurface;
	IDirect3DSurface9* pDepthSurface;
	m_pDevice->GetRenderTarget(0, &pScreenSurface);
    m_pDevice->GetDepthStencilSurface(&pDepthSurface);

    // Set the new depth buffer
    m_pDevice->SetDepthStencilSurface(m_pDepthStencilSurface);

	// Render to the scene texture
	IDirect3DSurface9* pSceneSurface;
	m_pSceneTexture->GetSurfaceLevel(0, &pSceneSurface);
	m_pDevice->SetRenderTarget(0, pSceneSurface);
	SAFE_RELEASE(pSceneSurface);

    D3DCOLOR clearColor = D3DCOLOR_XRGB(GetRValue(m_background), GetGValue(m_background), GetBValue(m_background));
	m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
	if (m_showGround)
	{
		static const float TEXTURE_SCALE  = 256;
		static const float MAP_SIZE       = 80;
		static const float UNITS_PER_CELL = 20;
		// Per-frame init so m_groundZ is picked up live; cost is 4
		// vertices × ~80 bytes, negligible against the surrounding draw.
		const float z = m_groundZ;
		const EmitterInstance::Vertex ground[4] = {
			{D3DXVECTOR3(-UNITS_PER_CELL*MAP_SIZE/2,-UNITS_PER_CELL*MAP_SIZE/2,z), D3DXVECTOR3(0,0,1), D3DXVECTOR2(                                    0,                                     0), D3DXVECTOR2(0,0), D3DCOLOR_RGBA(255,255,255,255)},
			{D3DXVECTOR3( UNITS_PER_CELL*MAP_SIZE/2,-UNITS_PER_CELL*MAP_SIZE/2,z), D3DXVECTOR3(0,0,1), D3DXVECTOR2(MAP_SIZE*UNITS_PER_CELL/TEXTURE_SCALE,                                     0), D3DXVECTOR2(0,0), D3DCOLOR_RGBA(255,255,255,255)},
			{D3DXVECTOR3(-UNITS_PER_CELL*MAP_SIZE/2, UNITS_PER_CELL*MAP_SIZE/2,z), D3DXVECTOR3(0,0,1), D3DXVECTOR2(                                    0, MAP_SIZE*UNITS_PER_CELL/TEXTURE_SCALE), D3DXVECTOR2(0,0), D3DCOLOR_RGBA(255,255,255,255)},
			{D3DXVECTOR3( UNITS_PER_CELL*MAP_SIZE/2, UNITS_PER_CELL*MAP_SIZE/2,z), D3DXVECTOR3(0,0,1), D3DXVECTOR2(MAP_SIZE*UNITS_PER_CELL/TEXTURE_SCALE, MAP_SIZE*UNITS_PER_CELL/TEXTURE_SCALE), D3DXVECTOR2(0,0), D3DCOLOR_RGBA(255,255,255,255)}
		};

		m_pDevice->SetTexture(0, m_pGroundTexture);
		m_pDevice->SetTransform(D3DTS_TEXTURE0, &Identity);
		m_pDevice->SetTexture(1, NULL);
		m_pDevice->SetRenderState(D3DRS_ZENABLE,          TRUE);
		m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
		m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, ground, sizeof(EmitterInstance::Vertex));
	}

    // Particles never write to the depth buffer — let painter's-order
    // (the order each emitter is drawn) decide stacking when emitters
    // overlap, matching the in-game behaviour.
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    for (auto& instance : m_instances)
    {
        instance->RenderNormal(m_pDevice);
	}
    m_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);

	// Bloom post-process. Runs after the scene is drawn but before
	// the heat/distortion pass, so distortion smears the bloomed
	// image (matches in-game order). The game's SceneBloom.fx
	// exposes one technique with three passes:
	//
	//   pass 0  bright filter   scene  -> ping
	//   pass 1  4-tap blur      src    -> dst    (ping-pong, N iters,
	//                                             BloomIteration grows
	//                                             the kernel each time)
	//   pass 2  combine         final  -> scene  (AddSmooth blend, no
	//                                             clear — shader pass
	//                                             state handles it)
	//
	// Skipped entirely when bloom is off, unavailable, or RTs
	// failed to alloc — no perf cost in those cases.
	//
	// BLOOM_BLUR_ITERATIONS = 4 -- canonical engine value, proven by
	// static RE of the Petroglyph 2025 64-bit patch:
	//   EAW Terrain Editor.exe: bound at .data:0x140f09244 = 4, 0 writers
	//   StarWarsG.exe:          bound at .data:0x140a129f4 = 4, 0 writers
	// Both binaries store the loop bound as a `.data`-baked int32 with
	// no runtime write site anywhere in the program -- equivalent to a
	// hardcoded constant. See tasks/find_bloom_iterations.md.
	static const UINT BLOOM_BLUR_ITERATIONS = 4;
	if (m_bloomEnabled && m_bloomReady && m_pBloomEffect != NULL
	    && m_pBloomPing != NULL && m_pBloomPong != NULL)
	{
		ID3DXEffect* pBloom = m_pBloomEffect->getD3DEffect();
		if (pBloom != NULL)
		{
			pBloom->SetFloat(m_hBloomStrength, m_bloomStrength);
			pBloom->SetFloat(m_hBloomCutoff,   m_bloomCutoff);
			pBloom->SetFloat(m_hBloomSize,     m_bloomSize);

			// m_resolutionConstants = (1/w, 1/h, 0.5/w, 0.5/h).
			// Used by every VS for half-pixel UV correction; the
			// .zw is also the blur's per-tap base spacing, so a
			// missing or zero value collapses the blur kernel.
			const float bloomW = (float)m_presentationParameters.BackBufferWidth;
			const float bloomH = (float)m_presentationParameters.BackBufferHeight;
			const D3DXVECTOR4 resCon(1.0f / bloomW, 1.0f / bloomH,
			                         0.5f / bloomW, 0.5f / bloomH);
			pBloom->SetVector(m_hBloomResolutionConstants, &resCon);

			// Fullscreen quad in clip space, same vertex layout as the
			// existing distortion compose quad below.
			static const EmitterInstance::Vertex bloomQuad[4] = {
				{D3DXVECTOR3(-1,-1,0), D3DXVECTOR2(0, 1), D3DXVECTOR4(1,1,1,1)},
				{D3DXVECTOR3( 1,-1,0), D3DXVECTOR2(1, 1), D3DXVECTOR4(1,1,1,1)},
				{D3DXVECTOR3(-1, 1,0), D3DXVECTOR2(0, 0), D3DXVECTOR4(1,1,1,1)},
				{D3DXVECTOR3( 1, 1,0), D3DXVECTOR2(1, 0), D3DXVECTOR4(1,1,1,1)}
			};

			IDirect3DSurface9* pPingSurface = NULL;
			IDirect3DSurface9* pPongSurface = NULL;
			IDirect3DSurface9* pSceneRT     = NULL;
			m_pBloomPing->GetSurfaceLevel(0, &pPingSurface);
			m_pBloomPong->GetSurfaceLevel(0, &pPongSurface);
			m_pSceneTexture->GetSurfaceLevel(0, &pSceneRT);

			pBloom->SetTechnique(m_hBloomTechnique);
			UINT nPasses = 0;
			if (SUCCEEDED(pBloom->Begin(&nPasses, 0)) && nPasses >= 3)
			{
				// ---------- Pass 0: bright filter (scene -> ping) ----------
				m_pDevice->SetRenderTarget(0, pPingSurface);
				m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0,0,0,0), 1.0f, 0);
				pBloom->SetTexture(m_hBloomSceneTextureParam, m_pSceneTexture);
				pBloom->BeginPass(0);
				pBloom->CommitChanges();
				m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bloomQuad, sizeof(EmitterInstance::Vertex));
				pBloom->EndPass();

				// ---------- Pass 1: blur loop, ping/pong N iterations ----------
				// After bright filter the result lives in PING. Each
				// iteration alternates the source and destination and
				// bumps BloomIteration; the shader uses it to widen the
				// 4-tap diagonal kernel:
				//
				//   delta = BloomSize * half_pixel * (1 + 2 * BloomIteration)
				//
				// so iteration 0 has the smallest kernel and each
				// subsequent iteration spreads the highlights wider.
				IDirect3DTexture9* srcTex = m_pBloomPing;
				IDirect3DSurface9* dstSurf = pPongSurface;
				IDirect3DTexture9* dstTex = m_pBloomPong;
				for (UINT it = 0; it < BLOOM_BLUR_ITERATIONS; ++it)
				{
					m_pDevice->SetRenderTarget(0, dstSurf);
					m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0,0,0,0), 1.0f, 0);
					pBloom->SetTexture(m_hBloomSceneTextureParam, srcTex);
					if (m_hBloomIteration != NULL)
					{
						pBloom->SetFloat(m_hBloomIteration, (float)it);
					}
					pBloom->BeginPass(1);
					pBloom->CommitChanges();
					m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bloomQuad, sizeof(EmitterInstance::Vertex));
					pBloom->EndPass();

					// Swap for the next iteration. After the loop ends,
					// `srcTex` points at the texture holding the final
					// blurred result.
					if (it + 1 < BLOOM_BLUR_ITERATIONS)
					{
						IDirect3DTexture9* tmpTex = srcTex;
						IDirect3DSurface9* tmpSurf = (dstSurf == pPongSurface) ? pPingSurface : pPongSurface;
						srcTex = dstTex;
						dstTex = tmpTex;
						dstSurf = tmpSurf;
					}
					else
					{
						srcTex = dstTex; // final result is here
					}
				}

				// ---------- Pass 2: combine (final blurred -> scene RT) ----------
				// AddSmooth blend (SrcBlend=ONE, DestBlend=INVSRCCOLOR)
				// is declared inside the .fx pass block, so we don't
				// Clear the scene RT — the shader's pass state mixes
				// bloom over the existing image.
				m_pDevice->SetRenderTarget(0, pSceneRT);
				pBloom->SetTexture(m_hBloomSceneTextureParam, srcTex);
				pBloom->BeginPass(2);
				pBloom->CommitChanges();
				m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bloomQuad, sizeof(EmitterInstance::Vertex));
				pBloom->EndPass();

				pBloom->End();
			}

			// Unbind textures we sourced from to avoid driver warnings
			// about a texture being bound as both RT and sampler on the
			// next pass (the heat pass that follows binds its own RT
			// immediately, but be defensive).
			pBloom->SetTexture(m_hBloomSceneTextureParam, NULL);

			SAFE_RELEASE(pSceneRT);
			SAFE_RELEASE(pPongSurface);
			SAFE_RELEASE(pPingSurface);
			SAFE_RELEASE(pBloom);
		}
	}

	// Now render to the heat texture
	IDirect3DSurface9* pDistortSurface;
	m_pDistortTexture->GetSurfaceLevel(0, &pDistortSurface);
	m_pDevice->SetRenderTarget(0, pDistortSurface);
	SAFE_RELEASE(pDistortSurface);

	m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(129,128,255), 1.0f, 0);
	for (auto& instance : m_instances)
    {
        instance->RenderHeat(m_pDevice);
	}

	// Now render to the screen
	m_pDevice->SetRenderTarget(0, pScreenSurface);
    m_pDevice->SetDepthStencilSurface(pDepthSurface);
	SAFE_RELEASE(pScreenSurface);
    SAFE_RELEASE(pDepthSurface);
	m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0,0,0), 0.0f, 0);

	static const EmitterInstance::Vertex quad[4] = {
		{D3DXVECTOR3(-1,-1,0), D3DXVECTOR2(0, 1), D3DXVECTOR4(1,1,1,1)},
		{D3DXVECTOR3( 1,-1,0), D3DXVECTOR2(1, 1), D3DXVECTOR4(1,1,1,1)},
		{D3DXVECTOR3(-1, 1,0), D3DXVECTOR2(0, 0), D3DXVECTOR4(1,1,1,1)},
		{D3DXVECTOR3( 1, 1,0), D3DXVECTOR2(1, 0), D3DXVECTOR4(1,1,1,1)}
	};

    ID3DXEffect* pEffect = m_pDistortShader->getD3DEffect();
    m_pDevice->SetTexture(0, m_pSceneTexture);
    m_pDevice->SetTexture(1, m_pDistortTexture);
	pEffect->SetTexture("SceneTexture",      m_pSceneTexture);
	pEffect->SetTexture("DistortionTexture", m_pDistortTexture);

	UINT nPasses = 1;
	pEffect->Begin(&nPasses, 0);
	for (UINT i = 0; i < nPasses; i++)
	{
		pEffect->BeginPass(i);
		m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(EmitterInstance::Vertex));
		pEffect->EndPass();
	}
	pEffect->End();
    SAFE_RELEASE(pEffect);

	m_pDevice->EndScene();
	m_pDevice->Present(NULL, NULL, NULL, NULL);
	return true;
}

IDirect3DTexture9* Engine::GetTexture(const string& name) const
{
	return m_textureManager.getTexture(m_pDevice, name);
}

void Engine::OnParticleSystemChanged(int track)
{
	for (auto& instance : m_instances)
    {
		instance->onParticleSystemChanged(*this, track);
	}
}

void Engine::GetViewPort(D3DVIEWPORT9* viewport) const
{
	m_pDevice->GetViewport(viewport);
}

const Engine::Camera& Engine::GetCamera() const
{
	return m_eye;
}

void Engine::SetCamera( const Camera& camera )
{
	m_eye = camera;

	// Construct matrices
	D3DXMatrixLookAtRH(&m_view, &camera.Position, &camera.Target, &camera.Up );
	D3DXMatrixMultiply(&m_viewProjection, &m_view, &m_projection);

	// Create some resulting matrices
	m_viewRotation = m_view;
	m_viewRotation._41 = m_viewRotation._42 = m_viewRotation._43 = 0.0;
	D3DXMatrixInverse(&m_billboard,   NULL, &m_viewRotation);
    D3DXMatrixInverse(&m_viewInverse, NULL, &m_view);

    // Set matrices
	m_pDevice->SetTransform(D3DTS_VIEW,       &m_view);
	m_pDevice->SetTransform(D3DTS_PROJECTION, &m_projection);
}

void Engine::SetGround(bool enable)			        { m_showGround = enable; }
void Engine::SetGroundZ(float z)			        { m_groundZ    = z;      }
void Engine::SetBackground(COLORREF color)		    { m_background = color; }
void Engine::SetHeatDebug(bool debug)		        { m_debugHeat  = debug;  }
void Engine::SetBloom(bool enable)                  { m_bloomEnabled  = enable; }
void Engine::SetBloomStrength(float v)              { m_bloomStrength = v; }
void Engine::SetBloomCutoff(float v)                { m_bloomCutoff   = v; }
void Engine::SetBloomSize(float v)                  { m_bloomSize     = v; }
void Engine::SetWind(const D3DXVECTOR3& wind)       { m_wind = wind; }
void Engine::SetGravity(const D3DXVECTOR3& gravity) { D3DXVec3Normalize(&m_gravity, &gravity); }
void Engine::SetLight(LightType which, const Light& light)
{
	int index = 0;
	switch (which)
	{
		case LT_SUN:	index = 0; break;
		case LT_FILL1:	index = 1; break;
		case LT_FILL2:	index = 2; break;
	}
	m_lights[index] = light;
	
	// Calculate direction from position
    m_lights[index].Direction   = -m_lights[index].Position;
    m_lights[index].Direction.w = 0.0f;
	D3DXVec4Normalize(&m_lights[index].Direction, &m_lights[index].Direction);

	// Recalculate Spherical Harmonics matrices
	SPH_Calculate_Matrices(m_sphLightFill, &m_lights[1], 2, m_ambient);
	SPH_Calculate_Matrices(m_sphLightAll,  &m_lights[0], 3, m_ambient);
}

void Engine::SetAmbient(const D3DXVECTOR4& color)
{
	m_ambient = color;

	// Recalculate Spherical Harmonics matrices
	SPH_Calculate_Matrices(m_sphLightFill, &m_lights[1], 2, m_ambient);
	SPH_Calculate_Matrices(m_sphLightAll,  &m_lights[0], 3, m_ambient);
}

void Engine::Reset()
{
	ReleaseBloomTargets();
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
    SAFE_RELEASE(m_pDepthStencilSurface);

	// Reset device
	m_presentationParameters.BackBufferWidth  = 0;
    m_presentationParameters.BackBufferHeight = 0;
	m_presentationParameters.BackBufferCount  = 1;
    m_presentationParameters.Windowed         = true;

	m_pDistortShader->OnLostDevice();
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        m_pShaders[i]->OnLostDevice();
    }
	if (m_pBloomEffect != NULL) m_pBloomEffect->OnLostDevice();
	if (FAILED(m_pDevice->Reset(&m_presentationParameters)))
	{
		throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));
	}
	m_pDistortShader->OnResetDevice();
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        m_pShaders[i]->OnResetDevice();
    }
	if (m_pBloomEffect != NULL) m_pBloomEffect->OnResetDevice();

	ResetParameters();
}

void Engine::ResetParameters()
{
	if (m_presentationParameters.BackBufferWidth > 0 && m_presentationParameters.BackBufferHeight > 0)
	{
		// http://www.gamedev.net/columns/hardcore/shadowvolume/page4.asp
		float n = 1.0f;
		D3DXMatrixPerspectiveFovRH(&m_projection, D3DXToRadian(45), (float)m_presentationParameters.BackBufferWidth / m_presentationParameters.BackBufferHeight, n, 1000.0f );
		m_projection._33 = -1.0f;
		m_projection._43 = -2 * n;

		// Create dynamic textures
		if (FAILED(m_pDevice->CreateTexture(m_presentationParameters.BackBufferWidth, m_presentationParameters.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pSceneTexture, NULL)))
		{
			throw runtime_error("Unable to create texture");
		}

		if (FAILED(m_pDevice->CreateTexture(m_presentationParameters.BackBufferWidth, m_presentationParameters.BackBufferHeight, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pDistortTexture, NULL)))
		{
			SAFE_RELEASE(m_pSceneTexture);
			throw runtime_error("Unable to create texture");
		}

        if (FAILED(m_pDevice->CreateDepthStencilSurface(m_presentationParameters.BackBufferWidth, m_presentationParameters.BackBufferHeight, m_presentationParameters.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, 0, TRUE, &m_pDepthStencilSurface, NULL)))
        {
            SAFE_RELEASE(m_pDistortTexture);
			SAFE_RELEASE(m_pSceneTexture);
			throw runtime_error("Unable to create depth buffer");
        }

		// Full-resolution ping-pong RTs for the bloom blur. The
		// shader's blur kernel is measured in source-texel units
		// via m_resolutionConstants.zw — keeping these at full
		// scene resolution means one set of values drives all
		// passes and matches what the canonical EAW engine does.
		// Failure to allocate disables bloom for this session but
		// doesn't block the rest of the renderer.
		ReleaseBloomTargets();
		UINT bloomW = m_presentationParameters.BackBufferWidth;
		UINT bloomH = m_presentationParameters.BackBufferHeight;
		if (FAILED(m_pDevice->CreateTexture(bloomW, bloomH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pBloomPing, NULL))
		 || FAILED(m_pDevice->CreateTexture(bloomW, bloomH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pBloomPong, NULL)))
		{
			// Don't throw — bloom is an optional post-process. Just
			// disable it for this device-reset cycle and continue.
			ReleaseBloomTargets();
		}

		// Reset states
		m_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		m_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

		// Reset vertex declaration
		m_pDevice->SetVertexDeclaration(m_pDeclaration);

		// Set color texture properties
		m_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		m_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		m_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		m_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		m_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		m_pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		m_pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		m_pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

		// Set normal texture properties
		m_pDevice->SetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		m_pDevice->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);
		m_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		m_pDevice->SetTextureStageState(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		m_pDevice->SetTextureStageState(1, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
		m_pDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		m_pDevice->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		m_pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);

		// Set world matrix
		D3DXMATRIX identity;
		D3DXMatrixIdentity(&identity);
		m_pDevice->SetTransform(D3DTS_WORLD, &identity);

		// Reset camera
		SetCamera(m_eye);
	}
}

D3DFORMAT Engine::GetDepthStencilFormat(D3DFORMAT AdapterFormat, bool withStencilBuffer)
{
	static const D3DFORMAT DepthStencilFormatsNS[7] = { D3DFMT_D32,   D3DFMT_D24S8,  D3DFMT_D24X4S4, D3DFMT_D24FS8, D3DFMT_D24X8, D3DFMT_D16, D3DFMT_D15S1 };
	static const D3DFORMAT DepthStencilFormatsS[4]  = { D3DFMT_D24S8, D3DFMT_D24FS8, D3DFMT_D24X4S4, D3DFMT_D15S1 };

	int              nFormats = (withStencilBuffer) ? 4 : 7;
	const D3DFORMAT* Formats  = (withStencilBuffer) ? DepthStencilFormatsS : DepthStencilFormatsNS;

	for (int i = 0; i < nFormats; i++)
	{
		if (SUCCEEDED(m_pDirect3D->CheckDeviceFormat     (D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, AdapterFormat, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, Formats[i])))
		if (SUCCEEDED(m_pDirect3D->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, AdapterFormat, AdapterFormat, Formats[i])))
		{
			return Formats[i];
		}
	}

	return D3DFMT_UNKNOWN;
}

D3DMULTISAMPLE_TYPE Engine::GetMultiSampleType(DWORD* MultiSampleQuality, D3DFORMAT DisplayFormat, D3DFORMAT DepthStencilFormat, BOOL Windowed)
{
	D3DMULTISAMPLE_TYPE MultiSampleTypes[16] = {
		D3DMULTISAMPLE_16_SAMPLES, D3DMULTISAMPLE_15_SAMPLES, D3DMULTISAMPLE_14_SAMPLES, D3DMULTISAMPLE_13_SAMPLES,
		D3DMULTISAMPLE_12_SAMPLES, D3DMULTISAMPLE_11_SAMPLES, D3DMULTISAMPLE_10_SAMPLES, D3DMULTISAMPLE_9_SAMPLES,
		D3DMULTISAMPLE_8_SAMPLES, D3DMULTISAMPLE_7_SAMPLES, D3DMULTISAMPLE_6_SAMPLES, D3DMULTISAMPLE_5_SAMPLES,
		D3DMULTISAMPLE_4_SAMPLES, D3DMULTISAMPLE_3_SAMPLES, D3DMULTISAMPLE_2_SAMPLES, D3DMULTISAMPLE_NONE
	};

    for (int i = 0; i < 16; i++)
	{
		if (SUCCEEDED(m_pDirect3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, DisplayFormat,      Windowed, MultiSampleTypes[i], MultiSampleQuality)))
		if (SUCCEEDED(m_pDirect3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, DepthStencilFormat, Windowed, MultiSampleTypes[i], MultiSampleQuality)))
		{
			(*MultiSampleQuality)--;
			return MultiSampleTypes[i];
		}
	}

	*MultiSampleQuality = 0;
	return D3DMULTISAMPLE_NONE;
}

Engine::Engine(HWND hFocus, HWND hDevice, ITextureManager& textureManager, IShaderManager& shaderManager)
    : m_textureManager(textureManager), m_shaderManager(shaderManager)
{
	// Zero shader pointers up front so partial-failure cleanup is safe
	m_pDistortShader = NULL;
	for (int i = 0; i < NUM_SHADERS; i++) m_pShaders[i] = NULL;
	m_pBloomEffect = NULL;
	m_pBloomPing   = NULL;
	m_pBloomPong   = NULL;
	m_hBloomStrength = m_hBloomCutoff = m_hBloomSize = NULL;
	m_hBloomIteration = m_hBloomSceneTextureParam = NULL;
	m_hBloomResolutionConstants = NULL;
	m_hBloomTechnique = NULL;
	m_bloomPassCount  = 0;

	// Initialize members
	m_showGround     = true;
	m_groundZ        = 0.0f;
	m_debugHeat      = false;
	m_bloomEnabled   = false;
	m_bloomReady     = false;
	// Defaults match the canonical EAW Terrain Editor's brand-new
	// (Untitled) map — the blank-slate values the editor ships
	// when no specific map's been authored yet. The shader's
	// source defaults (1.0 / 0.1 / 0.25) are placeholders the
	// game overwrites at runtime and aren't the canonical
	// "fresh start" values.
	m_bloomStrength  = 0.00f;
	m_bloomCutoff    = 0.90f;
	m_bloomSize      = 0.10f;
	m_gravity        = D3DXVECTOR3(0,0,-1);
	m_wind           = D3DXVECTOR3(0,0,0);
	m_eye.Position   = D3DXVECTOR3(0,-250,125);
	m_eye.Target     = D3DXVECTOR3(0,0,0);
	m_eye.Up		 = D3DXVECTOR3(0,0,1);
    m_numEmitters    = 0;
    m_numParticles   = 0;
    m_ambient        = D3DXVECTOR4(0,0,0,0);
    m_background     = RGB(0x14,0x08,0x34);

	//
	// Initialize Direct3D
	//
	m_pDirect3D = Direct3DCreate9(D3D_SDK_VERSION);
	if (m_pDirect3D == NULL)
	{
		throw runtime_error("Unable to initialize Direct3D");
	}

	ZeroMemory(&m_presentationParameters, sizeof(m_presentationParameters));
	m_presentationParameters.BackBufferFormat       = D3DFMT_UNKNOWN;
	m_presentationParameters.SwapEffect             = D3DSWAPEFFECT_DISCARD;
	m_presentationParameters.hDeviceWindow          = hDevice;
	m_presentationParameters.Windowed               = TRUE;
	m_presentationParameters.EnableAutoDepthStencil = TRUE;
	m_presentationParameters.Flags                  = D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL;
	m_presentationParameters.PresentationInterval   = D3DPRESENT_INTERVAL_DEFAULT;

	D3DDISPLAYMODE DisplayMode;
	if (FAILED(m_pDirect3D->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &DisplayMode)))
	{
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to get current display mode");
	}

	if ((m_presentationParameters.AutoDepthStencilFormat = GetDepthStencilFormat(DisplayMode.Format, false)) == D3DFMT_UNKNOWN)
	{
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to find a matching depth buffer format");
	}

	m_presentationParameters.MultiSampleType = GetMultiSampleType(&m_presentationParameters.MultiSampleQuality, DisplayMode.Format, m_presentationParameters.AutoDepthStencilFormat, m_presentationParameters.Windowed);

	// Create device (first try hardware, then software)
	if (FAILED(m_pDirect3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocus, D3DCREATE_HARDWARE_VERTEXPROCESSING, &m_presentationParameters, &m_pDevice)))
	if (FAILED(m_pDirect3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocus, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &m_presentationParameters, &m_pDevice)))
	{
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to create render device");
	}

	// Create vertex declaration
	if (FAILED(m_pDevice->CreateVertexDeclaration(ParticleElements, &m_pDeclaration)))
	{
		SAFE_RELEASE(m_pDevice);
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to create vertex declaration");
	}

	// Create ground texture
	if (FAILED(D3DXCreateTextureFromResource(m_pDevice, NULL, MAKEINTRESOURCE(IDB_GROUND), &m_pGroundTexture)))
	{
		SAFE_RELEASE(m_pDeclaration);
		SAFE_RELEASE(m_pDevice);
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to load ground texture");
	}

	// Distortion shader (built-in resource, not part of the hot-reloadable set)
    ID3DXEffect* pDistortEffect = NULL;
	if (FAILED(D3DXCreateEffectFromResource(m_pDevice, NULL, MAKEINTRESOURCE(IDS_SCENEHEAT), NULL, NULL, D3DXFX_NOT_CLONEABLE, NULL, &pDistortEffect, NULL)))
	{
		SAFE_RELEASE(m_pGroundTexture);
		SAFE_RELEASE(m_pDeclaration);
		SAFE_RELEASE(m_pDevice);
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to load a shader");
	}
    m_pDistortShader = new Effect(pDistortEffect);
    pDistortEffect->SetFloat("DistortionAmount", 0.50f);
    SAFE_RELEASE(pDistortEffect);

	// Initial shader load — same all-or-nothing semantics as ReloadShaders().
	// On failure we tear the device down and throw, just like before.
	if (!ReloadShaders())
	{
		SAFE_RELEASE(m_pDistortShader);
		SAFE_RELEASE(m_pGroundTexture);
		SAFE_RELEASE(m_pDeclaration);
		SAFE_RELEASE(m_pDevice);
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to load a shader");
	}

    Light sun = {
        D3DXVECTOR4( 1.0f, 1.0f, 1.0f, 1.0f),
        D3DXVECTOR4( 0.0f, 0.0f, 0.0f, 0.0f),
        D3DXVECTOR4( 1.0f, 0.0f, 0.0f, 0.0f),
        D3DXVECTOR4( 0.0f, 0.0f, 0.0f, 0.0f) 
    };

    Light fill = {
        D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f),
        D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f),
        D3DXVECTOR4(0.0f, 0.0f, 0.0f, 1.0f),
        D3DXVECTOR4(0.0f, 0.0f, 0.0f, 0.0f) 
    };

    SetLight(LT_SUN,   sun);
    SetLight(LT_FILL1, fill);
    SetLight(LT_FILL2, fill);
	ResetParameters();
}

Engine::~Engine()
{
	ReleaseBloomTargets();
	SAFE_RELEASE(m_pBloomEffect);
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        SAFE_RELEASE(m_pShaders[i]);
    }
    SAFE_RELEASE(m_pDepthStencilSurface);
	SAFE_RELEASE(m_pDistortShader);
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
	SAFE_RELEASE(m_pGroundTexture);
	SAFE_RELEASE(m_pDeclaration);
	SAFE_RELEASE(m_pDevice);
	SAFE_RELEASE(m_pDirect3D);
}
