#include <algorithm>
#include <assert.h>
#include <vector>
#include <cstdint>
#include <cmath>     // [hard-guard] std::isfinite for the estimate clamp
#include <cctype>    // tolower for case-insensitive hardpoint bone matching
#include <set>       // hardpoint damage-bone hide set
#include <string>
#include <cstdio>    // [shadow-leak hunt] fopen/fprintf for the ALO_DUMP_RSTATE probe
#include "engine.h"
#include "exceptions.h"
#include "ResourceLimits.h"   // kMaxTextureAssetBytes (asset-read size caps, #415)
#include "resource.h"
#include "ParticleSystemInstance.h"
#include "EmitterInstance.h"
#include "SphericalHarmonics.h"
#include "utils.h"     // WideToAnsi for custom-slot path bridging
#include "host/AlphaCompositor.h"
#include "GizmoSizing.h"   // pure screen-uniform gizmo-handle formula
#include "PlaneHandle.h"   // pure ground-plane handle math (RayPlaneOffset, HandleHit, etc.)
#include "GizmoRibbon.h"   // aesthetics: camera-facing ribbon quad expansion
#include "ParticleMipFilter.h" // #481: ALO_PARTICLE_MIPFILTER override parser (unit-tested)
#include "RingFade.h"      // aesthetics: ring back-face alpha falloff
#include "SelectionBoxStyle.h" // aesthetics: selection-box bracket/dash geometry
#include "ReferenceObjectWorld.h" // pure reference-object world-matrix builder (scale+rot+trans)
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

// slot 0 is Off (no resource); slots 1-8 map to bundled skydome textures.
// RCDATA entries for IDR_SKYDOME_* are added in Task 5; until then,
// FindResource for slots 1-8 returns NULL and ReloadSkydomeTexture returns false.
static const int kSkydomeBundledResources[Engine::kSkydomeBundledCount] = {
    0,                       // 0: Off
    IDR_SKYDOME_SPACE,       // 1
    IDR_SKYDOME_ATMOSPHERE,  // 2
    IDR_SKYDOME_SUNSET,      // 3
    IDR_SKYDOME_DAWN,        // 4
    IDR_SKYDOME_NIGHT,       // 5
    IDR_SKYDOME_OVERCAST,    // 6
    IDR_SKYDOME_STUDIO,      // 7
    IDR_SKYDOME_INDOOR,      // 8
};

// Parallel table of in-archive paths for slots 1-8. Routed
// through FileManager so the mod-overlay → loose-file → MEG-archive chain
// resolves them automatically (same path emitter textures take). Slot 0 has
// no game asset (Solid colour). When FileManager can't resolve a path (no
// base game installed, the active mod is missing the file), ReloadSkydomeTexture
// falls back to the procedural RCDATA at kSkydomeBundledResources[slot] so
// the slot still renders something useful.
static const char* const kSkydomeBundledGamePaths[Engine::kSkydomeBundledCount] = {
    NULL,                                              // 0: Solid colour
    "DATA\\ART\\TEXTURES\\W_SKYSTORM01.DDS",           // 1: Storm
    "DATA\\ART\\TEXTURES\\W_SKY_MURK_CLOUDS.DDS",      // 2: Murky Clouds
    "DATA\\ART\\TEXTURES\\W_SKY_SMOG_CLOUDS.DDS",      // 3: Smog Clouds
    "DATA\\ART\\TEXTURES\\W_SKYBLUE_HORIZON.DDS",      // 4: Blue Horizon
    "DATA\\ART\\TEXTURES\\W_SKYBLUE01.DDS",            // 5: Blue Sky
    "DATA\\ART\\TEXTURES\\W_SKYORANGE_HORIZON.DDS",    // 6: Orange Horizon
    "DATA\\ART\\TEXTURES\\W_SKYORANGE00.DDS",          // 7: Orange Sky
    "DATA\\ART\\TEXTURES\\W_SKYSTORM_VOLCANIC00.DDS",  // 8: Volcanic Storm
};

// Public getters so main.cpp can build thumbnails without duplicating the tables.
const int* Engine::GetSkydomeBundledResources()
{
    return kSkydomeBundledResources;
}

const char* const* Engine::GetSkydomeBundledGamePaths()
{
    return kSkydomeBundledGamePaths;
}

// Helper: try to load a texture from a file resolved via FileManager. Returns
// the texture (caller owns one ref) on success, NULL on miss. Used by both
// the curated slot path and the custom slot path.
static IDirect3DTexture9* LoadTextureViaFileManager(IDirect3DDevice9* pDevice,
                                                      IFileManager& fileManager,
                                                      const std::string& path)
{
    IFile* file = fileManager.getFile(path);
    if (file == NULL) return NULL;
    // ReadAndRelease handles the exact-byte read
    // (pre-fix this ignored the read's return value) and Releases the
    // file reference. On empty or short read it throws ReadException,
    // which we map to NULL for caller compatibility.
    std::vector<unsigned char> bytes;
    try
    {
        bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes);
    }
    catch (ReadException&)
    {
        return NULL;
    }
    IDirect3DTexture9* pTex = NULL;
    HRESULT hr = D3DXCreateTextureFromFileInMemory(pDevice, bytes.data(), (unsigned long)bytes.size(), &pTex);
    return SUCCEEDED(hr) ? pTex : NULL;
}

D3DVERTEXELEMENT9 Engine::ParticleElements[] = {
	{0, offsetof(EmitterInstance::Vertex, Position),  D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, 
	{0, offsetof(EmitterInstance::Vertex, Normal),    D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0}, 
	{0, offsetof(EmitterInstance::Vertex, TexCoord0), D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, 
	{0, offsetof(EmitterInstance::Vertex, TexCoord1), D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1}, 
	{0, offsetof(EmitterInstance::Vertex, Color),     D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0}, 
	D3DDECL_END()
};

// Preview clock with pause / frame-step support.
//
// Every consumer of "simulation now" — emitter spawn time, particle
// Update dt, the shader hTime uniform, the spawner driver dt — funnels
// through GetTimeF(). Freezing time at this single site freezes the
// whole simulation while Engine::Render() keeps drawing, which is the
// analysis behaviour we want.
//
// Two statics maintain a continuous simulation clock across pause
// boundaries:
//
//   wall          = monotonic seconds since process start
//   g_pauseOffset = wall seconds "lost" to pause / step (subtracted
//                   from wall to produce simulation time)
//   simTime       = wall - g_pauseOffset           when running
//                 = g_pauseAnchor                  when paused
//
// On pause:  anchor = current simTime; clock freezes there.
// On step:   anchor advances by (n / 60.0f) seconds while still paused.
//            Stepping persists past resume because offset is re-derived
//            from the (possibly bumped) anchor at resume time.
// On resume: offset = wall - anchor; simTime continues exactly where
//            the anchor pointed, with no time-warp pop.
//
// All state is process-local and resets to "not paused" at startup;
// no persistence by design.
static bool   g_previewPaused      = false;
static TimeF  g_previewPauseAnchor = 0.0f;   // simTime while paused
static TimeF  g_pauseOffset        = 0.0f;   // wall - simTime when running

static TimeF WallTimeF()
{
    static auto start = GetTickCount();
    return (GetTickCount() - start) / 1000.0f;
}

TimeF GetTimeF()
{
    if (g_previewPaused) return g_previewPauseAnchor;
    return WallTimeF() - g_pauseOffset;
}

void SetPreviewPaused(bool paused)
{
    if (paused == g_previewPaused) return;
    if (paused)
    {
        // Freeze at the current simulation time.
        g_previewPauseAnchor = WallTimeF() - g_pauseOffset;
    }
    else
    {
        // Re-derive offset from the (possibly stepped) anchor so the
        // running clock resumes from exactly the anchor's value. This
        // correctly accounts for any StepPreviewFrames calls made
        // during the pause.
        g_pauseOffset = WallTimeF() - g_previewPauseAnchor;
    }
    g_previewPaused = paused;
}

bool IsPreviewPaused()
{
    return g_previewPaused;
}

// #481 capture determinism: pause the preview clock at a FIXED sim time.
// SetPreviewPaused(true) freezes at the current (process-age-dependent) wall
// time, which leaves any m_time-consuming shader (scanline scroll, heat phase)
// run-dependent even under seeded RNG + fixed frame stepping. Headless capture
// paths freeze at a constant anchor instead so shader time is identical every
// run. Unpause (SetPreviewPaused(false)) re-derives the wall offset from the
// anchor, so this composes with the existing pause/step machinery.
void FreezePreviewClockAt(TimeF anchor)
{
    g_previewPaused      = true;
    g_previewPauseAnchor = anchor;
}

void StepPreviewFrames(int frames)
{
    if (!g_previewPaused || frames <= 0) return;
    // Advance the frozen anchor by N notional 60 Hz frames. The next
    // Update() call sees a single dt of (frames / 60.0f) seconds — for
    // the engine's forward-Euler / track-cursor integration that's
    // visually indistinguishable from N small dts at the granularities
    // we care about.
    g_previewPauseAnchor += frames / 60.0f;
}

ParticleSystemInstance* Engine::SpawnParticleSystem(const ParticleSystem& system, Object3D* parent)
{
    // [hard-guard spawn-time check] Refuse the placement (and clear the
    // rest of the preview) when the estimated TOTAL — already-placed
    // instances plus this one — exceeds the guard cap. Estimate 0 = no
    // estimate pushed yet = gate inert (runtime budget is the backstop).
    // Runs BEFORE any allocation, so Clear() only touches pre-existing
    // instances — no re-entrancy hazard for the not-yet-created one.
    // A nullptr return now means EXACTLY one thing: a gate refusal.
    if (m_overloadGuardEnabled && m_estimatedPerInstance > 0.0)
    {
        const double projected = (GetNumInstances() + 1) * m_estimatedPerInstance;
        if (projected > (double)m_maxPreviewParticles)
        {
            m_spawnRefusal = { projected, m_maxPreviewParticles, GetNumInstances() + 1 };
            m_spawnRefusalPending = true;
            Clear();
            return nullptr;
        }
    }

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

    // Overload guard: population is gone, so refill the spawn budget and
    // drop the latch immediately (don't wait for the next Update tick).
    m_spawnBudget       = m_maxPreviewParticles;
    m_overloadActive    = false;
    m_overloadThisFrame = false;
    m_lastOverloadTime  = -1.0f;
}

void Engine::SetOverloadGuard(bool enabled, int maxParticles)
{
	if (maxParticles < kMinConfigurableParticles) maxParticles = kMinConfigurableParticles;
	if (maxParticles > kMaxConfigurableParticles) maxParticles = kMaxConfigurableParticles;
	m_overloadGuardEnabled = enabled;
	m_maxPreviewParticles  = maxParticles;
	m_maxPreviewInstances  = maxParticles / kInstancesDivisor;
	if (!enabled)
	{
		// Latch off NOW — mirrors Clear()'s immediate reset so the UI
		// banner drops without waiting for the clear-delay debounce.
		m_overloadActive    = false;
		m_overloadThisFrame = false;
		m_lastOverloadTime  = -1.0f;
	}
#ifndef NDEBUG
	printf("[overload] guard config: enabled=%d maxParticles=%d (instances=%d)\n",
	       enabled ? 1 : 0, m_maxPreviewParticles, m_maxPreviewInstances);
	fflush(stdout);
#endif
}

void Engine::SetEstimatedLoad(double perInstance)
{
    if (perInstance < 0.0 || !std::isfinite(perInstance)) perInstance = 0.0;
    m_estimatedPerInstance = perInstance;
    // [hard-guard edit-time check] A parameter revision can push the
    // already-placed preview over budget: clear it and record the
    // refusal so the banner explains what happened.
    const int n = GetNumInstances();
    if (m_overloadGuardEnabled && perInstance > 0.0 && n > 0 &&
        n * perInstance > (double)m_maxPreviewParticles)
    {
        m_spawnRefusal = { n * perInstance, m_maxPreviewParticles, n };
        m_spawnRefusalPending = true;
        Clear();
    }
}

bool Engine::TakeSpawnRefusal(SpawnRefusal* out)
{
    if (!m_spawnRefusalPending) return false;
    if (out) *out = m_spawnRefusal;
    m_spawnRefusalPending = false;
    return true;
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
// "<install dir>\". Used to place the bloom diagnostic
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

// [soft-shadows] Release the screen-space shadow-mask render targets. Both are
// D3DPOOL_DEFAULT (texture + matching-MSAA surface) so they must be released
// before any device Reset/ResetEx, mirroring the bloom + MSAA RT lifecycle.
void Engine::ReleaseShadowMaskTargets()
{
	SAFE_RELEASE(m_pShadowMask);
	SAFE_RELEASE(m_pShadowMaskMsaa);
}

// [soft-shadows] Introspect the freshly-loaded StencilDarkenFinalBlur effect.
// Mirrors InitBloomEffect's all-or-nothing handle gate: cache the blurAmt scalar
// + the WORLDVIEWPROJECTION matrix (the blur VS transforms the full-screen quad
// by it — we drive it to identity) + the first technique that validates on this
// device. Any missing piece => m_shadowBlurReady=false => soft falls back to hard.
void Engine::InitShadowBlurEffect()
{
	m_shadowBlurReady = false;
	m_hShadowBlurAmt  = NULL;
	m_hShadowBlurWvp  = NULL;
	m_hShadowBlurTech = NULL;

	if (m_pShadowBlurEffect == NULL)
	{
		printf("[soft-shadow] StencilDarkenFinalBlur.fx not loaded — soft shadows unavailable (hard fallback)\n");
		fflush(stdout);
		return;
	}

	ID3DXEffect* pFx = m_pShadowBlurEffect->getD3DEffect();   // AddRef'd
	if (pFx == NULL) { printf("[soft-shadow] null ID3DXEffect — hard fallback\n"); fflush(stdout); return; }

	D3DXEFFECT_DESC desc;
	if (FAILED(pFx->GetDesc(&desc)))
	{
		printf("[soft-shadow] GetDesc failed — hard fallback\n"); fflush(stdout);
		SAFE_RELEASE(pFx);
		return;
	}

	m_hShadowBlurAmt = pFx->GetParameterByName(NULL, "blurAmt");
	m_hShadowBlurWvp = pFx->GetParameterBySemantic(NULL, "WORLDVIEWPROJECTION");

	for (UINT i = 0; i < desc.Techniques; ++i)
	{
		D3DXHANDLE hTech = pFx->GetTechnique(i);
		if (hTech && SUCCEEDED(pFx->ValidateTechnique(hTech)))
		{
			m_hShadowBlurTech = hTech;
			break;
		}
	}

	SAFE_RELEASE(pFx);

	// blurAmt + the WVP matrix + a valid technique are all required. blurAmt
	// doubles as a fingerprint: the ShaderManager default placeholder lacks it,
	// so requiring it rejects a fallback-to-default (which would render garbage).
	m_shadowBlurReady = (m_hShadowBlurAmt != NULL)
	                 && (m_hShadowBlurWvp != NULL)
	                 && (m_hShadowBlurTech != NULL);

	printf("[soft-shadow] StencilDarkenFinalBlur: blurAmt=%s wvp=%s tech=%s -> %s\n",
	       m_hShadowBlurAmt  ? "found" : "MISSING",
	       m_hShadowBlurWvp  ? "found" : "MISSING",
	       m_hShadowBlurTech ? "found" : "MISSING",
	       m_shadowBlurReady ? "READY (soft available)" : "UNAVAILABLE (hard fallback)");
	fflush(stdout);
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

	// [soft-shadows] Optional blur-composite effect for soft model shadows.
	// Same resolution chain + graceful-NULL contract as bloom. Already in the
	// shader manifest, but we hold a dedicated handle with cached params so the
	// soft path doesn't depend on a manifest-index lookup.
	SAFE_RELEASE(m_pShadowBlurEffect);
	m_pShadowBlurEffect = m_shaderManager.getShader(m_pDevice, "Engine\\StencilDarkenFinalBlur.fx");
	InitShadowBlurEffect();

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
	// Re-resolve the active skydome texture too, so a mod
	// override of (say) DATA\ART\TEXTURES\W_SKYBLUE01.DDS takes effect on
	// the next render. No-op when the slot is Off.
	if (m_skydomeIndex != kSkydomeOffSlot)
	{
		ReloadSkydomeTexture(m_skydomeIndex);
	}
	// re-resolve the game domes so a changed .alo / texture (and, on the
	// mod-switch path where ReloadShaders->ShaderManager::Clear ran first, a
	// changed .fxo) is picked up. On a standalone Reload-Textures the cached
	// shader is still valid, so the re-getShader is a cheap no-op.
	if (!m_skydomePrimaryName.empty() || !m_skydomeSecondaryName.empty())
	{
		RebuildSkydomeMeshes();
	}
	// A mod/submod switch changes the object catalog -> invalidate it so the
	// next Update() rebuilds it OFF the UI thread (++generation discards any in-flight
	// build started under the OLD context). But ReloadTextures ALSO runs on texture-only
	// paths (F5 reload, every file open) where the mod context is UNCHANGED -- invalidating
	// there would needlessly rebuild an identical catalog AND make the selected reference
	// object vanish for the async rebuild. So invalidate ONLY when the active mod/submod
	// roots actually differ from what the current/in-flight catalog reflects.
	// Compare the full content-root stack (was mod path + submods) so a
	// same-mod layer REORDER also invalidates, not just a mod/submod-set change.
	const bool modContextChanged =
		m_fileManager.GetContentRoots() != m_catalogContextRoots;
	if (modContextChanged)
	{
		m_referenceCatalogBuilt = false;
		++m_catalogGeneration;
	}
	// Reference object: on a real mod-context change, clear the SHOWN selection to None
	// NOW (no stale id during the async rebuild) and let the catalog-ready retry restore
	// it iff it still exists -- ResolveDesiredReference takes its not-built branch
	// (the catalog was just invalidated above) and does both. On a texture-only reload the
	// catalog is still valid, so re-resolve the shown object in place (refresh its
	// textures, no vanish), exactly as before.
	if (modContextChanged)
	{
		ResolveDesiredReference();
	}
	else if (!m_referenceObjectName.empty())
	{
		RebuildReferenceObjectMesh();
	}
	printf("[Textures] Reload: cache cleared, %d instance(s) notified\n", n); fflush(stdout);
}

void Engine::Update()
{
	TimeF currentTime = GetTimeF();

	EaseReferenceDisplay();   // ease the render-only display transform (smooth gizmo/object motion)

	// Harvest a finished background catalog build (worker -> UI handoff) and
	// (re)kick one when wanted but missing. This swap is on the UI thread, so the other
	// catalog readers (EnumerateReferenceObjects / RebuildReferenceObjectMesh, also UI
	// thread) never race the worker -- the worker only touched its OWN FileManager.
	{
		std::unique_ptr<GameObjectCatalog> ready;
		uint64_t readyGen = 0;
		{
			std::lock_guard<std::mutex> lock(m_catalogMutex);
			if (m_pendingCatalog) { ready = std::move(m_pendingCatalog); readyGen = m_pendingCatalogGen; }
		}
		if (ready)
		{
			if (m_catalogThread.joinable()) m_catalogThread.join();   // worker published + cleared building
			if (readyGen == m_catalogGeneration)                       // not stale (no mod switch mid-build)
			{
				m_referenceCatalog      = std::move(*ready);
				m_referenceCatalogBuilt = true;
				m_catalogJustReady      = true;                        // host -> emit state-changed (picker re-queries)
				if (m_referenceMeshDeferred)                            // a deferred/selected object was waiting
				{
					// Catalog now ready: existence-gated restore -- show + load the
					// desired object iff it exists in the new stack, else stay None.
					// ResolveDesiredReference clears m_referenceMeshDeferred itself.
					ResolveDesiredReference();
				}
			}
			// else: stale build (mod switched mid-flight) -> discard; the kick below rebuilds.
		}
		StartCatalogBuildIfNeeded();
	}

	// Overload guard: refill the per-frame spawn budget. Hysteresis: once
	// overloaded, spawning stays suppressed until the population decays
	// below 90% of the cap, so the boundary doesn't flicker at the 4 Hz
	// stats rate. [guard-config] Skipped entirely when the guard is
	// disabled — the gates return early, so no refusal can be recorded
	// and the latch stays false (banner/amber never show).
	if (m_overloadGuardEnabled)
	{
		const int resumeAt = m_overloadActive
			? (m_maxPreviewParticles * 9) / 10 : m_maxPreviewParticles;
		m_spawnBudget = (m_numParticles < resumeAt)
			? m_maxPreviewParticles - m_numParticles : 0;
	}
	// NOTE: m_overloadThisFrame is deliberately NOT reset here — it is
	// reset at the END of Update, after the latch evaluation. Refusals
	// recorded BETWEEN frames (bridge/spawner-driven instance
	// construction) must count toward this frame's latch; a reset here
	// would erase them.

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

	// Latch with a clear-delay debounce: refusals only occur on frames
	// where a spawn round fires, so the raw per-frame flag flickers at
	// moderate rates; hold the latch until kOverloadClearDelaySec passes
	// with no refusal (see engine.h). [guard-config] Skipped when the
	// guard is disabled — no refusal can be recorded, and SetOverloadGuard
	// already dropped the latch, so leave m_overloadActive false.
	if (m_overloadGuardEnabled)
	{
		if (m_overloadThisFrame) m_lastOverloadTime = currentTime;
		const bool overloadNow = m_overloadThisFrame
			|| (m_overloadActive
			    && (currentTime - m_lastOverloadTime) < kOverloadClearDelaySec);
#ifndef NDEBUG
		// Overload guard: log only the latch TRANSITIONS — never per refusal
		// (refusals happen per-particle on a hot path).
		if (overloadNow != m_overloadActive)
		{
			printf("[overload] spawn suppression %s (particles=%d instances=%d)\n",
			       overloadNow ? "ON" : "OFF", m_numParticles, m_numEmitters);
			fflush(stdout);
		}
#endif
		m_overloadActive = overloadNow;
	}

	// Reset AFTER the latch evaluation so refusals between now and the
	// next Update (inter-frame spawns) accumulate into the next frame.
	// Unconditional: a refusal recorded just before the guard was disabled
	// must not leak into a later re-enable.
	m_overloadThisFrame = false;
}

bool Engine::RecoverDeviceIfNeeded()
{
	if (m_pDevice == NULL) return false;
	HRESULT hr = m_pDevice->TestCooperativeLevel();
	if (hr == D3D_OK) return true;
	if (hr == D3DERR_DEVICELOST) return false;
	if (hr == D3DERR_DEVICENOTRESET)
	{
		try { Reset(); }
		catch (...) { return false; }
		return m_pDevice->TestCooperativeLevel() == D3D_OK;
	}
	return false;
}

// [PERF] round-2 sub-profiling helpers — QPC microsecond deltas for the
// per-pass timing in Render(). Frequency is fixed for the process; cache it.
static LONGLONG EngQpcNow()
{
	LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}
static double EngQpcUs(LONGLONG a, LONGLONG b)
{
	static LONGLONG f = 0;
	if (f == 0) { LARGE_INTEGER q; if (QueryPerformanceFrequency(&q)) f = q.QuadPart; }
	return f ? static_cast<double>(b - a) * 1.0e6 / static_cast<double>(f) : 0.0;
}

bool Engine::Render()
{
	static const D3DXMATRIX Identity(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);

	// See if we can render. Mirrors RecoverDeviceIfNeeded but keeps the
	// switch here so DEVICELOST early-returns false (no point doing the
	// rest of Render if we can't yet); RecoverDeviceIfNeeded is the
	// "fix the latch, don't render" variant for non-render-thread callers.
	switch (m_pDevice->TestCooperativeLevel())
	{
		case D3DERR_DEVICELOST:
			return false;

		case D3DERR_DEVICENOTRESET:
			Reset();
			break;
	}

	// [runtime-MSAA] Apply a pending MSAA level change on the render thread.
	// The setter (SetMsaaLevel) stores the preference and raises this flag;
	// all D3D surface work happens here, where device ownership is known safe.
	if (m_msaaDirty) { ApplyMsaaLevelNow(); m_msaaDirty = false; }

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
	
	// [PERF] round-2 per-pass timing — scene segment starts here.
	const LONGLONG _ptScene0 = EngQpcNow();
	m_pDevice->BeginScene();

	// When the layered-window compositor is installed, swap slot
	// 0 from the swap-chain back buffer to the compositor's off-screen
	// ARGB RT. The pScreenSurface capture immediately below then picks
	// this RT up as the "screen" target for the full render chain
	// (scene → bloom → distort → final composite), and Composite() at
	// the bottom of Render pushes it via UpdateLayeredWindow.
	if (m_pAlphaCompositor && m_pAlphaCompositor->GetRenderTarget())
	{
		m_pDevice->SetRenderTarget(0, m_pAlphaCompositor->GetRenderTarget());
	}

	IDirect3DSurface9* pScreenSurface;
	IDirect3DSurface9* pDepthSurface;
	m_pDevice->GetRenderTarget(0, &pScreenSurface);
    m_pDevice->GetDepthStencilSurface(&pDepthSurface);

	// Render to the scene texture (or the MSAA RT when antialiasing is active).
	// pSceneSurface is kept alive until after the resolve so StretchRect has a
	// valid non-MS destination; it is released exactly once at the resolve block.
	IDirect3DSurface9* pSceneSurface;
	m_pSceneTexture->GetSurfaceLevel(0, &pSceneSurface);
	// RT + depth must share the same multisample type AT ALL TIMES. The
	// compositor's non-MSAA RT is bound just above, so we must unbind depth (NULL is
	// always legal) BEFORE swapping to the MSAA RT, then bind the matching MSAA depth.
	// Setting an MSAA depth while a non-MSAA RT is still bound fails the match check and
	// leaves the scene rendering nowhere -> black viewport.
	if (m_msaaActive)
	{
		m_pDevice->SetDepthStencilSurface(NULL);
		m_pDevice->SetRenderTarget(0, m_pMsaaColor);
		m_pDevice->SetDepthStencilSurface(m_pMsaaDepth);
	}
	else
	{
		m_pDevice->SetDepthStencilSurface(m_pDepthStencilSurface);
		m_pDevice->SetRenderTarget(0, pSceneSurface);
	}

    D3DCOLOR clearColor = D3DCOLOR_XRGB(GetRValue(m_background), GetGValue(m_background), GetBValue(m_background));
	m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, clearColor, 1.0f, 0);

	// Clear-then-SetViewport ordering rule.
	// The full-RT Clear above fills m_pSceneTexture with engine clear
	// color in its entirety. NOW narrow the viewport to the scene-rect
	// sub-region so scene draws only land inside it. The post-process
	// passes below restore the full-RT viewport before sampling.
	//
	// This ordering eliminates post-process bleed across the scene-rect
	// boundary: bloom's gaussian taps near the
	// inner scene-rect edge sample uniform engine clear color outside,
	// not stale pixels from last frame.
	//
	// Non-composition transports (canvas-jpeg) never call
	// SetSceneViewport, so m_sceneViewportActive stays false and this
	// block is a no-op for them — Render behaves byte-identical to
	// the full-RT path.
	D3DVIEWPORT9 prevViewportS5 = {};
	bool         restoreViewportS5 = false;
	if (m_sceneViewportActive)
	{
		m_pDevice->GetViewport(&prevViewportS5);
		D3DVIEWPORT9 vp = {};
		vp.X      = static_cast<DWORD>(m_sceneViewportX);
		vp.Y      = static_cast<DWORD>(m_sceneViewportY);
		vp.Width  = static_cast<DWORD>(m_sceneViewportW);
		vp.Height = static_cast<DWORD>(m_sceneViewportH);
		vp.MinZ   = 0.0f;
		vp.MaxZ   = 1.0f;
		m_pDevice->SetViewport(&vp);
		restoreViewportS5 = true;
	}

	// Optional skydome pass, after Clear, before ground.
	// RenderSkydomes() draws the real game .alo domes when one is selected,
	// else falls back to the simple-background sphere (slot != Off).
	RenderSkydomes();

	if (m_showGround)
	{
		// bump-mapped lit ground via the game's terrain shader when the
		// effect is ready; else the original unlit fixed-function quad.
		if (m_pGroundEffect != NULL && m_pGroundDecl != NULL)
		{
			RenderGroundLit();
		}
		else
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
	}

	// Unit grid: drawn after the ground, before the reference object, so a
	// placed object occludes the grid lines behind it. No-op when hidden.
	RenderUnitGrid();

	// Imported reference object: solid, depth-tested geometry drawn after
	// the ground and before the (depth-test-only) particles, so particles sort
	// against it and it sits on the ground plane. No-op when none is loaded.
	RenderReferenceObject();
	RenderReferenceShadows();

	// Selection box (depth-tested, part of the scene) when the object is
	// selected -- shows the clickable region. Drawn before the on-top gizmo.
	RenderReferenceSelectionBox();

	// Translate-manipulator axis handles over the placed object.
	RenderReferenceManipulator();

    // Particles never write to the depth buffer — let painter's-order
    // (the order each emitter is drawn) decide stacking when emitters
    // overlap, matching the in-game behaviour. ZENABLE is re-asserted here so
    // the depth test against the ground holds regardless of which ground path
    // ran above (the effect's Begin/End restores states to its pre-Begin
    // values, which may leave ZENABLE off).
    m_pDevice->SetRenderState(D3DRS_ZENABLE,      TRUE);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // Particle mip sampling (#481): draw the non-heat particle pass with
    // MIPFILTER = NONE on both texture stages. The Prim* particle shaders —
    // unlike every mesh shader, which declares MIPFILTER=LINEAR in its
    // sampler_state / FIXEDFUNCTION state block — declare NO sampler
    // filtering at all, and D3D9's device default mip filter is NONE. With
    // the editor's previous always-trilinear stages, a small on-screen
    // sprite sampled the game texture's own box-filtered deep mips, whose
    // alpha cutout is smeared away (P_ASTEROIDS mip 6 is a-0.4..0.75
    // everywhere) and whose normals average to ndotl~0 — rendering
    // bump-mode (blend 11) asteroids as near-black squares. Mip-0 sampling
    // restores the rock-shaped cutouts that match the observed in-game
    // look. NOTE this is the editor-side setting consistent with that
    // observed result, not a probed copy of the game's literal sampler
    // calls (instrumenting swfoc.exe is the open follow-up); the env
    // override below exists to A/B against the old behavior at runtime:
    //   ALO_PARTICLE_MIPFILTER=linear   -> pre-#481 trilinear
    //   ALO_PARTICLE_MIPFILTER=bias:<f> -> trilinear with LOD bias <f>
    // The bracket saves/restores all four touched sampler states so
    // nothing leaks into later passes regardless of mode.
    DWORD oldMip0 = 0, oldMip1 = 0, oldBias0 = 0, oldBias1 = 0;
    m_pDevice->GetSamplerState(0, D3DSAMP_MIPFILTER,     &oldMip0);
    m_pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER,     &oldMip1);
    m_pDevice->GetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, &oldBias0);
    m_pDevice->GetSamplerState(1, D3DSAMP_MIPMAPLODBIAS, &oldBias1);
    {
        static int s_read = 0;
        static ParticleMipFilterMode s_mip;
        if (!s_read)
        {
            s_read = 1;
            char b[32] = {0};
            GetEnvironmentVariableA("ALO_PARTICLE_MIPFILTER", b, sizeof(b));
            s_mip = ParseParticleMipFilter(b);
            if (!s_mip.recognized)
            {
                printf("[particle-mip] ALO_PARTICLE_MIPFILTER='%s' not recognized "
                       "(want 'linear' or 'bias:<f>') — using default mip-0 sampling\n", b);
                fflush(stdout);
            }
        }
        const DWORD mipFilter = (s_mip.mode == ParticleMipFilterMode::MODE_NONE)
                              ? D3DTEXF_NONE : D3DTEXF_LINEAR;
        m_pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, mipFilter);
        m_pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, mipFilter);
        if (s_mip.mode == ParticleMipFilterMode::MODE_BIAS)
        {
            DWORD bias; memcpy(&bias, &s_mip.bias, sizeof(bias));
            m_pDevice->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, bias);
            m_pDevice->SetSamplerState(1, D3DSAMP_MIPMAPLODBIAS, bias);
        }
    }

    for (auto& instance : m_instances)
    {
        instance->RenderNormal(m_pDevice);
	}
    m_pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER,     oldMip0);
    m_pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER,     oldMip1);
    m_pDevice->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, oldBias0);
    m_pDevice->SetSamplerState(1, D3DSAMP_MIPMAPLODBIAS, oldBias1);
    m_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);

	// Resolve the multisampled scene surface into the non-MS scene texture
	// so the post-process chain (bloom/distort/compose) and --capture readback —
	// which require a non-multisampled source — work unchanged.  StretchRect from
	// a MS surface to a same-size non-MS surface performs the hardware MSAA resolve.
	// After resolve, rebind slot-0 RT to pSceneSurface so the post-process passes
	// below write into the (non-MS) scene texture, converging both paths.
	if (m_msaaActive)
	{
		m_pDevice->StretchRect(m_pMsaaColor, NULL, pSceneSurface, NULL, D3DTEXF_NONE);
		// Same match rule on the way back: drop the MSAA depth before binding the
		// non-MS scene surface, then restore the non-MS depth (converges to the
		// non-MSAA path's post-scene RT/depth state for the post-process passes).
		m_pDevice->SetDepthStencilSurface(NULL);
		m_pDevice->SetRenderTarget(0, pSceneSurface);
		m_pDevice->SetDepthStencilSurface(m_pDepthStencilSurface);
	}
	SAFE_RELEASE(pSceneSurface);

	// restore full-RT viewport before the
	// bloom + distort post-process passes. They read+write at full-RT
	// resolution on m_pSceneTexture / m_pDistortTexture / m_pBloomTexture[];
	// keeping the scene-rect viewport active would clip their full-screen
	// quads. DComp's SetClip on the engine visual crops the off-scene-
	// rect region after compositing, so the wasted post-process work is
	// invisible (the "post-process at full-RT" trade-off).
	if (restoreViewportS5)
	{
		m_pDevice->SetViewport(&prevViewportS5);
	}

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
	// hardcoded constant.
	static const UINT BLOOM_BLUR_ITERATIONS = 4;
	const LONGLONG _ptScene1 = EngQpcNow();   // scene ends / bloom begins
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

	const LONGLONG _ptBloom1 = EngQpcNow();   // bloom ends / distort begins
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

	const LONGLONG _ptDistort1 = EngQpcNow();  // distort ends / composite begins
	// Now render to the screen
	m_pDevice->SetRenderTarget(0, pScreenSurface);
	// In alpha-compositor mode the slot-0 RT is our off-screen
	// D3DMULTISAMPLE_NONE surface. The auto-depth-stencil captured at
	// the top of Render is multisampled (matches the swap chain), so
	// restoring it here pairs an MS_NONE RT with an MSAA depth — D3D9
	// silently drops the next draw on that mismatch. Keep the engine's
	// own MS_NONE depth (m_pDepthStencilSurface) bound instead; the
	// legacy Present path still wants the auto-depth restored.
	if (!m_pAlphaCompositor)
	{
		m_pDevice->SetDepthStencilSurface(pDepthSurface);
	}
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
		// Mask alpha writes on the final scene blit so the eroded
		// scene-texture alpha doesn't overwrite the opaque alpha the screen
		// RT was just cleared to (the Clear above uses XRGB → A=0xFF). The
		// layered/DComp compositor treats this RT's per-pixel alpha as the
		// viewport's opacity; a game backbuffer ignores alpha. Particle
		// transparent/modulate blends erode the scene RT alpha, and SceneHeat.fx
		// copies it straight through, so without this mask the viewport composites
		// particles against the chrome behind it → washed-out / too-light. Forcing
		// the final alpha opaque presents the engine's RGB at full opacity, exactly
		// like the game → editor↔in-game parity, blend-mode-agnostic. No shader
		// edit (no-fork rule). Set AFTER BeginPass so the effect's pass-state apply
		// can't clobber it; restore full RGBA after the draw so later frames'
		// particle blends are unaffected.
		m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(EmitterInstance::Vertex));
		m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
		pEffect->EndPass();
	}
	pEffect->End();
    SAFE_RELEASE(pEffect);

	const LONGLONG _ptComposite1 = EngQpcNow();  // composite ends / present begins
	m_pDevice->EndScene();

	// When an AlphaCompositor is attached the engine renders into its
	// shared-handle RT and the host's DComp path
	// (host::Compositor::CompositeEngineFrame) presents those pixels GPU-side
	// — the engine itself does NOT present. Without a compositor (--capture /
	// headless / poc) we present the swap chain directly.
	if (!m_pAlphaCompositor)
	{
		m_pDevice->Present(NULL, NULL, NULL, NULL);
	}

	// [PERF] round-2 — store per-pass us for the host to fold into [PERF2].
	const LONGLONG _ptPresent1 = EngQpcNow();
	m_lastRenderTimings.scene     = EngQpcUs(_ptScene0,     _ptScene1);
	m_lastRenderTimings.bloom     = EngQpcUs(_ptScene1,     _ptBloom1);
	m_lastRenderTimings.distort   = EngQpcUs(_ptBloom1,     _ptDistort1);
	m_lastRenderTimings.composite = EngQpcUs(_ptDistort1,   _ptComposite1);
	m_lastRenderTimings.present   = EngQpcUs(_ptComposite1, _ptPresent1);
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

// Ground-texture bundled-resource lookup table. 0 = "no bundled
// resource". Kept in this .cpp (rather than the header) so the .rc IDs
// don't need to be visible to every includer of engine.h.
//
// Index 0 (dirt) is the editor's OWN default, bundled as RCDATA and
// always loadable. Grass/Sand/Snow (1-3) are vanilla EaW textures —
// NOT bundled (we must not ship proprietary game assets); they resolve
// from the user's EaW/FoC install at runtime via kGroundTextureGameLeaf
// below (see ReloadGroundTexture / IsGroundSlotAvailable). Slot 4 is the
// procedural solid colour; 5..7 are user-supplied custom paths only.
static const UINT kGroundTextureResourceIds[Engine::kGroundTextureCount] = {
    IDB_GROUND,         // 0 dirt (bundled editor default)
    0,                  // 1 grass — game-sourced (W_TEMPGRND00.DDS)
    0,                  // 2 sand  — game-sourced (W_SAND00.DDS)
    0,                  // 3 snow  — game-sourced (W_SNOW_RGH.DDS)
    0,                  // 4 solid color (procedural — see m_groundSolidColor)
    0, 0, 0,            // 5..7 — empty bundled, user-supplied only
};

// Base-colour leaf names resolved from the user's game install
// (DATA\ART\TEXTURES, with a loose-by-leaf fallback for mods that stash
// them flat) — the symmetric twin of the `_bc` normal-map leaves in
// ReloadGroundNormalTexture. NULL = the slot has no game source (dirt is
// bundled; solid/custom slots resolve by colour/path instead).
static const char* const kGroundTextureGameLeaf[Engine::kGroundTextureCount] = {
    NULL,                // 0 dirt — bundled editor default
    "W_TEMPGRND00.DDS",  // 1 grass
    "W_SAND00.DDS",      // 2 sand
    "W_SNOW_RGH.DDS",    // 3 snow
    NULL, NULL, NULL, NULL,
};

// Average colour of a ground-texture image, read from a 1×1 SCRATCH copy — the
// live texture is DEFAULT-pool (unlockable under D3D9Ex), so we re-decode a CPU
// copy just to read its mean. D3DX_FILTER_TRIANGLE (NOT _BOX, which only halves
// 2×2 for mipmaps) makes every source texel contribute equally, so the single
// 1×1 texel is the true whole-image average. One-time, only on a ground change.
// (The custom-file variant re-reads the file to average it — a tolerable cost on
// a one-time user action; bundled textures reuse their in-memory bytes.)
static COLORREF ReadScratch1x1Average(IDirect3DTexture9* pTex)
{
    COLORREF avg = RGB(128, 128, 128);
    if (pTex == NULL) return avg;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(pTex->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
    {
        DWORD argb = *(DWORD*)lr.pBits;   // D3DFMT_A8R8G8B8
        avg = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
        pTex->UnlockRect(0);
    }
    pTex->Release();
    return avg;
}
static COLORREF AverageColorFromMemory(IDirect3DDevice9* pDevice, const void* data, DWORD size)
{
    IDirect3DTexture9* pTex = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemoryEx(
            pDevice, data, size, 1, 1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SCRATCH, D3DX_FILTER_TRIANGLE, D3DX_FILTER_NONE, 0, NULL, NULL, &pTex)))
        return RGB(128, 128, 128);
    return ReadScratch1x1Average(pTex);
}
static COLORREF AverageColorFromFile(IDirect3DDevice9* pDevice, const std::wstring& path)
{
    IDirect3DTexture9* pTex = NULL;
    if (FAILED(D3DXCreateTextureFromFileExW(
            pDevice, path.c_str(), 1, 1, 1, 0, D3DFMT_A8R8G8B8,
            D3DPOOL_SCRATCH, D3DX_FILTER_TRIANGLE, D3DX_FILTER_NONE, 0, NULL, NULL, &pTex)))
        return RGB(128, 128, 128);
    return ReadScratch1x1Average(pTex);
}

// Internal: load a texture from a custom file path. Returns true and writes
// *ppOut on success; false leaves *ppOut untouched. pAvgOut (optional) receives
// the texture's average colour.
static bool LoadGroundTextureFromFile(IDirect3DDevice9*       pDevice,
                                       const std::wstring&     path,
                                       IDirect3DTexture9**     ppOut,
                                       COLORREF*               pAvgOut = NULL)
{
    if (pDevice == NULL || path.empty() || ppOut == NULL) return false;
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileW(pDevice, path.c_str(), &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut) *pAvgOut = AverageColorFromFile(pDevice, path);
    return true;
}

// Internal: load a bundled texture from the .exe's RCDATA resource.
// Returns true and writes *ppOut on success; false leaves *ppOut
// untouched. resourceId == 0 means "no bundled default" (e.g. an
// empty user-only slot) and is treated as failure.
static bool LoadGroundTextureFromResource(IDirect3DDevice9*    pDevice,
                                           UINT                 resourceId,
                                           IDirect3DTexture9**  ppOut,
                                           COLORREF*            pAvgOut = NULL)
{
    if (pDevice == NULL || resourceId == 0 || ppOut == NULL) return false;
    HMODULE  hMod  = GetModuleHandle(NULL);
    HRSRC    hRes  = FindResource(hMod, MAKEINTRESOURCE(resourceId), RT_RCDATA);
    HGLOBAL  hData = (hRes != NULL) ? LoadResource(hMod, hRes) : NULL;
    void*    pData = (hData != NULL) ? LockResource(hData)     : NULL;
    DWORD    dwSize = (hRes != NULL) ? SizeofResource(hMod, hRes) : 0;
    if (pData == NULL || dwSize == 0) return false;
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemory(pDevice, pData, dwSize, &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut) *pAvgOut = AverageColorFromMemory(pDevice, pData, dwSize);
    return true;
}

// Internal: load a game-sourced ground texture by leaf name from the user's
// EaW/FoC install via the FileManager (mod roots → base paths → MEG). The
// FileManager twin of LoadGroundTextureFromResource: it reads the bytes ONCE
// and both decodes the texture and averages them into *pAvgOut, so the
// game-sourced path keeps m_groundColor in parity with the bundled path (the
// average feeds the ambient-SPH lighting floor and the viewport pill backdrop).
// Tries DATA\ART\TEXTURES\<leaf> first, then bare <leaf> (mods that stash the
// texture flat). Returns false with *ppOut untouched on any miss/decode failure
// — the caller's graceful-degradation signal (no install ⇒ slot unavailable).
static bool LoadGroundTextureViaFileManager(IDirect3DDevice9*    pDevice,
                                             IFileManager&        fileManager,
                                             const char*          leaf,
                                             IDirect3DTexture9**  ppOut,
                                             COLORREF*            pAvgOut = NULL)
{
    if (pDevice == NULL || leaf == NULL || ppOut == NULL) return false;
    IFile* file = fileManager.getFile(std::string("DATA\\ART\\TEXTURES\\") + leaf);
    if (file == NULL) file = fileManager.getFile(leaf);   // mod may stash it flat
    if (file == NULL) return false;
    std::vector<unsigned char> bytes;
    try
    {
        bytes = ReadAndReleaseCapped(file, kMaxTextureAssetBytes);   // exact-byte read (size-capped); Releases the file ref
    }
    catch (...)
    {
        // Graceful-degradation boundary: ANY failure (ReadException, or a
        // std::bad_alloc from sizing the byte buffer to the archive entry)
        // means "slot unavailable" — never a crash. Broader than the sibling
        // texture loaders by design, because a ground-slot resolve miss must
        // fall back to dirt, not terminate.
        return false;
    }
    IDirect3DTexture9* pNew = NULL;
    if (FAILED(D3DXCreateTextureFromFileInMemory(pDevice, bytes.data(),
                                                 (unsigned long)bytes.size(), &pNew)))
        return false;
    *ppOut = pNew;
    if (pAvgOut)
        *pAvgOut = AverageColorFromMemory(pDevice, bytes.data(), (DWORD)bytes.size());
    return true;
}

// build a 1×1 procedural texture filled with the given COLORREF.
// Used by the "Solid Color" slot (kGroundSolidColorSlot). One-pixel
// tile is enough because the ground is sampled with WRAP wrap-mode —
// every texel across the entire ground reads back the same colour.
static bool CreateSolidColorTexture(IDirect3DDevice9*    pDevice,
                                     COLORREF             color,
                                     IDirect3DTexture9**  ppOut)
{
    if (pDevice == NULL || ppOut == NULL) return false;
    IDirect3DTexture9* pNew = NULL;
    // D3DPOOL_MANAGED → D3DPOOL_DEFAULT, because
    // D3D9Ex rejects the managed pool. But a DEFAULT-pool texture cannot
    // be LockRect'd unless it is ALSO created D3DUSAGE_DYNAMIC — without
    // it, LockRect returns D3DERR_INVALIDCALL, CreateSolidColorTexture
    // fails, and the solid-colour ground slot silently never applies (it
    // worked under the old MANAGED pool, which is lockable). Add the
    // dynamic usage so the 1×1 fill below is legal under D3D9Ex; the
    // texture is still recreated in Engine::Reset via ReloadGroundTexture
    // (DEFAULT/dynamic resources are lost on device reset).
    if (FAILED(pDevice->CreateTexture(1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &pNew, NULL)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(pNew->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        pNew->Release();
        return false;
    }
    DWORD argb = (DWORD)(0xFFu) << 24
               | (DWORD)GetRValue(color) << 16
               | (DWORD)GetGValue(color) <<  8
               | (DWORD)GetBValue(color);
    *(DWORD*)lr.pBits = argb;
    pNew->UnlockRect(0);
    *ppOut = pNew;
    return true;
}

bool Engine::ReloadGroundTexture()
{
    if (m_pDevice == NULL) return false;   // pre-init guard

    // Solid-color slot — procedural texture from m_groundSolidColor.
    if (m_groundTextureIndex == kGroundSolidColorSlot)
    {
        IDirect3DTexture9* pNew = NULL;
        if (!CreateSolidColorTexture(m_pDevice, m_groundSolidColor, &pNew))
            return false;
        SAFE_RELEASE(m_pGroundTexture);
        m_pGroundTexture = pNew;
        m_groundColor = m_groundSolidColor;   // solid slot: the colour IS the floor
#ifndef NDEBUG
        printf("[Ground] solid-color slot=%d color=#%02X%02X%02X\n",
               m_groundTextureIndex,
               GetRValue(m_groundSolidColor),
               GetGValue(m_groundSolidColor),
               GetBValue(m_groundSolidColor));
        fflush(stdout);
#endif
        return true;
    }

    // Resolution order: custom path → game-sourced leaf (grass/sand/snow,
    // resolved from the user's install) → bundled resource (dirt only) →
    // on all-failure, fall back to slot 0 (dirt, always loadable from RCDATA).
    IDirect3DTexture9* pNew = NULL;
    const std::wstring& path = m_groundSlotCustomPaths[m_groundTextureIndex];
    if (!path.empty())
    {
        if (!LoadGroundTextureFromFile(m_pDevice, path, &pNew, &m_groundColor))
        {
#ifndef NDEBUG
            printf("[Ground] custom path failed for slot=%d; trying game/bundled\n",
                   m_groundTextureIndex);
            fflush(stdout);
#endif
        }
    }
    if (pNew == NULL)
    {
        const char* leaf = kGroundTextureGameLeaf[m_groundTextureIndex];
        if (leaf != NULL &&
            !LoadGroundTextureViaFileManager(m_pDevice, m_fileManager, leaf, &pNew, &m_groundColor))
        {
#ifndef NDEBUG
            printf("[Ground] game texture '%s' not resolved for slot=%d; falling back\n",
                   leaf, m_groundTextureIndex);
            fflush(stdout);
#endif
            // Release-visible signal ONLY for the corrupt-install case: the slot
            // probed available (file present) yet failed to decode -- distinct from
            // "no install" (expected, silent). Without this, a user who picked Snow
            // and silently got dirt has no paper trail in a Release build.
            if (IsGroundSlotAvailable(m_groundTextureIndex))
            {
                char msg[256];
                _snprintf_s(msg, sizeof(msg), _TRUNCATE,
                    "[Ground] slot=%d game texture '%s' is present but failed to decode; "
                    "using dirt (file may be corrupt).\n", m_groundTextureIndex, leaf);
                OutputDebugStringA(msg);
            }
        }
    }
    if (pNew == NULL)
    {
        UINT bundledId = kGroundTextureResourceIds[m_groundTextureIndex];
        if (bundledId != 0)
            LoadGroundTextureFromResource(m_pDevice, bundledId, &pNew, &m_groundColor);
    }
    if (pNew == NULL)
    {
#ifndef NDEBUG
        printf("[Ground] slot=%d empty/failed; falling back to default\n",
               m_groundTextureIndex);
        fflush(stdout);
#endif
        if (m_groundTextureIndex != 0)
        {
            m_groundTextureIndex = 0;
            return ReloadGroundTexture();
        }
        return false;                       // dirt itself failed → engine is in trouble
    }
    // Release the prior texture only after the new one is in hand —
    // ensures we don't have a transient null window where a paint
    // could race against us.
    SAFE_RELEASE(m_pGroundTexture);
    m_pGroundTexture = pNew;
#ifndef NDEBUG
    const char* src = !m_groundSlotCustomPaths[m_groundTextureIndex].empty() ? "custom"
                    : (kGroundTextureGameLeaf[m_groundTextureIndex] != NULL)  ? "game"
                    :                                                           "bundled";
    printf("[Ground] texture set slot=%d source=%s\n", m_groundTextureIndex, src);
    fflush(stdout);
#endif
    return true;
}

bool Engine::SetGroundTexture(int index)
{
    if (index < 0 || index >= kGroundTextureCount) return false;
    // Refuse selection of an unavailable slot — empty user slot, OR a
    // game-sourced slot (grass/sand/snow) the install can't resolve. UI
    // greys these out; this is defence in depth against a stale persisted
    // selection or a programmatic call. (Availability, not emptiness: a
    // game slot reads "empty" structurally now that it has no bundled
    // resource, yet is selectable whenever the install provides it.)
    if (!IsGroundSlotAvailable(index)) return false;
    // Fast-path: already at this slot AND we have a valid texture.
    if (index == m_groundTextureIndex && m_pGroundTexture != NULL) return true;
    m_groundTextureIndex = index;
    bool ok = ReloadGroundTexture();
    ReloadGroundNormalTexture();   // re-resolve the slot's companion _bc map
    return ok;
}

bool Engine::SetGroundSlotCustomPath(int slot, const std::wstring& path)
{
    if (slot < 0 || slot >= kGroundTextureCount) return false;
    m_groundSlotCustomPaths[slot] = path;
    // If the mutated slot is currently selected, reload the engine's
    // ground texture so the preview reflects the change immediately.
    if (slot == m_groundTextureIndex)
    {
        // If the slot can no longer render (cleared user-supplied path on a
        // higher slot, or a game slot the install can't resolve), bounce the
        // selection back to dirt rather than leaving the engine pointing at
        // nothing. Availability-aware: clearing a custom path off a
        // game-sourced slot reverts to its install texture, not dirt.
        if (!IsGroundSlotAvailable(slot))
        {
            m_groundTextureIndex = 0;
        }
        bool ok = ReloadGroundTexture();
        ReloadGroundNormalTexture();   // re-resolve the slot's companion _bc map
        return ok;
    }
    return true;
}

const std::wstring& Engine::GetGroundSlotCustomPath(int slot) const
{
    static const std::wstring empty;
    if (slot < 0 || slot >= kGroundTextureCount) return empty;
    return m_groundSlotCustomPaths[slot];
}

bool Engine::IsGroundSlotEmpty(int slot) const
{
    if (slot < 0 || slot >= kGroundTextureCount) return true;
    if (slot == kGroundSolidColorSlot) return false;   // always populated procedurally
    if (!m_groundSlotCustomPaths[slot].empty()) return false;
    return kGroundTextureResourceIds[slot] == 0;
}

bool Engine::IsGroundSlotAvailable(int slot) const
{
    if (slot < 0 || slot >= kGroundTextureCount)  return false;
    if (slot == kGroundSolidColorSlot)            return true;    // procedural — always
    if (!m_groundSlotCustomPaths[slot].empty())   return true;    // user path (optimistic, as today)
    if (kGroundTextureResourceIds[slot] != 0)     return true;    // bundled (dirt)
    if (kGroundTextureGameLeaf[slot] != NULL)                     // game-sourced (grass/sand/snow)
    {
        EnsureGroundSlotResolvable();
        return m_groundSlotResolvable[slot];
    }
    return false;                                                 // empty user slot
}

void Engine::EnsureGroundSlotResolvable() const
{
    // Recompute only when the mod-layer set changed (see header note on why
    // GetContentRoots() is a complete invalidation key). Cheap getFile()+Release()
    // probe per game leaf — no decode.
    const std::vector<std::wstring>& roots = m_fileManager.GetContentRoots();
    if (m_groundSlotResolvableValid && roots == m_groundSlotResolvableRoots)
        return;
    for (int i = 0; i < kGroundTextureCount; ++i)
    {
        const char* leaf = kGroundTextureGameLeaf[i];
        bool resolvable = false;
        if (leaf != NULL)
        {
            // FileManager::getFile swallows its own IOExceptions and returns
            // NULL today, so this can't throw — but this probe runs inside a
            // const query reached from the snapshot builder, so a defensive
            // catch keeps a future getFile change from crashing the snapshot
            // path: the worst case is a slot reported (un)resolvable, never a
            // crash.
            try
            {
                IFile* f = m_fileManager.getFile(std::string("DATA\\ART\\TEXTURES\\") + leaf);
                if (f == NULL) f = m_fileManager.getFile(leaf);   // mod may stash it flat
                if (f != NULL) { f->Release(); resolvable = true; }
            }
            catch (...)
            {
                resolvable = false;
            }
        }
        m_groundSlotResolvable[i] = resolvable;
    }
    m_groundSlotResolvableRoots = roots;
    m_groundSlotResolvableValid = true;
#ifndef NDEBUG
    printf("[Ground] resolvable probe: grass=%d sand=%d snow=%d (mod-roots=%u)\n",
           m_groundSlotResolvable[1], m_groundSlotResolvable[2], m_groundSlotResolvable[3],
           (unsigned)roots.size());
    fflush(stdout);
#endif
}

bool Engine::SetGroundSolidColor(COLORREF color)
{
    m_groundSolidColor = color;
    // If the solid-colour slot is currently selected, regenerate the
    // texture so the colour change shows immediately.
    if (m_groundTextureIndex == kGroundSolidColorSlot)
        return ReloadGroundTexture();
    return true;
}
void Engine::SetBloom(bool enable)                  { m_bloomEnabled  = enable; }
void Engine::SetBloomStrength(float v)              { m_bloomStrength = v; }
void Engine::SetBloomCutoff(float v)                { m_bloomCutoff   = v; }
void Engine::SetBloomSize(float v)                  { m_bloomSize     = v; }

// [runtime-MSAA] Store the preferred sample count and mark the MSAA surfaces
// dirty for rebuild on the next render frame. Safe to call from any thread —
// the actual D3D work happens in ApplyMsaaLevelNow(), called from Render().
void Engine::SetMsaaLevel(int samples)
{
    if (samples != 0 && samples != 2 && samples != 4 && samples != 8) samples = 0;
    m_msaaPreferredLevel = samples;
    m_msaaDirty = true;
}

// [runtime-MSAA] Returns ascending list of supported sample counts: always
// includes 0 (Off), plus each of {2,4,8} for which BOTH the colour format
// (D3DFMT_A8R8G8B8) and the current depth-stencil format pass
// CheckDeviceMultiSampleType. Returns {0} if the D3D object is null.
std::vector<int> Engine::GetSupportedMsaaLevels() const
{
    std::vector<int> result;
    result.push_back(0);  // Off is always available
    if (!m_pDirect3D) return result;
    const D3DFORMAT depthFmt = m_presentationParameters.AutoDepthStencilFormat;
    const BOOL      windowed = m_presentationParameters.Windowed;
    for (int n : {2, 4, 8})
    {
        const D3DMULTISAMPLE_TYPE type = (D3DMULTISAMPLE_TYPE)n;
        DWORD colorQ = 0, depthQ = 0;
        if (SUCCEEDED(m_pDirect3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                D3DFMT_A8R8G8B8, windowed, type, &colorQ))
         && SUCCEEDED(m_pDirect3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                depthFmt, windowed, type, &depthQ)))
        {
            result.push_back(n);
        }
    }
    return result;
}

// [runtime-MSAA] Release existing MSAA surfaces and recreate at the highest
// supported level <= m_msaaPreferredLevel. Render-thread only — calls D3D.
// On any allocation failure both surfaces are released and m_msaaActive stays
// false (byte-identical to the pre-MSAA path).
void Engine::ApplyMsaaLevelNow()
{
    SAFE_RELEASE(m_pMsaaColor);
    SAFE_RELEASE(m_pMsaaDepth);
    m_msaaActive = false;
    m_currentMsaaLevel = 0;

    // Resolve the preference to the highest SUPPORTED level <= the request.
    int target = 0;
    if (m_msaaPreferredLevel > 0)
    {
        std::vector<int> sup = GetSupportedMsaaLevels();  // ascending, includes 0
        for (int lv : sup)
            if (lv > 0 && lv <= m_msaaPreferredLevel) target = lv;  // highest <= pref
    }
    if (target <= 0) return;  // Off (or nothing supported)

    const UINT w = m_presentationParameters.BackBufferWidth;
    const UINT h = m_presentationParameters.BackBufferHeight;
    if (w == 0 || h == 0) return;

    const D3DMULTISAMPLE_TYPE type     = (D3DMULTISAMPLE_TYPE)target;
    const D3DFORMAT           depthFmt = m_presentationParameters.AutoDepthStencilFormat;
    const bool colorOk = SUCCEEDED(m_pDevice->CreateRenderTarget(
        w, h, D3DFMT_A8R8G8B8, type, 0, FALSE /*lockable*/, &m_pMsaaColor, NULL));
    const bool depthOk = colorOk && SUCCEEDED(m_pDevice->CreateDepthStencilSurface(
        w, h, depthFmt, type, 0, TRUE /*discard*/, &m_pMsaaDepth, NULL));
    if (colorOk && depthOk)
    {
        m_msaaActive       = true;
        m_currentMsaaLevel = target;
    }
    else
    {
        // Partial failure — release whatever was allocated and stay on the non-MSAA path.
        SAFE_RELEASE(m_pMsaaColor);
        SAFE_RELEASE(m_pMsaaDepth);
        m_msaaActive       = false;
        m_currentMsaaLevel = 0;
    }
}

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

// scene-global shadow tint setter. The declaration has lived in
// engine.h since the original codebase shipped but never had a body —
// no shader effect handle currently consumes the value. We store it
// here so the API is no longer linker-dangling and the Lighting
// dialog's value round-trips correctly. When a future shader binds a
// SHADOW_COLOR semantic this will Just Work; until then it's a no-op
// visually.
void Engine::SetShadow(const D3DXVECTOR4& color)
{
	m_shadow = color;
}

const Engine::Light& Engine::GetLight(LightType which) const
{
	int index = 0;
	switch (which)
	{
		case LT_SUN:	index = 0; break;
		case LT_FILL1:	index = 1; break;
		case LT_FILL2:	index = 2; break;
	}
	return m_lights[index];
}

void Engine::Reset()
{
	// [resize-perf] sub-stage QPC brackets filled into
	// m_resetPerf at the end; the host logs them at 1 Hz. See engine.h.
	const LONGLONG _rpT0 = EngQpcNow();

	ReleaseBloomTargets();
	ReleaseShadowMaskTargets();   // [soft-shadows] DEFAULT-pool mask RTs
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
    SAFE_RELEASE(m_pDepthStencilSurface);
	// MSAA surfaces are D3DPOOL_DEFAULT — must be released before device Reset.
	SAFE_RELEASE(m_pMsaaColor);
	SAFE_RELEASE(m_pMsaaDepth);
	m_msaaActive = false;

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
	if (m_pShadowBlurEffect != NULL) m_pShadowBlurEffect->OnLostDevice();   // [soft-shadows] distinct ID3DXEffect
	// The skydome effect needs the same OnLost/OnReset dance — without it,
	// the effect's internal D3DPOOL_DEFAULT state-cache references survive
	// past Reset and cause D3DERR_INVALIDCALL on any later size change.
	// Surfaced as the ground-texture-stuck-at-0 bug in --test-host mode
	// after the polluter pair background-picker × spawner-import-mod;
	// interactive use never noticed because Render()'s recovery path
	// papered over the failed Reset on the next WM_PAINT. (Fixed
	// 2026-05-20.)
	if (m_pSkydomeEffect != NULL) m_pSkydomeEffect->OnLostDevice();
	// same OnLost dance for the ground effect; its normal textures are
	// D3DPOOL_DEFAULT under D3D9Ex (procedural flat normal + D3DX-loaded _bc
	// map) and must be released before Reset, recreated after (below).
	if (m_pGroundEffect != NULL) m_pGroundEffect->OnLostDevice();
	SAFE_RELEASE(m_pGroundNormalTexture);
	SAFE_RELEASE(m_pGroundFlatNormalTexture);
	// D3D9Ex disallows D3DPOOL_MANAGED, so
	// resources that were previously managed-pool (skydome VB/IB, the
	// solid-colour ground texture, and any custom skydome texture)
	// are now D3DPOOL_DEFAULT and must be released before Reset and
	// recreated after. Every newly-
	// D3DPOOL_DEFAULT resource that misses this dance produces a
	// stale-resource D3DERR on the next Reset.
	ReleaseSkydomeMeshBuffers();
	SAFE_RELEASE(m_pSkydomeTexture);
	// release the game-dome DEFAULT-pool VB/IB + material textures and
	// OnLostDevice their effects (decls survive). Both slots.
	m_skydomePrimaryMesh.OnLostDevice();
	m_skydomeSecondaryMesh.OnLostDevice();
	m_referenceObjectMesh.OnLostDevice();   // same DEFAULT-pool dance
	for (auto& a : m_referenceAttachments) if (a) a->mesh.OnLostDevice();   // hardpoint attach models
	SAFE_RELEASE(m_pGroundTexture);
	// The compositor's off-screen RT is D3DPOOL_DEFAULT, so
	// it must be released before m_pDevice->Reset — otherwise Reset
	// fails with D3DERR_INVALIDCALL and the engine is left in a
	// half-broken state (textures null, shaders OnLost'd but device
	// never reset). The Resize() call at the end of this function
	// recreates the RT against the new back-buffer size.
	if (m_pAlphaCompositor) m_pAlphaCompositor->ReleaseGpuResources();
	// D3DX texture helpers (D3DXCreateTextureFromFileInMemory,
	// D3DXCreateTextureFromResource) silently substitute D3DPOOL_DEFAULT
	// for D3DPOOL_MANAGED under D3D9Ex — the documented MANAGED default
	// inside the helper hits D3D9Ex's pool restriction and the helper
	// falls back to DEFAULT. TextureManager caches the result, so every
	// cached handle is a DEFAULT-pool resource that must be released
	// before Reset. An early mitigation (grep for the D3DPOOL_MANAGED literal) couldn't
	// find it because the helper hides the pool argument.
	m_textureManager.OnLostDevice();
	// release the event query before Reset.
	// IDirect3DQuery9 is not in any D3DPOOL_*, but D3D9Ex's device Reset
	// invalidates queries the same way it invalidates D3DPOOL_DEFAULT
	// resources. Lazy-recreated by the next IssueEndFrameQuery call
	// against the post-Reset device.
	SAFE_RELEASE(m_pEndFrameQuery);
	const LONGLONG _rpT1 = EngQpcNow();   // [resize-perf] lost ends / device Reset begins
	if (FAILED(m_pDevice->Reset(&m_presentationParameters)))
	{
		throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));
	}
	const LONGLONG _rpT2 = EngQpcNow();   // [resize-perf] device Reset ends / reload begins
	m_pDistortShader->OnResetDevice();
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        m_pShaders[i]->OnResetDevice();
    }
	if (m_pBloomEffect != NULL) m_pBloomEffect->OnResetDevice();
	if (m_pShadowBlurEffect != NULL) m_pShadowBlurEffect->OnResetDevice();   // [soft-shadows]
	if (m_pSkydomeEffect != NULL) m_pSkydomeEffect->OnResetDevice();
	if (m_pGroundEffect  != NULL) m_pGroundEffect->OnResetDevice();
	// two-phase like the rest of the dance: all effects OnReset here,
	// then the DEFAULT-pool VB/IB + textures refill below.
	m_skydomePrimaryMesh.OnResetEffects();
	m_skydomeSecondaryMesh.OnResetEffects();
	m_referenceObjectMesh.OnResetEffects();   // phase 1
	for (auto& a : m_referenceAttachments) if (a) a->mesh.OnResetEffects();   // attachments, phase 1
	// recreate the D3DPOOL_DEFAULT ground normal textures post-Reset.
	CreateGroundFlatNormal();
	// rebuild the previously-managed-pool
	// resources. CreateSkydomeMeshBuffers regenerates the procedural
	// VB/IB; ReloadGroundTexture re-runs the bundled-or-solid-colour
	// loader using m_groundTextureIndex; ReloadSkydomeTexture re-runs
	// the bundled-or-custom path using m_skydomeIndex.
	CreateSkydomeMeshBuffers();
	ReloadGroundTexture();
	ReloadGroundNormalTexture();   // re-resolve the companion _bc map
	ReloadSkydomeTexture(m_skydomeIndex);
	// phase 2: refill the game-dome DEFAULT-pool VB/IB + material
	// textures from the cached transcoded blobs (no re-parse).
	m_skydomePrimaryMesh.CreateBuffers(m_pDevice, m_fileManager);
	m_skydomeSecondaryMesh.CreateBuffers(m_pDevice, m_fileManager);
	m_referenceObjectMesh.CreateBuffers(m_pDevice, m_fileManager);   // phase 2
	for (auto& a : m_referenceAttachments) if (a) a->mesh.CreateBuffers(m_pDevice, m_fileManager);   // attachments, phase 2

	ResetParameters();

	const LONGLONG _rpT3 = EngQpcNow();   // [resize-perf] reload ends / alpha resize begins

	// The alpha compositor owns D3D9 resources (RT + sysmem
	// surface) sized to the popup client area. Refresh them so the
	// off-screen RT keeps pace with the swap-chain's back-buffer
	// size, which the engine's render chain (m_pSceneTexture etc.)
	// is already keyed off via BackBufferWidth/Height.
	if (m_pAlphaCompositor && m_presentationParameters.BackBufferWidth > 0
	    && m_presentationParameters.BackBufferHeight > 0)
	{
		m_pAlphaCompositor->Resize(
		    static_cast<int>(m_presentationParameters.BackBufferWidth),
		    static_cast<int>(m_presentationParameters.BackBufferHeight));
	}

	const LONGLONG _rpT4 = EngQpcNow();   // [resize-perf] alpha resize ends

	// re-apply the cached scene
	// viewport so its projection aspect ratio survives Reset.
	// ResetParameters() above rebuilt m_projection at FULL-RT aspect via
	// D3DXMatrixPerspectiveFovRH (engine.cpp:1448), overwriting whatever
	// scene-rect-aspect projection SetSceneViewport had set last. Without
	// this re-apply, the first frame after Reset would render at
	// full-RT aspect until React's next layout/scene-rect dispatch
	// catches up — visible as a one-frame aspect glitch at every window
	// resize. SetSceneViewport recomputes m_projection at scene-rect
	// aspect AND the Render hook's gating flag (m_sceneViewportActive)
	// stays set so the next frame uses the constrained viewport.
	//
	// We snapshot the cached state, flip the active flag false to defeat
	// the idempotent guard inside SetSceneViewport, then call back into
	// SetSceneViewport with the snapshot. Net: m_sceneViewportActive
	// re-armed, m_projection recomputed at scene-rect aspect, log line
	// emitted as if the scene-rect was freshly dispatched.
	if (m_sceneViewportActive)
	{
		int sx = m_sceneViewportX;
		int sy = m_sceneViewportY;
		int sw = m_sceneViewportW;
		int sh = m_sceneViewportH;
		m_sceneViewportActive = false;
		SetSceneViewport(sx, sy, sw, sh);
	}

	// [resize-perf] publish this Reset's sub-stage costs. count increments
	// only on a COMPLETED reset (the device-Reset throw above skips this),
	// so the host's delta-per-second reads as successful resets.
	m_resetPerf.lastLostMs        = EngQpcUs(_rpT0, _rpT1) / 1000.0;
	m_resetPerf.lastDeviceResetMs = EngQpcUs(_rpT1, _rpT2) / 1000.0;
	m_resetPerf.lastReloadMs      = EngQpcUs(_rpT2, _rpT3) / 1000.0;
	m_resetPerf.lastAlphaResizeMs = EngQpcUs(_rpT3, _rpT4) / 1000.0;
	m_resetPerf.lastTotalMs       = EngQpcUs(_rpT0, EngQpcNow()) / 1000.0;
	++m_resetPerf.count;
}

// [resize-perf] Cheap resize-only reset. See engine.h for the
// contract and the first-party ResetEx semantics this leans on. Mirrors
// Reset()'s structure minus everything ResetEx makes unnecessary: no
// OnLostDevice/OnResetDevice on shaders/effects, no skydome VB/IB release,
// no ground/skydome texture re-decode, no TextureManager cache wipe. The
// end-frame query is still released + lazily recreated — IDirect3DQuery9
// invalidation across device resets was observed empirically under plain
// Reset and a query re-create costs nothing next frame.
bool Engine::ResetForResize()
{
	if (m_pDevice == NULL) return false;

	const LONGLONG _rpT0 = EngQpcNow();

	// Release the size-keyed render targets so ResetParameters below can
	// recreate them at the new backbuffer size (it CreateTexture-s into
	// the member pointers without releasing first). NOT required by
	// ResetEx itself — DEFAULT-pool resources persist — purely lifetime
	// hygiene for the recreate.
	ReleaseBloomTargets();
	ReleaseShadowMaskTargets();   // [soft-shadows] DEFAULT-pool mask RTs
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
	SAFE_RELEASE(m_pDepthStencilSurface);
	// MSAA surfaces are D3DPOOL_DEFAULT — must be released before ResetEx.
	SAFE_RELEASE(m_pMsaaColor);
	SAFE_RELEASE(m_pMsaaDepth);
	m_msaaActive = false;
	SAFE_RELEASE(m_pEndFrameQuery);

	m_presentationParameters.BackBufferWidth  = 0;   // size to the HWND client
	m_presentationParameters.BackBufferHeight = 0;
	m_presentationParameters.BackBufferCount  = 1;
	m_presentationParameters.Windowed         = true;

	const LONGLONG _rpT1 = EngQpcNow();
	HRESULT hr = m_pDevice->ResetEx(&m_presentationParameters, NULL);
	if (FAILED(hr))
	{
		// Device is now in the lost state (ResetEx docs). Caller falls
		// back to the full Reset() / RecoverDeviceIfNeeded path.
		char buf[96];
		sprintf(buf, "[Engine] ResetForResize: ResetEx failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
		OutputDebugStringA(buf);
		return false;
	}
	const LONGLONG _rpT2 = EngQpcNow();

	// Rebuild the size-keyed targets + re-apply pipeline state and the
	// full-RT projection (same routine the full Reset uses). Throws on
	// allocation failure — propagate; the caller's fallback handles it.
	ResetParameters();
	const LONGLONG _rpT3 = EngQpcNow();

	// Same tail as Reset(): the AlphaCompositor's shared RT + readback
	// surfaces track the backbuffer size, and the cached scene viewport
	// must be re-applied so the projection survives at scene-rect aspect.
	if (m_pAlphaCompositor && m_presentationParameters.BackBufferWidth > 0
	    && m_presentationParameters.BackBufferHeight > 0)
	{
		m_pAlphaCompositor->Resize(
		    static_cast<int>(m_presentationParameters.BackBufferWidth),
		    static_cast<int>(m_presentationParameters.BackBufferHeight));
	}
	const LONGLONG _rpT4 = EngQpcNow();

	if (m_sceneViewportActive)
	{
		int sx = m_sceneViewportX;
		int sy = m_sceneViewportY;
		int sw = m_sceneViewportW;
		int sh = m_sceneViewportH;
		m_sceneViewportActive = false;
		SetSceneViewport(sx, sy, sw, sh);
	}

	m_resetPerf.lastLostMs        = EngQpcUs(_rpT0, _rpT1) / 1000.0;
	m_resetPerf.lastDeviceResetMs = EngQpcUs(_rpT1, _rpT2) / 1000.0;
	m_resetPerf.lastReloadMs      = EngQpcUs(_rpT2, _rpT3) / 1000.0;
	m_resetPerf.lastAlphaResizeMs = EngQpcUs(_rpT3, _rpT4) / 1000.0;
	m_resetPerf.lastTotalMs       = EngQpcUs(_rpT0, EngQpcNow()) / 1000.0;
	++m_resetPerf.count;
	++m_resetPerf.cheapCount;
	return true;
}

// forwarder to the AlphaCompositor's shared
// HANDLE. Returns nullptr when the compositor isn't installed (canvas-
// jpeg mode skips the layered-window path) or before Resize has run.
// The DComp visual tree will consume this via a D3D11 OpenSharedResource;
// today nothing reads it but the standalone
// shared_texture_test exe verifies the handle is openable.
HANDLE Engine::GetSharedTextureHandle() const
{
	return m_pAlphaCompositor ? m_pAlphaCompositor->GetSharedHandle() : nullptr;
}

// cross-device GPU sync. See engine.h for
// the design rationale (engine-exposed helpers,
// host orchestrates call sites under composition mode only).
//
// IssueEndFrameQuery lazily creates the IDirect3DQuery9 event query on
// first call (m_pDevice must already exist — Engine::Reset releases the
// query so subsequent first-call-after-Reset triggers recreation).
// Query-create failure logs once via OutputDebugString and leaves
// m_pEndFrameQuery null — subsequent Issue/Wait calls are no-ops.
// Issue's D3DISSUE_END markers the moment in the D3D9 command stream
// after the engine's current frame submissions; the D3D9 driver
// guarantees the query reports SIGNALED only after all preceding
// commands have completed.
void Engine::IssueEndFrameQuery()
{
	if (m_pDevice == NULL) return;
	if (m_pEndFrameQuery == NULL)
	{
		HRESULT hr = m_pDevice->CreateQuery(D3DQUERYTYPE_EVENT, &m_pEndFrameQuery);
		if (FAILED(hr) || m_pEndFrameQuery == NULL)
		{
			OutputDebugStringA("[Engine] CreateQuery(EVENT) failed; cross-device sync disabled this run\n");
			m_pEndFrameQuery = NULL;
			return;
		}
	}
	m_pEndFrameQuery->Issue(D3DISSUE_END);
}

// WaitEndFrameQuery spins on GetData with the spike's 100k cap (see
// dxgi_spike.cpp:687-697 for the original). On timeout, logs once and
// returns — degraded mode where the D3D11 CopyResource may read
// partially-finished VRAM (visible tearing). Safer than blocking the
// host message pump indefinitely on a hung GPU. Returns the spin count
// (0 = signalled on the first poll) so the host can log GPU-wait pressure.
//
// [resize-perf] The wait now YIELDS between polls past a short
// tight burst. The original no-yield spin burned a full core while the
// GPU drained (measured ~4000 spins/frame at the unpaced ~3000 fps idle
// — engine.cpp's share of the splitter-drag contention). The first 64
// polls stay tight for the common already-signalled / sub-µs case;
// after that each poll SwitchToThread()s, handing the rest of the
// timeslice to any ready thread (WebView2's renderer, the compositor)
// instead of re-polling a bit that hasn't flipped. Cap semantics
// unchanged: a hung GPU still exits after 100k polls (wall-clock longer
// now that late polls yield — irrelevant next to a hung GPU).
int Engine::WaitEndFrameQuery()
{
	if (m_pEndFrameQuery == NULL) return 0;
	BOOL done = FALSE;
	int spins = 0;
	while (m_pEndFrameQuery->GetData(&done, sizeof(done), D3DGETDATA_FLUSH) == S_FALSE)
	{
		if (++spins > 100000)
		{
			OutputDebugStringA("[Engine] D3D9 sync query never signalled after 100k spins\n");
			break;
		}
		if (spins > 64) SwitchToThread();
	}
	return spins;
}

// adapter LUID accessor for the multi-GPU
// guard. IDirect3D9Ex::GetAdapterLUID returns the LUID of the adapter
// associated with the supplied D3D9 adapter ordinal — the bridge
// between D3D9's adapter-index world and DXGI's LUID world. Compositor
// compares this against the LUID of the adapter its D3D11 device
// picked via D3D_DRIVER_TYPE_HARDWARE; if they differ, the two
// devices are on different physical GPUs and the shared-handle path
// is fundamentally broken (OpenSharedResource silently returns a
// wrong texture).
LUID Engine::GetAdapterLuid() const
{
	LUID luid = {};
	if (m_pDirect3D == NULL || m_pDevice == NULL) return luid;

	D3DDEVICE_CREATION_PARAMETERS params = {};
	if (FAILED(m_pDevice->GetCreationParameters(&params))) return luid;

	if (FAILED(m_pDirect3D->GetAdapterLUID(params.AdapterOrdinal, &luid)))
	{
		LUID zero = {};
		return zero;
	}
	return luid;
}

// scene-rect viewport.
//
// Stash the rect, mark active, and recompute m_projection at the
// scene-rect aspect ratio. Next Engine::Render's scene pass picks
// up m_sceneViewportActive == true and applies SetViewport after the
// full-RT Clear (the Clear-then-SetViewport ordering rule —
// prevents post-process bleed across the scene-rect
// boundary). Post-process passes restore the cached viewport before
// running.
//
// The projection-matrix shape mirrors ResetParameters at
// engine.cpp:1518: D3DXMatrixPerspectiveFovRH @ 45° FOV, near=1.0,
// far=1000, then the engine's _33 / _43 overrides that flip Z. The
// only thing that varies is the aspect: (w / h) here instead of
// (BackBufferWidth / BackBufferHeight) there. Duplicated inline
// (~5 lines) per CLAUDE.md "surgical changes" guidance rather than
// factoring a RebuildProjection helper that nothing else needs.
//
// Passing w <= 0 or h <= 0 clears the active flag and restores the
// full-RT-aspect projection. The Render hook reads m_sceneViewportActive
// each frame, so the cleared state effectively re-enables the
// default (full-RT) viewport without us needing to call SetViewport
// here.
//
// Logged to OutputDebugString + printf (live-debugging surface). The
// canonical Playwright-detectable signal is the Compositor's
// [COMP-engine-transform] line, which fires through host.log on the
// same LayoutBroker gate that fired this call.
void Engine::SetSceneViewport(int x, int y, int w, int h)
{
	const bool clearing = (w <= 0 || h <= 0);
	if (clearing)
	{
		if (!m_sceneViewportActive) return;   // already cleared / never set

		// Restore full-RT projection (matches ResetParameters' default).
		if (m_presentationParameters.BackBufferWidth > 0 &&
		    m_presentationParameters.BackBufferHeight > 0)
		{
			float n = 1.0f;
			D3DXMatrixPerspectiveFovRH(&m_projection, D3DXToRadian(45),
			    (float)m_presentationParameters.BackBufferWidth /
			    (float)m_presentationParameters.BackBufferHeight, n, 1000.0f);
			m_projection._33 = -1.0f;
			m_projection._43 = -2 * n;
			// Push to device + recompute m_viewProjection so shader
			// effects (engine.cpp:613, 616) see the fresh matrix. Without
			// this, the device keeps the stale projection until something
			// else calls SetCamera (visible as "aspect snaps on click").
			D3DXMatrixMultiply(&m_viewProjection, &m_view, &m_projection);
			if (m_pDevice)
			{
				m_pDevice->SetTransform(D3DTS_PROJECTION, &m_projection);
			}
		}

		m_sceneViewportX      = 0;
		m_sceneViewportY      = 0;
		m_sceneViewportW      = 0;
		m_sceneViewportH      = 0;
		m_sceneViewportActive = false;
		OutputDebugStringA("[engine] SetSceneViewport CLEARED (restored full-RT projection)\n");
		printf("[engine] SetSceneViewport CLEARED (restored full-RT projection)\n");
		fflush(stdout);
		return;
	}

	// [black-line fix, session 10] Defensive clamp to the engine RT. The
	// caller (LayoutBroker) guard-bands the scene viewport a few px beyond the
	// DComp clip so the D3D9Ex->D3D11 shared-surface edge incoherency lands
	// outside the clip. The surrounding chrome guarantees margin so the band
	// stays in-bounds in practice, but never let SetViewport fail on a
	// degenerate (collapsed-panel) layout.
	if (m_presentationParameters.BackBufferWidth > 0)
	{
		if (x < 0) { w += x; x = 0; }
		if (x + w > static_cast<int>(m_presentationParameters.BackBufferWidth))
			w = static_cast<int>(m_presentationParameters.BackBufferWidth) - x;
	}
	if (m_presentationParameters.BackBufferHeight > 0)
	{
		if (y < 0) { h += y; y = 0; }
		if (y + h > static_cast<int>(m_presentationParameters.BackBufferHeight))
			h = static_cast<int>(m_presentationParameters.BackBufferHeight) - y;
	}
	if (w <= 0 || h <= 0) return;  // fully clamped away — nothing to render

	// Idempotent — same rect, no-op (silent — 60+ Hz pane-drag
	// dispatches don't flood logs).
	if (m_sceneViewportActive &&
	    x == m_sceneViewportX && y == m_sceneViewportY &&
	    w == m_sceneViewportW && h == m_sceneViewportH)
	{
		return;
	}

	m_sceneViewportX      = x;
	m_sceneViewportY      = y;
	m_sceneViewportW      = w;
	m_sceneViewportH      = h;
	m_sceneViewportActive = true;

	// Per-pixel-FoV projection — reference is a FIXED anchor: 45° per
	// kFovAnchorHeightPx (768) of viewport height, so one pixel always
	// subtends the same angle REGARDLESS of window size. Combined with
	// aspect = W/H, 1 px ≡ 1 px angular extent in both axes: growing
	// the scene rect (pane drag, dock slide, AND a window resize)
	// reveals more world at the edges; shrinking crops. No zoom/FoV
	// rescale of existing content, ever.
	//
	// History: the reference used to be the CURRENT RT height
	// (BackBufferHeight), which kept the per-pixel angle constant only
	// while the WINDOW size was constant — a dock slide revealed, but a
	// window resize rescaled the world to the new height (user verdict
	// 2026-06-10: "adjusts the zoom as I resize … not desired; I like
	// how the dock slide just reveals more/less"). An absolute anchor
	// extends the reveal behaviour to window resizes. 768 ≈ the default
	// window's client height, so the default framing matches the old
	// scheme within ~1%; overall zoom is the camera's job (mouse wheel).
	//
	// fovY is clamped to 120° — at extreme viewport heights (~2050+ px)
	// the linear per-pixel widening would approach the projection
	// breakdown at 180°; past the clamp the view rescales instead
	// (accepted: wide-angle distortion is objectionable there anyway).
	float n      = 1.0f;
	const float kFovAnchorHeightPx = 768.0f;
	float fovY   = D3DXToRadian(45.0f) * (float)h / kFovAnchorHeightPx;
	const float kMaxFovY = D3DXToRadian(120.0f);
	if (fovY > kMaxFovY) fovY = kMaxFovY;
	m_sceneFovY = fovY;   // capture the CLAMPED FoV the projection uses (gizmo sizing)
	float aspect = (float)w / (float)h;
	D3DXMatrixPerspectiveFovRH(&m_projection, fovY, aspect, n, 1000.0f);
	m_projection._33 = -1.0f;
	m_projection._43 = -2 * n;
	// Push the new projection to the device + recompute m_viewProjection
	// for shader-effect consumers (engine.cpp:613, 616). Without these,
	// the device retains whatever projection SetCamera last pushed
	// (typically from boot) until SetCamera fires again — visible as
	// "aspect snaps to correct on click in viewport" because click
	// triggers a camera op which calls SetCamera and finally pushes
	// the latest m_projection.
	D3DXMatrixMultiply(&m_viewProjection, &m_view, &m_projection);
	if (m_pDevice)
	{
		m_pDevice->SetTransform(D3DTS_PROJECTION, &m_projection);
	}

	// [resize-perf] Throttled to 1 Hz: this used to print + fflush +
	// OutputDebugStringA on EVERY apply, and splitter drags apply at the
	// stream rate (~28/s) — ODS alone is ms-class with a debugger
	// attached. One line per second is plenty to see the live rect.
	static DWORD s_lastSvpLogTick = 0;
	const DWORD svpNow = GetTickCount();
	if (s_lastSvpLogTick == 0 || (svpNow - s_lastSvpLogTick) >= 1000)
	{
		s_lastSvpLogTick = svpNow;
		char buf[224];
		snprintf(buf, sizeof(buf),
		    "[engine] SetSceneViewport x=%d y=%d w=%d h=%d (fovY=%.2f° aspect=%.3f anchorH=%.0f)\n",
		    x, y, w, h, fovY * (180.0f / 3.14159265f), aspect, kFovAnchorHeightPx);
		OutputDebugStringA(buf);
		printf("%s", buf);
		fflush(stdout);
	}
}

bool Engine::GetSceneViewport(int& x, int& y, int& w, int& h) const
{
	if (!m_sceneViewportActive) return false;
	x = m_sceneViewportX;
	y = m_sceneViewportY;
	w = m_sceneViewportW;
	h = m_sceneViewportH;
	return true;
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

		// [runtime-MSAA] Recreate MSAA surfaces honoring m_msaaPreferredLevel
		// (default 4, set via SetMsaaLevel). The helper resolves the preference
		// to the highest SUPPORTED level <= the request, so default-4 behaves
		// identically to the old inline block on hardware that supports 4×.
		ApplyMsaaLevelNow();

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

		// [soft-shadows] Full-backbuffer shadow-mask RT (mirrors the bloom RTs).
		// When MSAA is active also allocate a matching-MSAA surface: the mask is
		// rendered there (the stencil test needs the multisampled depth-stencil)
		// then StretchRect-resolved into the non-MS m_pShadowMask the blur samples
		// — the same resolve trick as m_pMsaaColor. Any failure here just disables
		// soft shadows for this device cycle (hard fallback); never blocks render.
		ReleaseShadowMaskTargets();
		if (FAILED(m_pDevice->CreateTexture(bloomW, bloomH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_pShadowMask, NULL)))
		{
			ReleaseShadowMaskTargets();
		}
		else if (m_msaaActive && m_currentMsaaLevel > 0)
		{
			if (FAILED(m_pDevice->CreateRenderTarget(bloomW, bloomH, D3DFMT_A8R8G8B8,
			            (D3DMULTISAMPLE_TYPE)m_currentMsaaLevel, 0, FALSE /*lockable*/, &m_pShadowMaskMsaa, NULL)))
			{
				// Mask MSAA surface failed: drop only the MSAA surface. We could
				// still soft-shade on the non-MS path, but on an MSAA device the
				// blur would sample a never-written mask -> fall back fully.
				ReleaseShadowMaskTargets();
			}
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

// Build the UV sphere vertex declaration + mesh used by the skydome
// render pass. Called once from the Engine constructor after m_pDevice is
// created. The VB/IB allocation moved into
// CreateSkydomeMeshBuffers() so Engine::Reset can recreate them after
// the device Reset (D3DPOOL_DEFAULT resources don't survive Reset).
void Engine::InitSkydomeMesh()
{
    // Vertex declaration — not pool-bound, survives device Reset.
    D3DVERTEXELEMENT9 decl[] = {
        {0, offsetof(SkydomeVertex, Position),  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, offsetof(SkydomeVertex, Normal),    D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
        {0, offsetof(SkydomeVertex, TexCoord),  D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    if (FAILED(m_pDevice->CreateVertexDeclaration(decl, &m_pSkydomeDecl)))
        throw runtime_error("Unable to create skydome mesh");

    // VB + IB — pool-bound, must be recreated on device Reset.
    CreateSkydomeMeshBuffers();
}

// Allocate + fill the skydome VB and IB.
// Called from InitSkydomeMesh (engine init) and from Engine::Reset (after
// device Reset succeeds). D3DPOOL_DEFAULT means the buffers live in
// driver-managed VRAM that's lost on Reset; the procedural sphere data
// is cheap to regenerate (~256 vertices, ~1024 indices), so we just
// re-emit it every time rather than caching.
void Engine::CreateSkydomeMeshBuffers()
{
    const int lon = kSkydomeLongSegments;
    const int lat = kSkydomeLatSegments;
    const int vertCount = (lon + 1) * (lat + 1);
    const int triCount  = lon * lat * 2;
    m_skydomeIndexCount = triCount * 3;

    // Generate vertices: U wraps lon segments [0,1], V is lat segments [0,1].
    // Sphere radius is 1; the shader will push depth to the far plane.
    //
    // Axis convention: the engine is Z-up (m_eye.Up = (0,0,1)), so the
    // sphere's poles are placed on ±Z — top pole at +Z, bottom pole at
    // -Z, horizon ring on the XY plane. This matches how the game
    // renders its skydomes and means an equirectangular texture's top
    // edge (V=0) faces up and its bottom edge (V=1) faces down.
    std::vector<SkydomeVertex> verts(vertCount);
    for (int j = 0; j <= lat; ++j)
    {
        const float v     = float(j) / float(lat);
        const float theta = v * D3DX_PI;             // 0..pi (pole to pole)
        const float sinTheta = sinf(theta);
        const float cosTheta = cosf(theta);
        for (int i = 0; i <= lon; ++i)
        {
            const float u   = float(i) / float(lon);
            const float phi = u * 2.0f * D3DX_PI;   // 0..2pi
            const float sinPhi = sinf(phi);
            const float cosPhi = cosf(phi);
            SkydomeVertex& vx = verts[j * (lon + 1) + i];
            vx.Position = D3DXVECTOR3(sinTheta * cosPhi, sinTheta * sinPhi, cosTheta);
            vx.Normal   = vx.Position;
            vx.TexCoord = D3DXVECTOR2(u, v);
        }
    }

    std::vector<uint16_t> idx(m_skydomeIndexCount);
    int k = 0;
    for (int j = 0; j < lat; ++j)
    {
        for (int i = 0; i < lon; ++i)
        {
            uint16_t a = uint16_t(j * (lon + 1) + i);
            uint16_t b = a + 1;
            uint16_t c = uint16_t((j + 1) * (lon + 1) + i);
            uint16_t d = c + 1;
            idx[k++] = a; idx[k++] = c; idx[k++] = b;
            idx[k++] = b; idx[k++] = c; idx[k++] = d;
        }
    }

    // VB — D3DPOOL_DEFAULT for D3D9Ex compatibility.
    if (FAILED(m_pDevice->CreateVertexBuffer(
        UINT(verts.size() * sizeof(SkydomeVertex)),
        D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &m_pSkydomeVB, NULL)))
        throw runtime_error("Unable to create skydome mesh");
    void* pVB = NULL;
    if (FAILED(m_pSkydomeVB->Lock(0, 0, &pVB, 0)))
        throw runtime_error("Unable to create skydome mesh");
    memcpy(pVB, verts.data(), verts.size() * sizeof(SkydomeVertex));
    m_pSkydomeVB->Unlock();

    // IB — D3DPOOL_DEFAULT for D3D9Ex compatibility.
    if (FAILED(m_pDevice->CreateIndexBuffer(
        UINT(idx.size() * sizeof(uint16_t)),
        D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &m_pSkydomeIB, NULL)))
        throw runtime_error("Unable to create skydome mesh");
    void* pIB = NULL;
    if (FAILED(m_pSkydomeIB->Lock(0, 0, &pIB, 0)))
        throw runtime_error("Unable to create skydome mesh");
    memcpy(pIB, idx.data(), idx.size() * sizeof(uint16_t));
    m_pSkydomeIB->Unlock();

#ifndef NDEBUG
    fprintf(stdout, "[Skydome] sphere mesh init verts=%d tris=%d\n", vertCount, triCount);
#endif
}

// Release the skydome VB + IB ahead of
// m_pDevice->Reset. Counterpart of CreateSkydomeMeshBuffers. Symmetric
// with the existing OnLostDevice pattern used for shaders + compositor RT.
void Engine::ReleaseSkydomeMeshBuffers()
{
    SAFE_RELEASE(m_pSkydomeVB);
    SAFE_RELEASE(m_pSkydomeIB);
}

void Engine::InitSkydomeEffect()
{
    HMODULE hMod  = GetModuleHandle(NULL);
    HRSRC   hRes  = FindResource(hMod, MAKEINTRESOURCE(IDR_SHADER_SKYDOME), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hData  = LoadResource(hMod, hRes);
    DWORD   dwSize = SizeofResource(hMod, hRes);
    void*   pData  = hData ? LockResource(hData) : NULL;
    if (!pData || !dwSize) return;

    LPD3DXBUFFER pErrors = NULL;
    HRESULT hr = D3DXCreateEffect(m_pDevice, pData, dwSize, NULL, NULL, 0, NULL,
                                  &m_pSkydomeEffect, &pErrors);
    if (FAILED(hr))
    {
#ifndef NDEBUG
        if (pErrors) fprintf(stderr, "[Skydome] effect compile failed: %s\n",
                             (const char*)pErrors->GetBufferPointer());
#endif
        SAFE_RELEASE(pErrors);
        m_pSkydomeEffect = NULL;
        return;
    }
    SAFE_RELEASE(pErrors);

    m_hSkydomeWVP = m_pSkydomeEffect->GetParameterByName(NULL, "g_WorldViewProj");
    m_hSkydomeTex = m_pSkydomeEffect->GetParameterByName(NULL, "g_Skydome");
}

bool Engine::ReloadSkydomeTexture(int slot)
{
    SAFE_RELEASE(m_pSkydomeTexture);
    if (slot == kSkydomeOffSlot) return true;

    if (slot > kSkydomeOffSlot && slot < kSkydomeBundledCount)
    {
        // Try the curated in-archive path first so the
        // skydome picks up real game textures (and mod overlays on top of
        // them) wherever they exist. Fall back to the bundled RCDATA
        // placeholder so the slot still renders something when the base
        // game / mod doesn't ship the file.
        const char* gamePath = kSkydomeBundledGamePaths[slot];
        if (gamePath != NULL)
        {
            m_pSkydomeTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, gamePath);
            if (m_pSkydomeTexture != NULL) return true;
        }
        HMODULE hMod   = GetModuleHandle(NULL);
        HRSRC   hRes   = FindResource(hMod, MAKEINTRESOURCE(kSkydomeBundledResources[slot]), RT_RCDATA);
        if (!hRes) return false;
        HGLOBAL hData  = LoadResource(hMod, hRes);
        DWORD   dwSize = SizeofResource(hMod, hRes);
        void*   pData  = hData ? LockResource(hData) : NULL;
        if (!pData || !dwSize) return false;
        return SUCCEEDED(D3DXCreateTextureFromFileInMemory(m_pDevice, pData, dwSize, &m_pSkydomeTexture));
    }

    if (slot >= kSkydomeFirstCustomSlot && slot < kSkydomeSlotCount)
    {
        const std::wstring& path = m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot];
        if (path.empty()) return false;
        // Custom slots now route through FileManager first, so a path like
        // "DATA\\ART\\TEXTURES\\foo.dds" resolves from the mod / base-game
        // MEGs the same way the curated slots do. If FileManager can't
        // resolve it (e.g. the user pasted an absolute path to a loose file
        // outside the game roots), fall back to direct file I/O so legacy
        // absolute-path custom slots keep working.
        std::string narrowPath = WideToAnsi(path);
        m_pSkydomeTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, narrowPath);
        if (m_pSkydomeTexture != NULL) return true;
        // D3DPOOL_MANAGED → D3DPOOL_DEFAULT.
        // D3D9Ex disallows the managed pool. Custom-slot textures are
        // re-loaded from disk via Engine::Reset → ReloadSkydomeTexture
        // (called with m_skydomeIndex) when the device is reset.
        return SUCCEEDED(D3DXCreateTextureFromFileEx(
            m_pDevice, path.c_str(),
            D3DX_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, D3DFMT_UNKNOWN,
            D3DPOOL_DEFAULT, D3DX_DEFAULT, D3DX_DEFAULT, 0, NULL, NULL,
            &m_pSkydomeTexture));
    }
    return false;
}

void Engine::RenderSkydome()
{
    // World = Translation(camera.Position) — keeps the sphere camera-locked.
    D3DXMATRIX world, wvp;
    D3DXMatrixTranslation(&world, m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);
    wvp = world * m_view * m_projection;

    // Save render state so the skydome pass doesn't pollute the rest of the frame.
    DWORD oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,      &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,     &oldCull);
    // Save the vertex declaration too. It is NOT part of the ID3DXEffect
    // state block (Begin/End won't restore it), so the skydome's declaration
    // (SkydomeVertex — position/normal/texcoord, NO diffuse-colour element)
    // would otherwise leak into the ground + particle draws that follow. With
    // no colour stream, the fixed-function pipeline defaults every vertex's
    // diffuse to white (0xFFFFFFFF) — which blows out additive particles to
    // white and breaks the alpha-blended ones. The ground is unaffected (its
    // vertices are already white), which is exactly why the bug looked like a
    // skydome-only blend issue.
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,      D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,     D3DCULL_CCW); // we're inside the sphere; Y↔Z swap in InitSkydomeMesh reversed handedness so the inside-facing triangles are now CCW

    m_pSkydomeEffect->SetMatrix (m_hSkydomeWVP, &wvp);
    m_pSkydomeEffect->SetTexture(m_hSkydomeTex, m_pSkydomeTexture);

    UINT passes = 0;
    m_pSkydomeEffect->Begin(&passes, 0);
    for (UINT i = 0; i < passes; ++i)
    {
        m_pSkydomeEffect->BeginPass(i);
        m_pDevice->SetVertexDeclaration(m_pSkydomeDecl);
        m_pDevice->SetStreamSource(0, m_pSkydomeVB, 0, sizeof(SkydomeVertex));
        m_pDevice->SetIndices(m_pSkydomeIB);
        const UINT vertCount = (kSkydomeLongSegments + 1) * (kSkydomeLatSegments + 1);
        m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                        vertCount,
                                        0,
                                        m_skydomeIndexCount / 3);
        m_pSkydomeEffect->EndPass();
    }
    m_pSkydomeEffect->End();

    // Restore the vertex declaration the skydome bound, so the ground +
    // particle draws use the engine's diffuse-colour-carrying declaration
    // again (see the save above). GetVertexDeclaration AddRef'd it.
    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();

    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,      oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,     oldCull);
}

// The blend mode a dome sub-mesh's shader expects. The game .fxo set this
// inside a no-op'd SB block (ALAMO_STATE_BLOCKS 0), so the app
// applies it. MeshAdditive* -> ONE/ONE (space starfields); MeshAlpha* ->
// SRCALPHA/INVSRCALPHA (land ring/horizon overlays); everything else (Skydome,
// MeshGloss base) -> opaque.
enum SkydomeBlend { SKYBLEND_OPAQUE, SKYBLEND_ADDITIVE, SKYBLEND_ALPHA };

static SkydomeBlend SkydomeBlendFor(const std::string& shaderName)
{
    if (_strnicmp(shaderName.c_str(), "MeshAdditive", 12) == 0) return SKYBLEND_ADDITIVE;
    if (_strnicmp(shaderName.c_str(), "MeshAlpha",     9) == 0) return SKYBLEND_ALPHA;
    return SKYBLEND_OPAQUE;
}

// Apply a sub-mesh's authored material params (index-parallel handles) to the
// effect: each param -> its sampler/uniform via the .fx Texture=(X) link;
// params absent from this shader (NULL handle) are skipped. This loop was
// byte-identical in RenderSkydomeMesh and RenderReferenceObject (DRY audit
// cpp-engine-0) — extracted so the two stay in lockstep. ONLY the param loop is
// shared; the surrounding uniform-bind blocks legitimately diverge (the
// ref-object path binds object-space eye/light + a skinned bone palette) and
// stay inline at each call site.
static void ApplyAloMaterialParams(ID3DXEffect* fx,
                                   const std::vector<AloShaderParam>& params,
                                   const std::vector<D3DXHANDLE>& matHandles,
                                   const std::vector<IDirect3DTexture9*>& matTextures)
{
    for (size_t i = 0; i < params.size(); ++i)
    {
        D3DXHANDLE ph = (i < matHandles.size()) ? matHandles[i] : NULL;
        if (ph == NULL) continue;
        const AloShaderParam& p = params[i];
        switch (p.kind)
        {
            case AloShaderParam::INT:    fx->SetInt(ph, p.i); break;
            case AloShaderParam::FLOAT:  fx->SetFloat(ph, p.f[0]); break;
            case AloShaderParam::FLOAT3: fx->SetFloatArray(ph, p.f, 3); break;
            case AloShaderParam::FLOAT4:
            {
                D3DXVECTOR4 v(p.f[0], p.f[1], p.f[2], p.f[3]);
                fx->SetVector(ph, &v);
                break;
            }
            case AloShaderParam::TEXTURE:
                if (i < matTextures.size()) fx->SetTexture(ph, matTextures[i]);
                break;
        }
    }
}

// World matrix for a BILLBOARD sub-mesh (a 0x206 bone, e.g. the star dome's sun
// glow). The quad's bone places it off-centre; the game orients it to FACE THE
// CAMERA so a flat additive quad is never seen edge-on. We reproduce that: keep
// the bone's object-space position (through the dome world), but replace its
// orientation with a screen-facing basis so the quad's authored in-plane extent
// (local X/Z) maps to camera right/up. Without this the quad draws flat and
// collapses to a 1px additive-white line edge-on (the false "closure seam").
static D3DXMATRIX MakeSkydomeBillboardWorld(const D3DXVECTOR3& eyePos,
                                            const D3DXVECTOR3& eyeUp,
                                            const D3DXMATRIX&  placement,
                                            const D3DXMATRIX&  domeWorld)
{
    // Bone object-space position -> world (through the dome's scale+translate).
    D3DXVECTOR3 cObj(placement._41, placement._42, placement._43);
    D3DXVECTOR3 center; D3DXVec3TransformCoord(&center, &cObj, &domeWorld);

    // Uniform dome scale = length of domeWorld's first basis row (sf,sf,sf).
    const D3DXVECTOR3 row0(domeWorld._11, domeWorld._12, domeWorld._13);
    const float sf = D3DXVec3Length(&row0);

    // Spherical billboard basis facing the eye (point-at-eye, not view-plane: the
    // sun is a single off-centre point on a camera-locked sphere). The quad lies in
    // its bone-local X/Z plane (local Y is the normal), so map X->right, Z->up,
    // Y->normal. NOTE: deliberately NOT the engine's view-aligned m_billboard
    // (inverse view-rotation) -- that screen-aligns a center-anchored quad; this
    // off-centre point needs to point AT the eye.
    D3DXVECTOR3 normal = eyePos - center;
    if (D3DXVec3Length(&normal) < 1e-4f) normal = D3DXVECTOR3(0, 1, 0);   // sun at the eye (bone @ origin)
    D3DXVec3Normalize(&normal, &normal);
    // right = up x normal. If eyeUp is (near-)parallel to normal (camera ~directly
    // over/under the sun) OR degenerate (zero), the cross collapses -> fall back to
    // a world axis guaranteed non-parallel to normal. Detect via the cross MAGNITUDE
    // (|a x b| = sin θ), which catches both cases a dot-threshold up-guard would miss.
    D3DXVECTOR3 right;
    D3DXVec3Cross(&right, &eyeUp, &normal);
    if (D3DXVec3Length(&right) < 1e-4f) D3DXVec3Cross(&right, &D3DXVECTOR3(1, 0, 0), &normal);
    if (D3DXVec3Length(&right) < 1e-4f) D3DXVec3Cross(&right, &D3DXVECTOR3(0, 1, 0), &normal);
    D3DXVec3Normalize(&right, &right);
    D3DXVECTOR3 up; D3DXVec3Cross(&up, &normal, &right); D3DXVec3Normalize(&up, &up);

    D3DXMATRIX w; D3DXMatrixIdentity(&w);
    w._11 = right.x  * sf; w._12 = right.y  * sf; w._13 = right.z  * sf;   // local X -> right
    w._21 = normal.x * sf; w._22 = normal.y * sf; w._23 = normal.z * sf;   // local Y -> normal (verts Y==0)
    w._31 = up.x     * sf; w._32 = up.y     * sf; w._33 = up.z     * sf;   // local Z -> up
    w._41 = center.x;      w._42 = center.y;      w._43 = center.z;        // sun world position
    return w;
}

// Draw one decoded .alo dome: each sub-mesh runs its OWN named game
// shader 1:1 (Skydome.fx / MeshGloss.fxo / MeshAdditive.fx). Mirrors the
// particle per-frame binding template (engine.cpp:746) but binds the REAL world
// matrix (Skydome.fx computes world_pos/world_normal for SH) instead of the
// identity the particle path uses. Saves + restores the full render-state delta
// any sub-mesh may touch so the dome can't leak blend/zwrite/cull/decl into the
// ground + particle draws that follow.
void Engine::RenderSkydomeMesh(SkydomeMesh& mesh, const D3DXMATRIX& world)
{
    // World/WVP are now per-sub-mesh (each carries its bone placement; a billboard
    // sub-mesh gets a camera-facing world), so they are computed inside the loop.
    DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend, oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    m_pDevice->GetRenderState(D3DRS_SRCBLEND,         &oldSrcBlend);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,        &oldDestBlend);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    // Draw in two phases matching the game's Opaque-then-Transparent order: opaque
    // sub-meshes first (fill the background), then additive/alpha layers blended on
    // top -- so layering is correct regardless of the .alo's sub-mesh order.
    for (int phase = 0; phase < 2; ++phase)
    {
      const bool opaquePhase = (phase == 0);
      for (SubMeshGpu& sub : mesh.SubMeshes())
      {
        if (sub.effect == NULL || sub.vb == NULL || sub.ib == NULL || sub.decl == NULL)
            continue;
        const SkydomeBlend blend = SkydomeBlendFor(sub.shaderName);
        if ((blend == SKYBLEND_OPAQUE) != opaquePhase)
            continue;   // opaque sub-meshes in phase 0, blended in phase 1

        // Render state the shader expects (the game .fxo may NOT set it itself --
        // ALAMO_STATE_BLOCKS -- so we set it regardless). Every dome
        // layer is a camera-centred background: depth test+write OFF (drawn first,
        // ground/particles paint over it) and CULL_NONE (one triangle per view ray
        // -> no overdraw, sidesteps the unknown .alo winding). Blend per shader
        // intent: additive (MeshAdditive) adds, alpha (MeshAlpha) src-over, else opaque.
        m_pDevice->SetRenderState(D3DRS_ZENABLE,      D3DZB_FALSE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        m_pDevice->SetRenderState(D3DRS_CULLMODE,     D3DCULL_NONE);
        switch (blend)
        {
            case SKYBLEND_ADDITIVE:
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
                break;
            case SKYBLEND_ALPHA:
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
                break;
            default: // SKYBLEND_OPAQUE (Skydome, MeshGloss base)
                m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
                break;
        }

        ID3DXEffect* fx = sub.effect->getD3DEffect();   // AddRef'd
        const Effect::Handles& h = sub.effect->getHandles();

        // Per-sub-mesh world. A billboard bone (the star dome's sun glow) is placed
        // at its bone position and RE-ORIENTED to face the camera, so its flat
        // additive quad never collapses to an edge-on 1px line. Every other sub-mesh
        // keeps the dome world verbatim -- byte-identical to the prior behaviour, so
        // no existing/mod dome render changes (the dome meshes are authored in object
        // space on near-identity bones; only the sun rides a 0x206 billboard bone).
        D3DXMATRIX subWorld = (sub.billboardMode != 0)
            ? MakeSkydomeBillboardWorld(m_eye.Position, m_eye.Up, sub.placement, world)
            : world;
        D3DXMATRIX subWvp = subWorld * m_view * m_projection;

        // Engine semantics: the verbatim particle binding template (engine.cpp:746)
        // but with the REAL world matrix -- the dome shaders compute world_pos /
        // world_normal for SH diffuse + (MeshGloss) specular -- not the identity the
        // particle path uses. Handles a shader doesn't declare are NULL -> no-op.
        D3DXVECTOR4 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z, 1.0f);
        fx->SetMatrix(h.hWorld,               &subWorld);
        fx->SetMatrix(h.hWorldViewProjection, &subWvp);
        fx->SetVector(h.hEyePosition,         &eyePos);
        fx->SetVector(h.hGlobalAmbient,       &m_ambient);
        fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
        fx->SetVector(h.hDirLightObjVec0,     &m_lights[0].Position);
        fx->SetVector(h.hDirLightDiffuse,     &m_lights[0].Diffuse);
        fx->SetVector(h.hDirLightSpecular,    &m_lights[0].Specular);
        fx->SetMatrixArray(h.hSphLightAll,    m_sphLightAll,  3);
        fx->SetMatrixArray(h.hSphLightFill,   m_sphLightFill, 3);
        fx->SetFloat(h.hTime,                 GetTimeF());

        // Authored material params (index-parallel handles) -> samplers via the
        // .fx Texture=(X) link. Skips params absent from this shader (NULL handle).
        ApplyAloMaterialParams(fx, sub.params, sub.matHandles, sub.matTextures);

        m_pDevice->SetVertexDeclaration(sub.decl);
        m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
        m_pDevice->SetIndices(sub.ib);

        UINT passes = 0;
        fx->Begin(&passes, 0);
        for (UINT pass = 0; pass < passes; ++pass)
        {
            fx->BeginPass(pass);
#ifndef NDEBUG
            {
                DWORD zw = 0, ab = 0, cm = 0;
                m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &zw);
                m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
                m_pDevice->GetRenderState(D3DRS_CULLMODE,         &cm);
                fprintf(stderr, "[SkyDraw] %s pass %u/%u zwrite=%lu ablend=%lu cull=%lu prims=%u\n",
                        sub.shaderName.c_str(), pass + 1, passes, zw, ab, cm, sub.primitiveCount);
            }
#endif
            m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                            sub.vertexCount, 0, sub.primitiveCount);
            fx->EndPass();
        }
        fx->End();
        fx->Release();
      }
    }

    // Restore the saved delta. Stream-source / indices are intentionally NOT
    // restored: every subsequent draw rebinds them.
    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,         oldSrcBlend);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,        oldDestBlend);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Compose entry, replacing the single RenderSkydome() call site. Draws
// the game domes (secondary behind, then primary) when a real dome is selected;
// otherwise falls back to the simple-background sphere (bundled / custom / solid).
void Engine::RenderSkydomes()
{
    const bool primaryReady   = !m_skydomePrimaryMesh.IsEmpty()   && m_skydomePrimaryMesh.HasResolved();
    const bool secondaryReady = !m_skydomeSecondaryMesh.IsEmpty() && m_skydomeSecondaryMesh.HasResolved();

    if (primaryReady || secondaryReady)
    {
        // Layering: depth is off, so the layer drawn LATER paints on top of the
        // earlier. In BOTH contexts the PRIMARY is the opaque/full-dome background
        // and is drawn FIRST; the SECONDARY composites ON TOP:
        //  Space: primary = opaque star sphere (the game's Sort_Order_Adjust=-1 back
        //         layer); secondary = ADDITIVE nebula (MeshAdditiveVColor, ONE/ONE) --
        //         it MUST add after the opaque base or the base overwrites it (the bug
        //         this fixes: secondary-first buried the nebula under the star sphere).
        //  Land:  primary = full sky-dome; secondary = partial overlay (rings/horizon).
        const bool primaryFirst = true;
        SkydomeMesh* order[2] = {
            primaryFirst ? &m_skydomePrimaryMesh   : &m_skydomeSecondaryMesh,
            primaryFirst ? &m_skydomeSecondaryMesh : &m_skydomePrimaryMesh
        };
        const bool ready[2] = {
            primaryFirst ? primaryReady   : secondaryReady,
            primaryFirst ? secondaryReady : primaryReady
        };

        D3DXMATRIX t;
        D3DXMatrixTranslation(&t, m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);
        for (int i = 0; i < 2; ++i)
        {
            if (!ready[i]) continue;
            const float sf = order[i]->ScaleFactor();
            D3DXMATRIX s, world;
            D3DXMatrixScaling(&s, sf, sf, sf);
            world = s * t;
            RenderSkydomeMesh(*order[i], world);
        }
        return;
    }

    if (m_skydomeIndex != kSkydomeOffSlot && m_pSkydomeTexture != NULL && m_pSkydomeEffect != NULL)
        RenderSkydome();
}

// Live reference-object world = rotation then translation. The engine is
// Z-UP (m_eye.Up = (0,0,1); see the Z-up note ~engine.cpp:2213), so "yaw"
// (heading -- turning while staying upright) is rotation about world Z, NOT the
// Y axis D3DXMatrixRotationYawPitchRoll would use. Build the Z-up analogue
// explicitly: yaw->Z, pitch->X, roll->Y, with yaw applied LAST (outermost) so it
// turns the already-tilted object about world up. Wire convention is
// [yaw,pitch,roll] in degrees (schema + BridgeDispatcher). Shared by the render,
// the selection box, and the pick so all three agree on placement.
D3DXMATRIX Engine::ReferenceObjectWorldFrom(const D3DXVECTOR3& pos, const D3DXVECTOR3& rotDeg) const
{
    // Delegate to the pure header so the scale/rotation math is unit-tested
    // headlessly (tests/test_reference_world.cpp). m_referenceScaleFactor (the
    // per-object <Scale_Factor>) is applied LEFTMOST = first, about the object origin,
    // and rides through render / pick / selection-box / hardpoint mounts unchanged.
    return ReferenceObjectWorldMatrix(pos, rotDeg, m_referenceScaleFactor);
}

// Committed transform -> the PICK uses this (the exact, snapped value).
D3DXMATRIX Engine::ReferenceObjectWorld() const
{ return ReferenceObjectWorldFrom(m_referencePosition, m_referenceRotation); }

// Eased "display" transform -> the RENDER uses this (smooth motion). 
D3DXMATRIX Engine::ReferenceObjectDisplayWorld() const
{ return ReferenceObjectWorldFrom(m_displayPosition, m_displayRotation); }

// Ease the render-only display transform toward the committed one once per
// frame (exponential smoothing off WallTimeF, so it stays smooth even when the
// particle preview is paused). Snaps on a discontinuity larger than any plausible
// drag/undo step (e.g. a file load that teleports the transform) -- scene-scale aware
// via the screen-uniform gizmo length. Rotation eases each Euler component along the
// shortest angular path so a 359 deg -> 1 deg change doesn't spin the long way.
void Engine::EaseReferenceDisplay()
{
    // QPC (microsecond) clock, NOT GetTickCount/WallTimeF (~15.6 ms) -- on a
    // high-refresh display the frame interval is below GetTickCount's resolution,
    // so consecutive frames would read dt==0 and collapse the ease to an instant snap.
    const long long nowQpc = EngQpcNow();
    float dt = (m_displayLastQpc == 0) ? 0.0f : (float)(EngQpcUs(m_displayLastQpc, nowQpc) * 1.0e-6);
    m_displayLastQpc = nowQpc;
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    constexpr float kEaseTau = 0.09f;   // time constant (s); ~95% caught up in ~3*tau
    const float k = (dt <= 0.0f) ? 0.0f : (1.0f - expf(-dt / kEaseTau));

    D3DXVECTOR3 dp = m_referencePosition - m_displayPosition;
    const float snapGap = 8.0f * ReferenceGizmoHandleLength();
    if (k <= 0.0f || D3DXVec3Length(&dp) > snapGap) {        // first frame / paused / teleport -> snap
        m_displayPosition = m_referencePosition;
        m_displayRotation = m_referenceRotation;
        return;
    }
    m_displayPosition += dp * k;
    auto easeAngle = [k](float disp, float target) -> float {
        float d = target - disp;
        while (d >  180.0f) d -= 360.0f;
        while (d < -180.0f) d += 360.0f;
        return disp + d * k;
    };
    m_displayRotation.x = easeAngle(m_displayRotation.x, m_referenceRotation.x);
    m_displayRotation.y = easeAngle(m_displayRotation.y, m_referenceRotation.y);
    m_displayRotation.z = easeAngle(m_displayRotation.z, m_referenceRotation.z);
}

// [shadow-leak hunt] Env-gated full device-state snapshot at the particle draw.
// Writes to the file named by ALO_DUMP_RSTATE (append), throttled to ~every 30th
// frame. No-op when the env var is unset, so it costs nothing in normal use and is
// Release-safe (gated by env, not NDEBUG). Pointer VALUES (rt/ds/vs/ps/tex) are
// printed as %p so a diff can show WHICH resource is bound across frames. The goal:
// snapshot a fresh-clean frame vs a shadow-enable-then-disable frame and diff to
// find the render state Engine::RenderReferenceShadows leaks and never restores.
void Engine::DumpParticleDrawStateIfRequested(unsigned long blendMode,
                                              IDirect3DTexture9* colorTex,
                                              IDirect3DTexture9* normalTex)
{
    char path[512];
    if (GetEnvironmentVariableA("ALO_DUMP_RSTATE", path, sizeof(path)) == 0)
        return;

    static int s_frame = 0;
    if ((s_frame++ % 30) != 0)
        return;

    if (m_pDevice == NULL)
        return;

    FILE* f = fopen(path, "a");
    if (!f)
        return;

    fprintf(f, "=== frame=%d shadows=%d blendMode=%lu ===\n",
            s_frame, m_modelShadowsEnabled ? 1 : 0, blendMode);

    // --- Render states (broad coverage; one fprintf per state) ---
    {
        struct RSEntry { D3DRENDERSTATETYPE rs; const char* name; };
        static const RSEntry kStates[] = {
            { D3DRS_COLORWRITEENABLE,         "COLORWRITEENABLE" },
            { D3DRS_COLORWRITEENABLE1,        "COLORWRITEENABLE1" },
            { D3DRS_COLORWRITEENABLE2,        "COLORWRITEENABLE2" },
            { D3DRS_COLORWRITEENABLE3,        "COLORWRITEENABLE3" },
            { D3DRS_ALPHABLENDENABLE,         "ALPHABLENDENABLE" },
            { D3DRS_SRCBLEND,                 "SRCBLEND" },
            { D3DRS_DESTBLEND,                "DESTBLEND" },
            { D3DRS_BLENDOP,                  "BLENDOP" },
            { D3DRS_SEPARATEALPHABLENDENABLE, "SEPARATEALPHABLENDENABLE" },
            { D3DRS_SRCBLENDALPHA,            "SRCBLENDALPHA" },
            { D3DRS_DESTBLENDALPHA,           "DESTBLENDALPHA" },
            { D3DRS_BLENDOPALPHA,             "BLENDOPALPHA" },
            { D3DRS_ALPHATESTENABLE,          "ALPHATESTENABLE" },
            { D3DRS_ALPHAREF,                 "ALPHAREF" },
            { D3DRS_ALPHAFUNC,                "ALPHAFUNC" },
            { D3DRS_ZENABLE,                  "ZENABLE" },
            { D3DRS_ZWRITEENABLE,             "ZWRITEENABLE" },
            { D3DRS_ZFUNC,                    "ZFUNC" },
            { D3DRS_STENCILENABLE,            "STENCILENABLE" },
            { D3DRS_TWOSIDEDSTENCILMODE,      "TWOSIDEDSTENCILMODE" },
            { D3DRS_STENCILREF,               "STENCILREF" },
            { D3DRS_STENCILMASK,              "STENCILMASK" },
            { D3DRS_STENCILWRITEMASK,         "STENCILWRITEMASK" },
            { D3DRS_STENCILFUNC,              "STENCILFUNC" },
            { D3DRS_STENCILPASS,              "STENCILPASS" },
            { D3DRS_STENCILFAIL,              "STENCILFAIL" },
            { D3DRS_STENCILZFAIL,             "STENCILZFAIL" },
            { D3DRS_CULLMODE,                 "CULLMODE" },
            { D3DRS_LIGHTING,                 "LIGHTING" },
            { D3DRS_FOGENABLE,                "FOGENABLE" },
            { D3DRS_COLORVERTEX,              "COLORVERTEX" },
            { D3DRS_DIFFUSEMATERIALSOURCE,    "DIFFUSEMATERIALSOURCE" },
            { D3DRS_SHADEMODE,                "SHADEMODE" },
            { D3DRS_FILLMODE,                 "FILLMODE" },
            { D3DRS_TEXTUREFACTOR,            "TEXTUREFACTOR" },
            { D3DRS_MULTISAMPLEANTIALIAS,     "MULTISAMPLEANTIALIAS" },
            { D3DRS_SCISSORTESTENABLE,        "SCISSORTESTENABLE" },
            { D3DRS_CLIPPLANEENABLE,          "CLIPPLANEENABLE" },
            { D3DRS_SRGBWRITEENABLE,          "SRGBWRITEENABLE" },
            // [texture-content hunt] previously-omitted states — a leak from the
            // shadow pass might live in fixed-function lighting / fog / depth-bias
            // that the original table didn't capture. FOGENABLE is already above.
            { D3DRS_AMBIENT,                  "AMBIENT" },
            { D3DRS_AMBIENTMATERIALSOURCE,    "AMBIENTMATERIALSOURCE" },
            { D3DRS_SPECULARMATERIALSOURCE,   "SPECULARMATERIALSOURCE" },
            { D3DRS_EMISSIVEMATERIALSOURCE,   "EMISSIVEMATERIALSOURCE" },
            { D3DRS_SPECULARENABLE,           "SPECULARENABLE" },
            { D3DRS_NORMALIZENORMALS,         "NORMALIZENORMALS" },
            { D3DRS_LOCALVIEWER,              "LOCALVIEWER" },
            { D3DRS_VERTEXBLEND,              "VERTEXBLEND" },
            { D3DRS_INDEXEDVERTEXBLENDENABLE, "INDEXEDVERTEXBLENDENABLE" },
            { D3DRS_CLIPPING,                 "CLIPPING" },
            { D3DRS_LASTPIXEL,                "LASTPIXEL" },
            { D3DRS_DITHERENABLE,             "DITHERENABLE" },
            { D3DRS_ANTIALIASEDLINEENABLE,    "ANTIALIASEDLINEENABLE" },
            { D3DRS_DEPTHBIAS,                "DEPTHBIAS" },
            { D3DRS_SLOPESCALEDEPTHBIAS,      "SLOPESCALEDEPTHBIAS" },
            { D3DRS_WRAP0,                    "WRAP0" },
            { D3DRS_POINTSPRITEENABLE,        "POINTSPRITEENABLE" },
            { D3DRS_RANGEFOGENABLE,           "RANGEFOGENABLE" },
            { D3DRS_FOGCOLOR,                 "FOGCOLOR" },
            { D3DRS_FOGTABLEMODE,             "FOGTABLEMODE" },
            { D3DRS_FOGVERTEXMODE,            "FOGVERTEXMODE" },
        };
        for (const RSEntry& e : kStates)
        {
            DWORD v = 0;
            m_pDevice->GetRenderState(e.rs, &v);
            fprintf(f, "  RS_%s = %lu\n", e.name, (unsigned long)v);
        }
    }

    // --- Texture stage states + bound texture pointer (stages 0,1) ---
    {
        struct TSSEntry { D3DTEXTURESTAGESTATETYPE ts; const char* name; };
        static const TSSEntry kStageStates[] = {
            { D3DTSS_COLOROP,               "COLOROP" },
            { D3DTSS_COLORARG1,             "COLORARG1" },
            { D3DTSS_COLORARG2,             "COLORARG2" },
            { D3DTSS_ALPHAOP,               "ALPHAOP" },
            { D3DTSS_ALPHAARG1,             "ALPHAARG1" },
            { D3DTSS_ALPHAARG2,             "ALPHAARG2" },
            { D3DTSS_RESULTARG,             "RESULTARG" },
            { D3DTSS_TEXCOORDINDEX,         "TEXCOORDINDEX" },
            { D3DTSS_TEXTURETRANSFORMFLAGS, "TEXTURETRANSFORMFLAGS" },
        };
        for (DWORD stage = 0; stage <= 1; ++stage)
        {
            for (const TSSEntry& e : kStageStates)
            {
                DWORD v = 0;
                m_pDevice->GetTextureStageState(stage, e.ts, &v);
                fprintf(f, "  TSS%lu_%s = %lu\n", (unsigned long)stage, e.name, (unsigned long)v);
            }
            IDirect3DBaseTexture9* t = NULL;
            m_pDevice->GetTexture(stage, &t);
            fprintf(f, "  TEX%lu_ptr = %p\n", (unsigned long)stage, (void*)t);
            if (t) t->Release();
        }
    }

    // --- Sampler states (samplers 0,1) ---
    {
        struct SampEntry { D3DSAMPLERSTATETYPE ss; const char* name; };
        static const SampEntry kSamplerStates[] = {
            { D3DSAMP_MINFILTER,    "MINFILTER" },
            { D3DSAMP_MAGFILTER,    "MAGFILTER" },
            { D3DSAMP_MIPFILTER,    "MIPFILTER" },
            { D3DSAMP_ADDRESSU,     "ADDRESSU" },
            { D3DSAMP_ADDRESSV,     "ADDRESSV" },
            { D3DSAMP_SRGBTEXTURE,  "SRGBTEXTURE" },
            { D3DSAMP_MAXMIPLEVEL,  "MAXMIPLEVEL" },
            { D3DSAMP_MIPMAPLODBIAS,"MIPMAPLODBIAS" },
        };
        for (DWORD s = 0; s <= 1; ++s)
        {
            for (const SampEntry& e : kSamplerStates)
            {
                DWORD v = 0;
                m_pDevice->GetSamplerState(s, e.ss, &v);
                fprintf(f, "  SAMP%lu_%s = %lu\n", (unsigned long)s, e.name, (unsigned long)v);
            }
        }
    }

    // --- Bindings: render target, depth/stencil, shaders, FVF, vertex decl, viewport ---
    {
        IDirect3DSurface9* rt = NULL;
        m_pDevice->GetRenderTarget(0, &rt);
        fprintf(f, "  RT0_ptr = %p\n", (void*)rt);
        if (rt) rt->Release();

        IDirect3DSurface9* ds = NULL;
        m_pDevice->GetDepthStencilSurface(&ds);
        fprintf(f, "  DEPTHSTENCIL_ptr = %p\n", (void*)ds);
        if (ds) ds->Release();

        D3DVIEWPORT9 vp;
        ZeroMemory(&vp, sizeof(vp));
        m_pDevice->GetViewport(&vp);
        fprintf(f, "  VIEWPORT = X=%lu Y=%lu W=%lu H=%lu MinZ=%f MaxZ=%f\n",
                (unsigned long)vp.X, (unsigned long)vp.Y,
                (unsigned long)vp.Width, (unsigned long)vp.Height,
                vp.MinZ, vp.MaxZ);

        IDirect3DVertexShader9* vs = NULL;
        m_pDevice->GetVertexShader(&vs);
        fprintf(f, "  VERTEXSHADER_ptr = %p\n", (void*)vs);
        if (vs) vs->Release();

        IDirect3DPixelShader9* ps = NULL;
        m_pDevice->GetPixelShader(&ps);
        fprintf(f, "  PIXELSHADER_ptr = %p\n", (void*)ps);
        if (ps) ps->Release();

        DWORD fvf = 0;
        m_pDevice->GetFVF(&fvf);
        fprintf(f, "  FVF = 0x%08lx\n", (unsigned long)fvf);

        IDirect3DVertexDeclaration9* decl = NULL;
        m_pDevice->GetVertexDeclaration(&decl);
        fprintf(f, "  VERTEXDECL_ptr = %p\n", (void*)decl);
        if (decl) {
            // [red-bug confirm] Dump the element LAYOUT, not just the pointer. The
            // shadow-toggle leak swaps the particle's 5-element 44B ParticleElements
            // decl for an empty FVF-derived layout; the element array is run-invariant
            // so it confirms the leak/fix across separate --capture runs.
            D3DVERTEXELEMENT9 elems[MAXD3DDECLLENGTH + 1];
            UINT nel = 0;
            if (SUCCEEDED(decl->GetDeclaration(elems, &nel))) {
                UINT real = 0;
                for (UINT i = 0; i < nel && i <= MAXD3DDECLLENGTH; i++) {
                    if (elems[i].Stream == 0xFF) break;  // D3DDECL_END sentinel
                    real++;
                }
                fprintf(f, "  VERTEXDECL_elements = %u\n", (unsigned)real);
                for (UINT i = 0; i < real; i++)
                    fprintf(f, "    elem[%u] stream=%u off=%u type=%u usage=%u uidx=%u\n",
                            i, elems[i].Stream, elems[i].Offset, elems[i].Type,
                            elems[i].Usage, elems[i].UsageIndex);
            } else {
                fprintf(f, "  VERTEXDECL_elements = (GetDeclaration FAILED)\n");
            }
            decl->Release();
        }
    }

    // [texture-content hunt] The particle's COLOR/NORMAL textures: descriptor +
    // a few center-row pixel samples. New hypothesis for the shadow leak — the
    // particle's TEXTURE CONTENT goes black (additive vanish / transparent black
    // = the sampled src color reads 0), which the render-state dump can't see.
    //
    // These particle textures come from D3DXCreateTextureFromFileInMemory, which
    // silently allocates D3DPOOL_DEFAULT (main.cpp createTexture) — so a direct
    // LockRect FAILS (D3DERR_INVALIDCALL on a non-managed, non-dynamic texture).
    // To read the content anyway we copy level-0 into a SYSTEMMEM scratch surface
    // via D3DXLoadSurfaceFromSurface (which reads through the device, default pool
    // and all) and lock THAT. A managed/system-mem texture is still locked in
    // place (fast path). Several points across the center row separate a
    // uniformly-black texture from a sparse one whose center happens to be empty.
    {
        // Sample the center row of a locked 32bpp surface at 1/4, 1/2, 3/4 across.
        // Bytes are in the texture's native channel order (B,G,R,A for the *R8G8B8
        // formats; R,G,B,A for the *B8G8R8 formats) — labelled BGRA as the common
        // A8R8G8B8 case; the format field disambiguates.
        auto sampleRow = [f](const char* label, const D3DLOCKED_RECT& lr,
                             UINT width, UINT height, const char* via)
        {
            const UINT cy = height / 2;
            const BYTE* row = (const BYTE*)lr.pBits + (size_t)cy * lr.Pitch;
            const UINT xs[3] = { width / 4, width / 2, (width * 3) / 4 };
            for (int i = 0; i < 3; ++i)
            {
                const BYTE* px = row + (size_t)xs[i] * 4;
                fprintf(f, "  %s px(x=%u,y=%u) BGRA=%u,%u,%u,%u [%s]\n",
                        label, xs[i], cy,
                        (unsigned)px[0], (unsigned)px[1], (unsigned)px[2], (unsigned)px[3], via);
            }
        };

        struct TexProbe { IDirect3DTexture9* tex; const char* label; };
        const TexProbe probes[] = { { colorTex, "COLORTEX" }, { normalTex, "NORMALTEX" } };
        for (const TexProbe& p : probes)
        {
            if (!p.tex)
            {
                fprintf(f, "  %s = (null)\n", p.label);
                continue;
            }

            D3DSURFACE_DESC d;
            ZeroMemory(&d, sizeof(d));
            p.tex->GetLevelDesc(0, &d);
            fprintf(f, "  %s desc=%ux%u fmt=%d pool=%d usage=%lu levels=%lu\n",
                    p.label, d.Width, d.Height, (int)d.Format, (int)d.Pool,
                    (unsigned long)d.Usage, (unsigned long)p.tex->GetLevelCount());

            const bool is32bppArgb =
                (d.Format == D3DFMT_A8R8G8B8 || d.Format == D3DFMT_X8R8G8B8 ||
                 d.Format == D3DFMT_A8B8G8R8 || d.Format == D3DFMT_X8B8G8R8);
            if (!is32bppArgb)
            {
                fprintf(f, "  %s content sample skipped (fmt=%d)\n", p.label, (int)d.Format);
                continue;
            }

            // Fast path: managed/sysmem textures lock in place.
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(p.tex->LockRect(0, &lr, NULL, D3DLOCK_READONLY)))
            {
                sampleRow(p.label, lr, d.Width, d.Height, "direct");
                p.tex->UnlockRect(0);
                continue;
            }

            // Fallback: copy level-0 into a SYSTEMMEM scratch surface so a
            // D3DPOOL_DEFAULT texture's content is still readable.
            IDirect3DSurface9* src = NULL;
            IDirect3DSurface9* scratch = NULL;
            HRESULT hr = p.tex->GetSurfaceLevel(0, &src);
            if (SUCCEEDED(hr))
                hr = m_pDevice->CreateOffscreenPlainSurface(
                         d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &scratch, NULL);
            if (SUCCEEDED(hr))
                hr = D3DXLoadSurfaceFromSurface(scratch, NULL, NULL, src, NULL, NULL,
                                                D3DX_FILTER_NONE, 0);
            if (SUCCEEDED(hr) && SUCCEEDED(scratch->LockRect(&lr, NULL, D3DLOCK_READONLY)))
            {
                sampleRow(p.label, lr, d.Width, d.Height, "scratch");
                scratch->UnlockRect();
            }
            else
            {
                fprintf(f, "  %s content sample FAILED hr=0x%08lx (pool=%d, scratch copy)\n",
                        p.label, (unsigned long)hr, (int)d.Pool);
            }
            if (scratch) scratch->Release();
            if (src) src->Release();
        }
    }

    fclose(f);
}

// Draw the imported reference object in two phases (opaque then
// transparent). Each rigid sub-mesh is placed by its bone's object-space matrix
// (sub.placement) times the live object world, and runs its OWN game shader 1:1
// with the same engine binding the particle / dome paths use. Render-state
// save/restore so the particle draw is unaffected. No-op when none loaded/resolved.
void Engine::RenderReferenceObject()
{
    if (!m_referenceObjectVisible)
        return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved())
        return;

    const D3DXMATRIX objectWorld = ReferenceObjectDisplayWorld();   // eased (render)

    // [refZ] Env-gated trace of the reference-object placement at the draw -- the
    // diagnostic that root-caused the "land unit floats above the ground" report
    // (a stale per-object transform leaking across an object swap; fixed by
    // ReferenceTransformMemory.h). Prints the committed + eased positions, the
    // per-object scale, the final world translation, and the ground Z, throttled so
    // a --record run doesn't spew. The env is read ONCE (static) so it's truly free
    // when ALO_REFZ is unset; kept as a durable probe for future placement reports.
    static int s_refzOn = -1;
    if (s_refzOn < 0) s_refzOn = (getenv("ALO_REFZ") != nullptr) ? 1 : 0;
    if (s_refzOn)
    {
        static int s_refz = 0;
        if ((s_refz++ % 30) == 0)
        {
            D3DXVECTOR3 omn(0,0,0), omx(0,0,0);
            m_referenceObjectMesh.GetBoundingBox(omn, omx);
            fprintf(stderr,
                "[refZ] f=%d name=%s vis=%d sel=%d scale=%.3f refPos=(%.3f,%.3f,%.3f) "
                "dispPos=(%.3f,%.3f,%.3f) worldT=(%.3f,%.3f,%.3f) groundZ=%.3f objAABBz=[%.3f..%.3f]\n",
                s_refz, m_referenceObjectName.c_str(),
                m_referenceObjectVisible ? 1 : 0, m_referenceObjectSelected ? 1 : 0,
                m_referenceScaleFactor,
                m_referencePosition.x, m_referencePosition.y, m_referencePosition.z,
                m_displayPosition.x, m_displayPosition.y, m_displayPosition.z,
                objectWorld._41, objectWorld._42, objectWorld._43,
                m_groundZ, omn.z, omx.z);
            fflush(stderr);
        }
    }

    DWORD oldAlphaBlend, oldSrcBlend, oldDestBlend, oldZWrite, oldZEnable, oldCull;
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    m_pDevice->GetRenderState(D3DRS_SRCBLEND,         &oldSrcBlend);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,        &oldDestBlend);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    m_pDevice->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    m_pDevice->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    D3DXVECTOR4 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z, 1.0f);

    // Two phases matching the game's Opaque-then-Transparent order: opaque
    // sub-meshes (fill + depth) first, then additive/alpha layers blended on top.
    // Each sub-mesh's render state is set here, not by the .fxo (its SB block is
    // compiled out -- ALAMO_STATE_BLOCKS 0). Opaque uses
    // CULL_CW (the editor renders RIGHT-handed -- LookAtRH/PerspectiveFovRH -- which
    // flips screen-space winding vs the game, so game-front faces present as CW
    // here; CCW culled the front faces and showed the lit hull interior, the
    // "inverted normals" report). Transparent uses CULL_NONE (glows/shields are
    // two-sided; MeshShield itself sets CullMode=NONE) + z-write OFF so the layers
    // don't occlude each other; depth TEST stays on so the opaque hull occludes
    // transparent geometry behind it.
    for (int phase = 0; phase < 2; ++phase)
    {
      const bool opaquePhase = (phase == 0);
      // Draw the unit (mi == 0) then each hardpoint attach model, both in this
      // phase. worldBase = objectWorld for the unit, or attachBone * objectWorld for an
      // attachment (mounting the attach model at the unit's named Attachment_Bone), so
      // its own additive/alpha layers (turret glows) blend over the assembled unit.
      for (size_t mi = 0; mi <= m_referenceAttachments.size(); ++mi)
      {
      ReferenceObjectMesh& refMesh = (mi == 0) ? m_referenceObjectMesh
                                               : m_referenceAttachments[mi - 1]->mesh;
      const D3DXMATRIX worldBase   = (mi == 0) ? objectWorld
                                               : (m_referenceAttachments[mi - 1]->boneMatrix * objectWorld);
      for (RefSubMeshGpu& sub : refMesh.SubMeshes())
      {
        if (sub.effect == NULL || sub.vb == NULL || sub.ib == NULL || sub.decl == NULL)
            continue;
        const bool subOpaque = (sub.renderClass == ALO_RC_OPAQUE);
        if (subOpaque != opaquePhase)
            continue;   // opaque sub-meshes in phase 0, additive/alpha in phase 1

        m_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        if (subOpaque)
        {
            m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     TRUE);
            m_pDevice->SetRenderState(D3DRS_CULLMODE,         D3DCULL_CW);
            m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        }
        else
        {
            m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
            m_pDevice->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
            m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            if (sub.renderClass == ALO_RC_ADDITIVE)
            {
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_ONE);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            }
            else // ALO_RC_ALPHA
            {
                m_pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
                m_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            }
        }

        D3DXMATRIX world = sub.placement * worldBase;
        D3DXMATRIX wvp   = world * m_view * m_projection;

        // Object-space eye + light for the bump shader's tangent-space lighting
        // (m_eyePosObj / m_light0ObjVector). These equal the world values only at
        // identity world; transform by inverse-world so the bump + specular stay
        // correct once the object is moved/rotated (picker spinners / gizmo).
        D3DXMATRIX invWorld;
        if (!D3DXMatrixInverse(&invWorld, NULL, &world))   // singular (a degenerate bone matrix)
            D3DXMatrixIdentity(&invWorld);                 // sane fallback: obj-space == world-space
        const D3DXVECTOR3 lightDir3(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
        D3DXVECTOR3 eyeObj3, lightObj3;
        D3DXVec3TransformCoord(&eyeObj3, &m_eye.Position, &invWorld);   // point
        D3DXVec3TransformNormal(&lightObj3, &lightDir3, &invWorld);     // direction
        const D3DXVECTOR4 eyeObj  (eyeObj3.x,   eyeObj3.y,   eyeObj3.z,   1.0f);
        const D3DXVECTOR4 lightObj(lightObj3.x, lightObj3.y, lightObj3.z, 0.0f);

        ID3DXEffect* fx = sub.effect->getD3DEffect();   // AddRef'd
        const Effect::Handles& h = sub.effect->getHandles();

        fx->SetMatrix(h.hWorld,               &world);
        fx->SetMatrix(h.hWorldViewProjection, &wvp);
        fx->SetVector(h.hEyePosition,         &eyePos);
        fx->SetVector(h.hEyeObjPosition,      &eyeObj);     // m_eyePosObj (was unbound -> wrong specular when rotated)
        fx->SetVector(h.hGlobalAmbient,       &m_ambient);
        fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
        fx->SetVector(h.hDirLightObjVec0,     &lightObj);   // object-space light dir (was world)
        fx->SetVector(h.hDirLightDiffuse,     &m_lights[0].Diffuse);
        fx->SetVector(h.hDirLightSpecular,    &m_lights[0].Specular);
        fx->SetMatrixArray(h.hSphLightAll,    m_sphLightAll,  3);
        fx->SetMatrixArray(h.hSphLightFill,   m_sphLightFill, 3);
        fx->SetFloat(h.hTime,                 GetTimeF());

        // Skinned (RSkin) sub-mesh: render in BIND POSE. The RSkin VS uses
        // m_viewProj (world->clip) + a float4x3 m_skinMatrixArray[24] bone palette
        // (P = mul(In.Pos, palette[Normal.w])), NOT m_world / m_worldViewProj. At
        // bind pose every bone's skin matrix collapses to the object world (verified
        // against the alo-viewer's invBind*current build), so bind a UNIFORM
        // objectWorld palette -> P = mul(In.Pos, objectWorld) = world. The .alo
        // skinned verts are model-space, so `world` here == objectWorld (placement
        // is identity for skinned).
        if (sub.skinned && h.hSkinMatrixArray)
        {
            D3DXMATRIX palette[24];
            for (int b = 0; b < 24; ++b) palette[b] = world;
            fx->SetMatrix(h.hViewProjection, &m_viewProjection);
            fx->SetMatrixArray(h.hSkinMatrixArray, palette, 24);
        }

        ApplyAloMaterialParams(fx, sub.params, sub.matHandles, sub.matTextures);

        m_pDevice->SetVertexDeclaration(sub.decl);
        m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
        m_pDevice->SetIndices(sub.ib);

        UINT passes = 0;
        fx->Begin(&passes, 0);
        for (UINT pass = 0; pass < passes; ++pass)
        {
            fx->BeginPass(pass);
#ifndef NDEBUG
            if (sub.primitiveCount > 0)
            {
                DWORD zw = 0, cm = 0;
                m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
                m_pDevice->GetRenderState(D3DRS_CULLMODE,     &cm);
                fprintf(stderr, "[RefObjDraw] %s pass %u/%u zwrite=%lu cull=%lu prims=%u\n",
                        sub.shaderName.c_str(), pass + 1, passes, zw, cm, sub.primitiveCount);
            }
#endif
            m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0,
                                            sub.vertexCount, 0, sub.primitiveCount);
            fx->EndPass();
        }
        fx->End();
        fx->Release();
      }   // sub-mesh loop
      }   // mesh loop (unit + attachments)
    }     // phase loop

    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,         oldSrcBlend);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,        oldDestBlend);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

void Engine::RenderReferenceShadows()
{
    if (!m_modelShadowsEnabled || !m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;

    bool any = !m_referenceObjectMesh.ShadowSubMeshes().empty();
    for (size_t i = 0; !any && i < m_referenceAttachments.size(); ++i)
        any = !m_referenceAttachments[i]->mesh.ShadowSubMeshes().empty();
    if (!any) return;

    // [redbug] One-shot marker proving the shadow pass actually DRAWS volumes (not
    // merely that the shadow-volume shader was loaded during ref-object resolution,
    // which happens regardless of whether this pass runs). The regression guard
    // greps for this so it can't false-PASS on a
    // shader-load-without-draw. Past the early-returns above the function always
    // draws, so this fires exactly when a real shadow pass executes. Gated by
    // ALO_SHADER_DIAG + once-per-process so production stays silent.
    {
        static int  s_diag   = -1;
        static bool s_logged = false;
        if (s_diag < 0) s_diag = (getenv("ALO_SHADER_DIAG") != nullptr) ? 1 : 0;
        if (s_diag && !s_logged) {
            size_t vols = m_referenceObjectMesh.ShadowSubMeshes().size();
            for (size_t i = 0; i < m_referenceAttachments.size(); ++i)
                vols += m_referenceAttachments[i]->mesh.ShadowSubMeshes().size();
            printf("[redbug-shadow] RenderReferenceShadows drawing %zu shadow volume(s)\n", vols);
            fflush(stdout);
            s_logged = true;
        }
    }

    const D3DXMATRIX objectWorld = ReferenceObjectDisplayWorld();

    // Extrusion distance for the silhouette volume. alo-viewer uses a large fixed
    // 4000; scale up for big meshes so the volume reliably clears the ground plane.
    float extrusionDist = 4000.0f;
    {
        D3DXVECTOR3 objMin, objMax;
        if (m_referenceObjectMesh.GetBoundingBox(objMin, objMax)) {
            D3DXVECTOR3 d = objMax - objMin;
            extrusionDist = max(4000.0f, 2.0f * D3DXVec3Length(&d));
        }
    }

    // Eye-space depth push for the shadow volume (replaces the old clip-space NDC
    // bias). The volume is shoved a CONSTANT distance DEEPER in EYE space before
    // projection, so coincident near-cap faces z-fail deterministically (kills the
    // self-shadow flicker) WITHOUT the camera-distance drift the clip-Z bias caused.
    //
    // Why eye-space, not clip-space: the old `z_clip += kBias*w` was a CONSTANT
    // offset in NDC. But under this editor's fixed-near=1 / infinite-far projection
    // (z_ndc = 1 - 2/d), a constant NDC nudge maps to a WORLD-depth recession of the
    // volume of ~ (d^2/2)*kBias — QUADRATIC in camera distance d. So the shadow
    // contact held position up close but slid/peter-panned as the camera zoomed out
    // (the reported bug). A constant eye-space translate is a constant WORLD-depth
    // offset at every distance, so the contact holds position at all zooms. The push
    // does introduce a sub-percent screen-space shift of the volume (a deeper vertex
    // projects slightly toward the principal point) but it SHRINKS with distance and
    // is negligible vs the d^2 drift it removes.
    //
    // Sign/magnitude tunable: too small -> self-shadow flicker returns; too large ->
    // the shadow visibly detaches from the model base up close.
    float kShadowEyePush = 2.0f;   // world units to push the volume DEEPER (the fix)
    float kOldClipZBias  = 0.0f;   // 0 = use the eye push (default, shipped behaviour)
#ifndef NDEBUG
    // [shadow-repro] Diagnostic overrides (Debug-only, inert unless set):
    //   ALO_SHADOW_ZPUSH = eye-space push distance in world units (A/B the magnitude).
    //   ALO_SHADOW_ZBIAS = revert to the OLD clip-Z NDC bias with this value, so the
    //     pre-fix camera-distance drift can be reproduced for before/after capture.
    //     ALO_SHADOW_ZBIAS=0 disables BOTH (the no-bias baseline; flicker expected).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZPUSH", b, sizeof(b)) > 0) kShadowEyePush = (float)atof(b); }
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_ZBIAS", b, sizeof(b)) > 0) { kOldClipZBias = (float)atof(b); kShadowEyePush = 0.0f; } }
#endif
    // RH eye space looks down -Z, so DEEPER = more negative z_eye => translate by
    // -push. Inserted between view and projection: vpPush = view * zpush * proj.
    // (zbiasClip is identity in the shipped path; only the Debug A/B sets it.)
    D3DXMATRIX zpush; D3DXMatrixTranslation(&zpush, 0.0f, 0.0f, -kShadowEyePush);
    D3DXMATRIX zbiasClip; D3DXMatrixIdentity(&zbiasClip); zbiasClip._43 = kOldClipZBias;
    D3DXMATRIX viewProjPush = m_view * zpush * m_projection * zbiasClip;

    // Shadow tint (the multiplicative ZERO/SRCCOLOR darken colour, from m_shadow =
    // the Lighting panel's Sun Shadow Color).
    float shR = m_shadow.x, shG = m_shadow.y, shB = m_shadow.z;
#ifndef NDEBUG
    // [shadow-repro] Debug override so the faint default tint can be forced dark
    // enough to MEASURE the edge feather / contact line in headless captures.
    // ALO_SHADOW_TINT = a single grey level [0..1] (0 = black, fully dark shadow).
    { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_TINT", b, sizeof(b)) > 0) { float v=(float)atof(b); shR=shG=shB=v; } }
#endif

    // --- save every state we touch ---
    DWORD oCW,oZF,oZW,oZE,oSE,oTSS,oSR,oSM,oSWM,oCull,oSFn,oSP,oSZF,oSFa,
          oCcwFn,oCcwP,oCcwZF,oCcwFa,oAB,oSB,oDB,oLit,oFVF,oATE;
    DWORD oTexCOP,oTexCA1,oTexAOP,oTexAA1;
    m_pDevice->GetRenderState(D3DRS_COLORWRITEENABLE,&oCW); m_pDevice->GetRenderState(D3DRS_ZFUNC,&oZF);
    m_pDevice->GetRenderState(D3DRS_ZWRITEENABLE,&oZW); m_pDevice->GetRenderState(D3DRS_ZENABLE,&oZE);
    m_pDevice->GetRenderState(D3DRS_STENCILENABLE,&oSE); m_pDevice->GetRenderState(D3DRS_TWOSIDEDSTENCILMODE,&oTSS);
    m_pDevice->GetRenderState(D3DRS_STENCILREF,&oSR); m_pDevice->GetRenderState(D3DRS_STENCILMASK,&oSM);
    m_pDevice->GetRenderState(D3DRS_STENCILWRITEMASK,&oSWM); m_pDevice->GetRenderState(D3DRS_CULLMODE,&oCull);
    m_pDevice->GetRenderState(D3DRS_STENCILFUNC,&oSFn); m_pDevice->GetRenderState(D3DRS_STENCILPASS,&oSP);
    m_pDevice->GetRenderState(D3DRS_STENCILZFAIL,&oSZF); m_pDevice->GetRenderState(D3DRS_STENCILFAIL,&oSFa);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILFUNC,&oCcwFn); m_pDevice->GetRenderState(D3DRS_CCW_STENCILPASS,&oCcwP);
    m_pDevice->GetRenderState(D3DRS_CCW_STENCILZFAIL,&oCcwZF); m_pDevice->GetRenderState(D3DRS_CCW_STENCILFAIL,&oCcwFa);
    m_pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE,&oAB); m_pDevice->GetRenderState(D3DRS_SRCBLEND,&oSB);
    m_pDevice->GetRenderState(D3DRS_DESTBLEND,&oDB); m_pDevice->GetRenderState(D3DRS_LIGHTING,&oLit);
    m_pDevice->GetRenderState(D3DRS_ALPHATESTENABLE,&oATE);
    m_pDevice->GetFVF(&oFVF);
    m_pDevice->GetTextureStageState(0,D3DTSS_COLOROP,&oTexCOP); m_pDevice->GetTextureStageState(0,D3DTSS_COLORARG1,&oTexCA1);
    m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAOP,&oTexAOP); m_pDevice->GetTextureStageState(0,D3DTSS_ALPHAARG1,&oTexAA1);
    IDirect3DVertexDeclaration9* oDecl=NULL; m_pDevice->GetVertexDeclaration(&oDecl);
    IDirect3DVertexShader9* oVS=NULL; m_pDevice->GetVertexShader(&oVS);
    IDirect3DPixelShader9*  oPS=NULL; m_pDevice->GetPixelShader(&oPS);
    IDirect3DBaseTexture9*  oTex0=NULL; m_pDevice->GetTexture(0,&oTex0);
    IDirect3DVertexBuffer9* oStream0=NULL; UINT oStreamOffset=0, oStreamStride=0;
    m_pDevice->GetStreamSource(0, &oStream0, &oStreamOffset, &oStreamStride);
    IDirect3DIndexBuffer9*  oIndices=NULL; m_pDevice->GetIndices(&oIndices);

    // [soft-shadows] Soft path is available only when the toggle is on AND the
    // blur effect + mask RT both came up. Otherwise fall back to the shipped hard
    // darken quad (no regression). Decided once, up front, so the save/restore of
    // the extra resources (RT / depth-stencil / viewport / samplers 0-3) is paired.
    const bool soft = m_softShadowsEnabled && m_shadowBlurReady
                   && m_pShadowBlurEffect != NULL && m_pShadowMask != NULL;

    // Extra save for the soft path's RT detour + sampler binds. AddRef'd handles
    // released in the restore tail; sampler filter/address states restored too.
    IDirect3DSurface9* oRT0 = NULL;  IDirect3DSurface9* oDS = NULL;
    D3DVIEWPORT9 oViewport;
    IDirect3DBaseTexture9* oTexS[4] = { NULL,NULL,NULL,NULL };
    DWORD oMinF[4], oMagF[4], oMipF[4], oAddrU[4], oAddrV[4];
    if (soft)
    {
        m_pDevice->GetRenderTarget(0, &oRT0);          // AddRef'd
        m_pDevice->GetDepthStencilSurface(&oDS);       // AddRef'd (may be NULL)
        m_pDevice->GetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->GetTexture(s, &oTexS[s]);       // AddRef'd
            m_pDevice->GetSamplerState(s, D3DSAMP_MINFILTER, &oMinF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MAGFILTER, &oMagF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_MIPFILTER, &oMipF[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSU, &oAddrU[s]);
            m_pDevice->GetSamplerState(s, D3DSAMP_ADDRESSV, &oAddrV[s]);
        }
    }

    // ============ VOLUME PASS — write stencil (single-pass two-sided z-fail) ============
    // Mirrors alo-viewer: one CULLMODE=NONE pass with TWOSIDEDSTENCILMODE so CW faces
    // INCR and CCW faces DECR the stencil on z-fail in a single draw.
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
    m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,TRUE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,1);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_INCR);   // CW faces incr on zfail
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,D3DCMP_ALWAYS);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,D3DSTENCILOP_DECR);// CCW faces decr on zfail
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,D3DSTENCILOP_KEEP);

    // Lambda draws every shadow sub-mesh for the main mesh + all attachments.
    // Technique t0_zfail is kept for its VS (extrudes silhouette to infinity);
    // the technique's own stencil state-block is overridden by our hand-coded ops.
    auto drawVolumes = [&]()
    {
        for (size_t mi = 0; mi <= m_referenceAttachments.size(); ++mi)
        {
            ReferenceObjectMesh& refMesh = (mi==0) ? m_referenceObjectMesh
                                                   : m_referenceAttachments[mi-1]->mesh;
            const D3DXMATRIX worldBase   = (mi==0) ? objectWorld
                                                   : (m_referenceAttachments[mi-1]->boneMatrix * objectWorld);
            for (RefSubMeshGpu& sub : refMesh.ShadowSubMeshes())
            {
                if (sub.effect==NULL || sub.vb==NULL || sub.ib==NULL || sub.decl==NULL) continue;
                if (!sub.effect->isShadowVolume()) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' resolved to a non-shadow effect - skipped\n", sub.shaderName.c_str());
#endif
                    continue;
                }
                D3DXMATRIX world = sub.placement * worldBase;
                D3DXMATRIX invWorld;
                if (!D3DXMatrixInverse(&invWorld,NULL,&world)) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': singular world matrix — identity used for light direction (shadow direction will be wrong)\n", sub.shaderName.c_str());
#endif
                    D3DXMatrixIdentity(&invWorld);
                }
                const D3DXVECTOR3 lightDir3(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
                D3DXVECTOR3 lightObj3; D3DXVec3TransformNormal(&lightObj3,&lightDir3,&invWorld);
                const D3DXVECTOR4 lightObj(lightObj3.x,lightObj3.y,lightObj3.z,0.0f);

                ID3DXEffect* fx = sub.effect->getD3DEffect();
                const Effect::Handles& h = sub.effect->getHandles();
                if (FAILED(fx->SetTechnique("t0_zfail"))) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s' has no t0_zfail technique - skipped\n", sub.shaderName.c_str());
#endif
                    fx->Release(); continue;
                }
                // Eye-space-pushed transforms (constant world-depth recession — see
                // the kShadowEyePush note above; replaces the old clip-Z bias that
                // drifted ~d^2 with camera distance). Rigid VS reads WorldViewProjection,
                // skinned VS reads ViewProjection; both ride the same pushed projection.
                D3DXMATRIX wvpB = world * viewProjPush;
                D3DXMATRIX vpB  = viewProjPush;
                fx->SetMatrix(h.hWorldViewProjection, &wvpB);
                fx->SetMatrix(h.hViewProjection,      &vpB);
                fx->SetVector(h.hDirLightObjVec0,     &lightObj);
                fx->SetVector(h.hDirLightVec0,        &m_lights[0].Position);
                if (sub.skinned && h.hSkinMatrixArray) {
                    D3DXMATRIX palette[24]; for (int b=0;b<24;++b) palette[b]=world;
                    fx->SetMatrixArray(h.hSkinMatrixArray, palette, 24);
                }
                D3DXHANDLE hExtr = fx->GetParameterBySemantic(NULL, "SHADOW_EXTRUSION_DISTANCE");
                if (hExtr) { D3DXVECTOR4 ex(extrusionDist,extrusionDist,extrusionDist,extrusionDist); fx->SetVector(hExtr, &ex); }

                m_pDevice->SetVertexDeclaration(sub.decl);
                m_pDevice->SetStreamSource(0, sub.vb, 0, sub.stride);
                m_pDevice->SetIndices(sub.ib);
                UINT passes=0;
                if (FAILED(fx->Begin(&passes,0)) || passes==0) {
#ifndef NDEBUG
                    fprintf(stderr, "[shadow] '%s': fx->Begin failed or returned 0 passes — skipping sub-mesh\n", sub.shaderName.c_str());
#endif
                    fx->End(); fx->Release(); continue;
                }
                for (UINT p=0;p<passes;++p) {
                    fx->BeginPass(p);
                    m_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,0,0,sub.vertexCount,0,sub.primitiveCount);
                    fx->EndPass();
                }
                fx->End(); fx->Release();
            }
        }
    };

    // Single two-sided pass: CW faces INCR, CCW faces DECR on z-fail (states set above).
    drawVolumes();

    if (soft)
    {
        // ============ SOFT PATH — mask-to-alpha + blurred multiply composite ============
        // Port of FoC's doSoftShadows: bake the stencil region into a screen-space
        // mask's ALPHA, then run StencilDarkenFinalBlur (4-tap blur + tint) as a
        // ZERO/SRCCOLOR multiply onto the scene. NO depth-bias states anywhere.
        const DWORD tint = D3DCOLOR_COLORVALUE(shR, shG, shB, 1.0f);

        // The mask RT to render the stencil mask into: the matching-MSAA surface
        // when MSAA is active (the stencil test needs the multisampled depth still
        // bound), else the non-MS mask texture's top surface.
        IDirect3DSurface9* pMaskTop  = NULL;       // m_pShadowMask L0, AddRef'd
        m_pShadowMask->GetSurfaceLevel(0, &pMaskTop);
        IDirect3DSurface9* pRenderInto = NULL;     // borrowed (no extra ref to release)
        if (m_msaaActive && m_pShadowMaskMsaa) { pRenderInto = m_pShadowMaskMsaa; }
        else                                    { pRenderInto = pMaskTop; }

        // --- mask-to-alpha (hand-coded StencilDarkenToAlpha state block) ---
        // Bind the mask RT but KEEP the current depth-stencil (the stencil written
        // by the volume pass must persist for the NOTEQUAL test).
        m_pDevice->SetRenderTarget(0, pRenderInto);
        D3DVIEWPORT9 maskVp = { 0, 0,
            m_presentationParameters.BackBufferWidth,
            m_presentationParameters.BackBufferHeight, 0.0f, 1.0f };
        m_pDevice->SetViewport(&maskVp);
        // Clear ALPHA to white (=1, "no shadow") everywhere; the gated quad punches
        // alpha=0 ("shadow") into the stencil region. RGB is irrelevant (blur reads
        // only .a) but a full white clear keeps the target well-defined.
        m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0xFFFFFFFF, 1.0f, 0);

        m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);   // shadow where stencil != 0
        m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
        m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
        m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
        m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_ALWAYS);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA); // ALPHA only
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
        m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

        // Full-screen XYZRHW quad whose diffuse ALPHA = 0 -> writes alpha 0 into the
        // shadow region (stencil != 0), leaving the cleared white (1) elsewhere.
        const float mx0 = -0.5f, my0 = -0.5f;
        const float mx1 = (float)m_presentationParameters.BackBufferWidth  - 0.5f;
        const float my1 = (float)m_presentationParameters.BackBufferHeight - 0.5f;
        struct PTVtx { float x,y,z,rhw; DWORD c; };
        const DWORD shadowAlpha0 = 0x00000000;   // RGBA, alpha 0
        const PTVtx maskQuad[4] = {
            {mx0,my0,0,1,shadowAlpha0},{mx1,my0,0,1,shadowAlpha0},
            {mx0,my1,0,1,shadowAlpha0},{mx1,my1,0,1,shadowAlpha0} };
        m_pDevice->SetVertexShader(NULL);
        m_pDevice->SetPixelShader(NULL);
        m_pDevice->SetTexture(0,NULL);
        m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
        m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,maskQuad,sizeof(PTVtx));

        // --- MSAA resolve: multisampled mask surface -> the sampled texture ---
        if (m_msaaActive && m_pShadowMaskMsaa && pMaskTop)
        {
            m_pDevice->StretchRect(m_pShadowMaskMsaa, NULL, pMaskTop, NULL, D3DTEXF_NONE);
        }

        // --- restore scene RT + depth-stencil + viewport before the composite ---
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);

        // --- blur composite (StencilDarkenFinalBlur, ZERO/SRCCOLOR multiply) ---
        // Bind the resolved mask to sampler stages 0-3 (the .fx's sampler0..3 all
        // read the same mask; the VS spreads 4 tap offsets across them). LINEAR
        // filtering + clamp: the mask alpha is a HARD 0/1 step, so POINT-sampled
        // taps land on discrete texels and stair-step the edge (reads crisp/hard
        // even with a wide blurAmt). Bilinear lets each tap straddle the edge texel
        // so the 4-tap cross resolves into a smooth feather (bug-1 fix; pairs with
        // the wider blurAmt below). MinF/MagF are saved/restored in the tail.
        for (DWORD s = 0; s < 4; ++s)
        {
            m_pDevice->SetTexture(s, m_pShadowMask);
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU,  D3DTADDRESS_CLAMP);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV,  D3DTADDRESS_CLAMP);
        }

        m_pDevice->SetRenderState(D3DRS_STENCILENABLE,FALSE);
        // Depth-test the composite so the shadow only darkens SCENE GEOMETRY, never
        // the cleared-far background. The blurred mask bleeds the shadow a few texels
        // past the model/ground silhouette; without this, that bleed darkened the
        // empty background (black bands in the sky around the model). The quad sits at
        // NDC z=1.0 (far) with ZFUNC=GREATER, so it passes only where the depth buffer
        // holds geometry (depth < 1.0) and is rejected on the cleared background
        // (depth == 1.0). ZWRITE stays off (the depth-stencil oDS is bound here).
        m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);
        m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_ZFUNC,D3DCMP_GREATER);
        m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
        m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
        m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
        m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
        m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
        m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);

        // Clip-space full-screen quad (the VS multiplies by m_worldViewProj, which
        // we drive to identity). Vertex COLOR0 = tint (the ps lerps tint<->white by
        // the mask alpha). UV v increases downward (top of screen = v 0).
        //
        // CRITICAL: the shadow mask was rendered into the FULL backbuffer (the
        // mask-to-alpha viewport is 0,0..W,H), but this composite draws into the
        // SCENE VIEWPORT (oViewport) — a SUB-RECT of the backbuffer in the live
        // editor (the React panels inset the 3D view). So the quad's UVs must span
        // the scene viewport's sub-region of the mask, NOT 0..1, or the shadow is
        // scaled+offset off the object (the "floating silhouette" bug). With an
        // edge-to-edge clip(±1)→viewport and UV→sub-rect mapping, pixel centers land
        // on texel centers exactly — no separate half-texel nudge needed. In
        // --capture the viewport IS the full backbuffer, so this reduces to 0..1
        // (which is why the bug never showed in headless capture).
        const float Wbb = (float)m_presentationParameters.BackBufferWidth;
        const float Hbb = (float)m_presentationParameters.BackBufferHeight;
        float u0 = (float)oViewport.X / Wbb;
        float u1 = (float)(oViewport.X + oViewport.Width)  / Wbb;
        float v0 = (float)oViewport.Y / Hbb;
        float v1 = (float)(oViewport.Y + oViewport.Height) / Hbb;
#ifndef NDEBUG
        // [shadow-repro] ALO_SHADOW_VPFIX=0 reverts to the old full-mask 0..1 UVs
        // (the pre-fix "floating silhouette" behaviour) for a before/after A/B.
        { char b[8]; if (GetEnvironmentVariableA("ALO_SHADOW_VPFIX", b, sizeof(b)) > 0 && atof(b) == 0.0) { u0=0.0f; u1=1.0f; v0=0.0f; v1=1.0f; } }
#endif
        struct BlurVtx { float x,y,z; float nx,ny,nz; DWORD c; float u,v; };
        // z = 1.0 (NDC far) so the ZFUNC=GREATER depth test above darkens only
        // geometry pixels (depth < 1.0), not the cleared-far background.
        const BlurVtx blurQuad[4] = {
            { -1.0f,  1.0f, 1.0f, 0,0,1, tint, u0, v0 },  // top-left
            {  1.0f,  1.0f, 1.0f, 0,0,1, tint, u1, v0 },  // top-right
            { -1.0f, -1.0f, 1.0f, 0,0,1, tint, u0, v1 },  // bottom-left
            {  1.0f, -1.0f, 1.0f, 0,0,1, tint, u1, v1 },  // bottom-right
        };

        ID3DXEffect* bfx = m_pShadowBlurEffect->getD3DEffect();   // AddRef'd
        D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
        if (m_hShadowBlurWvp)  bfx->SetMatrix(m_hShadowBlurWvp, &ident);
        // blurAmt is the 4-tap cross half-spread in UV. The game's authored 0.0015
        // is only ~2 texels at the editor's RT size — reads HARD with POINT sampling.
        // The shader is only a 4-tap cross, so a WIDE spread (e.g. 0.009 ≈ 11 texels)
        // makes the 4 taps read as discrete offset copies — a plus-shaped GHOST/halo.
        // ~0.003 (≈4 texels) keeps the taps blending; paired with the LINEAR sampling
        // above that gives a soft edge without ghosting. (For a genuinely WIDE soft
        // shadow the 4-tap must become multi-pass / more taps — a separate change.)
        // Tunable via ALO_SHADOW_BLURAMT.
        float blurAmt = 0.003f;
#ifndef NDEBUG
        // [shadow-repro] Diagnostic blur-width override (A/B the feather width
        // without recompiling). Inert unless ALO_SHADOW_BLURAMT is set; Debug-only.
        { char b[64]; if (GetEnvironmentVariableA("ALO_SHADOW_BLURAMT", b, sizeof(b)) > 0) blurAmt = (float)atof(b); }
#endif
        if (m_hShadowBlurAmt)  bfx->SetFloat(m_hShadowBlurAmt, blurAmt);
        bfx->SetTechnique(m_hShadowBlurTech);
        // The blur uses a vertex shader (vs_1_1) reading POSITION/COLOR0/TEXCOORD0,
        // so emit via an FVF with a NON-transformed float4 POSITION (the VS applies
        // the identity m_worldViewProj). D3DFVF_XYZRHW would skip the VS entirely.
        // The NORMAL slot is required: the game's compiled vs_1_1 was authored
        // against the VERTEX_MESH_NU2C layout (pos/normal/uv/color), so its input
        // declaration expects a normal between POSITION and TEXCOORD0. Omitting it
        // shifts the texcoord register the VS reads, so every pixel sampled one
        // mask texel -> the whole frame got the shadow tint instead of just the
        // cast region. (Matches alo-viewer's m_sceneQuad FVF.)
        m_pDevice->SetVertexShader(NULL);   // FVF path -> fixed-function VS slot; effect's VS binds in BeginPass
        m_pDevice->SetFVF(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1);
        UINT bpasses = 0;
        if (SUCCEEDED(bfx->Begin(&bpasses, 0)) && bpasses > 0)
        {
            for (UINT bp = 0; bp < bpasses; ++bp)
            {
                bfx->BeginPass(bp);
                m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, blurQuad, sizeof(BlurVtx));
                bfx->EndPass();
            }
        }
        bfx->End();
        bfx->Release();

        SAFE_RELEASE(pMaskTop);
    }
    else
    {
    // ============ DARKEN PASS (hard fallback) ============
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,FALSE);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,D3DCMP_NOTEQUAL);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,0);
    m_pDevice->SetRenderState(D3DRS_STENCILMASK,0x3f);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,0);
    m_pDevice->SetRenderState(D3DRS_STENCILPASS,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,D3DSTENCILOP_KEEP);
    m_pDevice->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,0x0f);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    m_pDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
    m_pDevice->SetRenderState(D3DRS_LIGHTING,FALSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);

    D3DVIEWPORT9 vp; m_pDevice->GetViewport(&vp);
    const float x0=(float)vp.X-0.5f, y0=(float)vp.Y-0.5f;
    const float x1=(float)(vp.X+vp.Width)-0.5f, y1=(float)(vp.Y+vp.Height)-0.5f;
    const DWORD tint = D3DCOLOR_COLORVALUE(shR,shG,shB,1.0f);
    struct PTVtx { float x,y,z,rhw; DWORD c; };
    const PTVtx quad[4] = { {x0,y0,0,1,tint},{x1,y0,0,1,tint},{x0,y1,0,1,tint},{x1,y1,0,1,tint} };
    m_pDevice->SetVertexShader(NULL);
    m_pDevice->SetPixelShader(NULL);
    m_pDevice->SetTexture(0,NULL);
    m_pDevice->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(PTVtx));
    }

    // --- restore ---
    m_pDevice->SetRenderState(D3DRS_STENCILENABLE,oSE);
    m_pDevice->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE,oTSS);
    m_pDevice->SetRenderState(D3DRS_COLORWRITEENABLE,oCW); m_pDevice->SetRenderState(D3DRS_ZFUNC,oZF);
    m_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,oZW); m_pDevice->SetRenderState(D3DRS_ZENABLE,oZE);
    m_pDevice->SetRenderState(D3DRS_STENCILREF,oSR); m_pDevice->SetRenderState(D3DRS_STENCILMASK,oSM);
    m_pDevice->SetRenderState(D3DRS_STENCILWRITEMASK,oSWM); m_pDevice->SetRenderState(D3DRS_CULLMODE,oCull);
    m_pDevice->SetRenderState(D3DRS_STENCILFUNC,oSFn); m_pDevice->SetRenderState(D3DRS_STENCILPASS,oSP);
    m_pDevice->SetRenderState(D3DRS_STENCILZFAIL,oSZF); m_pDevice->SetRenderState(D3DRS_STENCILFAIL,oSFa);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILFUNC,oCcwFn); m_pDevice->SetRenderState(D3DRS_CCW_STENCILPASS,oCcwP);
    m_pDevice->SetRenderState(D3DRS_CCW_STENCILZFAIL,oCcwZF); m_pDevice->SetRenderState(D3DRS_CCW_STENCILFAIL,oCcwFa);
    m_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,oAB); m_pDevice->SetRenderState(D3DRS_SRCBLEND,oSB);
    m_pDevice->SetRenderState(D3DRS_DESTBLEND,oDB); m_pDevice->SetRenderState(D3DRS_LIGHTING,oLit);
    m_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE,oATE);
    m_pDevice->SetTextureStageState(0,D3DTSS_COLOROP,oTexCOP); m_pDevice->SetTextureStageState(0,D3DTSS_COLORARG1,oTexCA1);
    m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAOP,oTexAOP); m_pDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,oTexAA1);
    // [red-bug fix] FVF and an explicit vertex declaration share ONE device slot in
    // D3D9, and GetFVF reports the SAME code (0x252) for BOTH the engine's explicit
    // ParticleElements decl (COLOR at offset 40) and the FVF-canonical layout
    // (COLOR at offset 24) because they share a component set (XYZ|NORMAL|DIFFUSE|TEX2).
    // The old unconditional SetFVF(oFVF) therefore overwrote the just-restored explicit
    // particle decl with the FVF-canonical layout, whose offsets do NOT match the
    // 44-byte EmitterInstance::Vertex. The inheriting particle DrawIndexedPrimitiveUP
    // (EmitterInstance::Render binds no decl/FVF of its own) then read the particle
    // COLOR out of the UV0 bytes -> additive source ~= 0 -> the additive emitter
    // vanished, and it persisted until a device reset because nothing rebinds
    // m_pDeclaration. Restore whichever vertex format was actually active, never both:
    // at the shadow-pass entry this engine always has an explicit decl bound, so oDecl
    // is non-null in practice; the SetFVF branch is a fallback for a pure-FVF device
    // (where GetVertexDeclaration returns null). Guarded headless by a
    // regression check (ALO_DUMP_RSTATE decl-element dump).
    if (oDecl) { m_pDevice->SetVertexDeclaration(oDecl); oDecl->Release(); }
    else       { m_pDevice->SetFVF(oFVF); }
    m_pDevice->SetVertexShader(oVS); if (oVS) oVS->Release();
    m_pDevice->SetPixelShader(oPS);  if (oPS) oPS->Release();
    m_pDevice->SetTexture(0, oTex0); if (oTex0) oTex0->Release();
    m_pDevice->SetStreamSource(0, oStream0, oStreamOffset, oStreamStride);
    if (oStream0) oStream0->Release();
    m_pDevice->SetIndices(oIndices);
    if (oIndices) oIndices->Release();

    // [soft-shadows] Restore the extra resources the soft path detoured through:
    // RT(0) + depth-stencil + viewport (already re-bound before the composite, but
    // re-assert here for the case the composite was skipped), sampler stages 0-3
    // textures + filter/address states. Stage-0 texture is owned by the existing
    // tail above; here we restore stages 1-3 + every stage's sampler states, and
    // release every AddRef'd handle. No-op when the hard path ran.
    if (soft)
    {
        m_pDevice->SetRenderTarget(0, oRT0);
        m_pDevice->SetDepthStencilSurface(oDS);
        m_pDevice->SetViewport(&oViewport);
        for (DWORD s = 0; s < 4; ++s)
        {
            if (s != 0) m_pDevice->SetTexture(s, oTexS[s]);   // stage 0 done above
            m_pDevice->SetSamplerState(s, D3DSAMP_MINFILTER, oMinF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MAGFILTER, oMagF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_MIPFILTER, oMipF[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSU, oAddrU[s]);
            m_pDevice->SetSamplerState(s, D3DSAMP_ADDRESSV, oAddrV[s]);
            if (oTexS[s]) oTexS[s]->Release();
        }
        if (oRT0) oRT0->Release();
        if (oDS)  oDS->Release();
    }
}

// Reusable fixed-function world-space line-list draw. Uses the passed
// EmitterInstance::Vertex decl (Position + diffuse Color; the FF view/proj are
// already set this frame). Depth test ON (so a placed object occludes lines
// behind it) but depth write OFF (lines never block particle sorting). Saves +
// restores every render / texture-stage state it touches (this discipline).
// File-static (not an Engine member) so the header needn't see EmitterInstance::Vertex.
static void DrawWorldLines(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                           const EmitterInstance::Vertex* verts, int lineCount,
                           bool depthTest = true)
{
    if (dev == NULL || decl == NULL || verts == NULL || lineCount <= 0) return;

    DWORD oldZEnable, oldZWrite, oldAlphaBlend, oldLighting, oldCull;
    DWORD oldColorOp, oldColorArg1, oldAlphaOp, oldAlphaArg1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    dev->GetRenderState(D3DRS_LIGHTING,         &oldLighting);
    dev->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oldColorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldAlphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
    IDirect3DBaseTexture9* oldTex0 = NULL;
    dev->GetTexture(0, &oldTex0);   // AddRef'd; released after restore
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    dev->GetVertexDeclaration(&oldDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    // depthTest=false => always-on-top (the manipulator gizmo, so handles aren't
    // hidden inside the object); true => co-planar depth-tested (the grid).
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    // Emit the vertex diffuse colour directly (no texture bound).
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    dev->DrawPrimitiveUP(D3DPT_LINELIST, lineCount, verts, sizeof(EmitterInstance::Vertex));

    dev->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    dev->SetTexture(0, oldTex0);
    if (oldTex0) oldTex0->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oldColorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldAlphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
    dev->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    dev->SetRenderState(D3DRS_LIGHTING,         oldLighting);
    dev->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Sibling of DrawWorldLines for the ground-plane handle's translucent
// fill: identical device-state bracketing, but ALPHA-BLEND ON (vertex-alpha *
// SRCALPHA/INVSRCALPHA) and a TRIANGLELIST. depthTest=false => always-on-top like
// the rest of the gizmo. triCount = number of triangles (verts = 3*triCount).
static void DrawWorldTris(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                          const EmitterInstance::Vertex* verts, int triCount,
                          bool depthTest = false)
{
    if (dev == NULL || decl == NULL || verts == NULL || triCount <= 0) return;

    DWORD oldZEnable, oldZWrite, oldAlphaBlend, oldLighting, oldCull, oldSrc, oldDst;
    DWORD oldColorOp, oldColorArg1, oldAlphaOp, oldAlphaArg1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oldZEnable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oldZWrite);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    dev->GetRenderState(D3DRS_SRCBLEND,         &oldSrc);
    dev->GetRenderState(D3DRS_DESTBLEND,        &oldDst);
    dev->GetRenderState(D3DRS_LIGHTING,         &oldLighting);
    dev->GetRenderState(D3DRS_CULLMODE,         &oldCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oldColorOp);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oldAlphaOp);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
    IDirect3DBaseTexture9* oldTex0 = NULL;
    dev->GetTexture(0, &oldTex0);
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    dev->GetVertexDeclaration(&oldDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, triCount, verts, sizeof(EmitterInstance::Vertex));

    dev->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
    dev->SetTexture(0, oldTex0);
    if (oldTex0) oldTex0->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oldColorOp);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oldAlphaOp);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
    dev->SetRenderState(D3DRS_ZENABLE,          oldZEnable);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oldZWrite);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    dev->SetRenderState(D3DRS_SRCBLEND,         oldSrc);
    dev->SetRenderState(D3DRS_DESTBLEND,        oldDst);
    dev->SetRenderState(D3DRS_LIGHTING,         oldLighting);
    dev->SetRenderState(D3DRS_CULLMODE,         oldCull);
}

// Camera-facing thick lines with a dark outline + per-segment colour/alpha.
// Each RibbonSeg becomes a billboarded quad (2 tris) via gizmoribbon::ExpandSegment.
// Two passes inside one state bracket: a dark underlay at (hw+outline) whose alpha
// tracks each seg (so faded ring segments fade their outline too), then the colour
// at hw. Alpha-blend ON like DrawWorldTris; depthTest=false => always-on-top.
struct RibbonSeg { D3DXVECTOR3 a, b; D3DCOLOR color; };

static void DrawWorldRibbons(IDirect3DDevice9* dev, IDirect3DVertexDeclaration9* decl,
                             const RibbonSeg* segs, int n, const D3DXVECTOR3& camPos,
                             float hw, float outline, D3DCOLOR outlineRGB, bool depthTest,
                             float globalAlpha = 1.0f)
{
    if (dev == NULL || decl == NULL || segs == NULL || n <= 0) return;

    DWORD oZ,oZW,oAB,oSrc,oDst,oLit,oCull,oCop,oCa1,oAop,oAa1;
    dev->GetRenderState(D3DRS_ZENABLE,          &oZ);
    dev->GetRenderState(D3DRS_ZWRITEENABLE,     &oZW);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &oAB);
    dev->GetRenderState(D3DRS_SRCBLEND,         &oSrc);
    dev->GetRenderState(D3DRS_DESTBLEND,        &oDst);
    dev->GetRenderState(D3DRS_LIGHTING,         &oLit);
    dev->GetRenderState(D3DRS_CULLMODE,         &oCull);
    dev->GetTextureStageState(0, D3DTSS_COLOROP,   &oCop);
    dev->GetTextureStageState(0, D3DTSS_COLORARG1, &oCa1);
    dev->GetTextureStageState(0, D3DTSS_ALPHAOP,   &oAop);
    dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oAa1);
    IDirect3DBaseTexture9* oTex = NULL; dev->GetTexture(0, &oTex);
    IDirect3DVertexDeclaration9* oDecl = NULL; dev->GetVertexDeclaration(&oDecl);

    D3DXMATRIX ident; D3DXMatrixIdentity(&ident);
    dev->SetTransform(D3DTS_WORLD, &ident);
    dev->SetVertexDeclaration(decl);
    dev->SetTexture(0, NULL);
    dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
    dev->SetRenderState(D3DRS_ZENABLE,          depthTest ? D3DZB_TRUE : D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);

    auto emit = [&](float halfW, bool dark) {
        std::vector<EmitterInstance::Vertex> v; v.reserve(n * 6);
        for (int i = 0; i < n; ++i) {
            float q[4][3];
            gizmoribbon::ExpandSegment(&segs[i].a.x, &segs[i].b.x, &camPos.x, halfW, q);
            D3DCOLOR c = segs[i].color;
            // dark outline: soft grey RGB; halo alpha = seg alpha scaled by the outline
            // colour's own alpha (so the halo is gentler than the line, not a hard keyline).
            if (dark) { const BYTE A = (BYTE)((((c >> 24) & 0xFF) * ((outlineRGB >> 24) & 0xFF)) / 255);
                        c = (outlineRGB & 0x00FFFFFF) | ((DWORD)A << 24); }
            // global gizmo translucency: scale the (possibly fade/outline-set) alpha
            const BYTE ga = (BYTE)(((c >> 24) & 0xFF) * globalAlpha);
            c = (c & 0x00FFFFFF) | ((DWORD)ga << 24);
            const int tri[6] = { 0, 1, 2,  0, 2, 3 };
            for (int t = 0; t < 6; ++t) {
                EmitterInstance::Vertex vert = {};
                vert.Position = D3DXVECTOR3(q[tri[t]][0], q[tri[t]][1], q[tri[t]][2]);
                vert.Color = c;
                v.push_back(vert);
            }
        }
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, n * 2, v.data(), sizeof(EmitterInstance::Vertex));
    };
    emit(hw + outline, /*dark=*/true);   // outline underlay first
    emit(hw,           /*dark=*/false);  // colour on top

    dev->SetVertexDeclaration(oDecl);
    if (oDecl) oDecl->Release();
    dev->SetTexture(0, oTex);
    if (oTex) oTex->Release();
    dev->SetTextureStageState(0, D3DTSS_COLOROP,   oCop);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, oCa1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   oAop);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, oAa1);
    dev->SetRenderState(D3DRS_ZENABLE,          oZ);
    dev->SetRenderState(D3DRS_ZWRITEENABLE,     oZW);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, oAB);
    dev->SetRenderState(D3DRS_SRCBLEND,         oSrc);
    dev->SetRenderState(D3DRS_DESTBLEND,        oDst);
    dev->SetRenderState(D3DRS_LIGHTING,         oLit);
    dev->SetRenderState(D3DRS_CULLMODE,         oCull);
}

// Unit grid: axis-aligned world lines on the ground plane (the engine's
// first D3DPT_LINELIST primitive). Spacing = m_gridSpacing over a fixed extent,
// with a brighter major line every 5 cells from centre. Co-planar with the
// ground (lifted by a small epsilon so it does not z-fight). No-op when hidden.
void Engine::RenderUnitGrid()
{
    if (!m_gridVisible) return;

    const float spacing = m_gridSpacing;            // > 0 (SetGridSpacing clamps)
    if (spacing <= 0.0f) return;
    const float extent  = 800.0f;                   // half-size: lines span +/-extent
    const int   cells   = (int)(extent / spacing);  // lines each side of centre
    if (cells <= 0) return;
    const float span = cells * spacing;             // half-extent on a cell boundary
    const float z    = m_groundZ + 0.05f;           // tiny lift above the ground quad
    const D3DCOLOR kMinor = D3DCOLOR_RGBA( 70,  70,  85, 255);
    const D3DCOLOR kMajor = D3DCOLOR_RGBA(120, 120, 150, 255);

    std::vector<EmitterInstance::Vertex> verts;
    verts.reserve((size_t)(cells * 2 + 1) * 4);
    auto addLine = [&](float ax, float ay, float bx, float by, D3DCOLOR c)
    {
        EmitterInstance::Vertex v0 = {}, v1 = {};
        v0.Position = D3DXVECTOR3(ax, ay, z); v0.Color = c;
        v1.Position = D3DXVECTOR3(bx, by, z); v1.Color = c;
        verts.push_back(v0);
        verts.push_back(v1);
    };
    for (int i = -cells; i <= cells; ++i)
    {
        const float p = i * spacing;
        const D3DCOLOR c = (i % 5 == 0) ? kMajor : kMinor;   // major every 5 cells
        addLine(p, -span, p,  span, c);   // line parallel to Y (constant X)
        addLine(-span, p,  span, p, c);   // line parallel to X (constant Y)
    }

    DrawWorldLines(m_pDevice, m_pDeclaration, verts.data(), (int)(verts.size() / 2));
}

// Screen->world ray for the cursor. Shared with GetCursorPos3D
// (MouseCursor.h) so the manipulator pick uses the IDENTICAL unproject incl. the
// scene-viewport aspect fix. Origin = near-plane point; dir = unit toward far.
void Engine::BuildCursorRay(short screenX, short screenY,
                            D3DXVECTOR3& outOrigin, D3DXVECTOR3& outDir) const
{
    D3DVIEWPORT9 viewport;
    int sx, sy, sw, sh;
    if (GetSceneViewport(sx, sy, sw, sh))
    {
        viewport.X = (DWORD)sx; viewport.Y = (DWORD)sy;
        viewport.Width = (DWORD)sw; viewport.Height = (DWORD)sh;
        viewport.MinZ = 0.0f; viewport.MaxZ = 1.0f;
    }
    else
    {
        GetViewPort(&viewport);
    }
    D3DXMATRIX world; D3DXMatrixIdentity(&world);
    D3DXVECTOR3 front, back;
    const D3DXVECTOR3 sNear((float)screenX, (float)screenY, 0.0f);
    const D3DXVECTOR3 sFar ((float)screenX, (float)screenY, 0.9f);
    D3DXVec3Unproject(&front, &sNear, &viewport, &m_projection, &m_view, &world);
    D3DXVec3Unproject(&back,  &sFar,  &viewport, &m_projection, &m_view, &world);
    outOrigin = front;
    D3DXVECTOR3 d = back - front;
    D3DXVec3Normalize(&outDir, &d);
}

namespace
{
    // Gizmo geometry constants shared by the render and the pick so the
    // drawn handle and the grabbable region stay in lockstep. Arrow length is
    // `baseLen` (screen-uniform; Engine::ReferenceGizmoHandleLength);
    // rings sit just outside the arrowheads.
    constexpr float kAxisPickScale   = 0.18f;   // arrow pick radius = len * this
    constexpr float kHoverGrow       = 1.3f;    // hovered arrow grows (visual + pick)
    constexpr float kRingRadiusScale = 1.15f;   // ring radius   = baseLen * this
    constexpr float kRingPickBand    = 0.14f;   // ring pick band = ringRadius * this
    constexpr float kPlaneInner = 0.0f;    // ground-plane square near edge (* baseLen) -- 0 => the
                                           //         quad runs from the origin, inner edges sit on the X/Y axes
    constexpr float kPlaneOuter = 0.58f;   //          far edge -> side 0.58*baseLen, in the +X/+Y quadrant

    // aesthetics pass. Ribbon width/outline are fractions of baseLen so they
    // stay ~constant px (baseLen is screen-uniform). Palette coordinated with the
    // accent #4ea3ff; the cyan-teal box is a non-axis hue ("selection chrome").
    // kGizmoAlpha fades the whole overlay back so it reads without dominating the scene.
    constexpr float kGizmoAlpha    = 0.72f;   // global translucency on every ribbon
    constexpr float kRibbonWidth   = 0.009f;  // half-width = baseLen * this
    constexpr float kRibbonOutline = 0.010f;  // dark underlay extra half-width (kept wide; softness is in the alpha)
    constexpr float kRingBackAlpha = 0.16f;   // camera-far ring fade floor
    constexpr float kRingIdle      = 0.42f;   // rotation rings sit back at rest; full on hover/active drag
    constexpr float kBracketFrac   = 0.067f;  // corner-bracket length = boxDiagonal * this (clamped to each edge)
    const D3DCOLOR kOutlineRGB  = D3DCOLOR_RGBA(30,33,42,130);    // soft dark-grey halo; alpha = halo translucency (~0.51)
    const D3DCOLOR kSelBoxColor = D3DCOLOR_RGBA(53,210,210,255);  // #35d2d2 cyan-teal

    D3DXVECTOR3 unitAxis(int axis)
    {
        return D3DXVECTOR3(axis == 0 ? 1.0f : 0.0f,
                           axis == 1 ? 1.0f : 0.0f,
                           axis == 2 ? 1.0f : 0.0f);
    }

    // Signed distance along the unit axis `u` (anchored at A) to the point on that
    // axis closest to the ray (origin P, unit dir d). Classic two-line closest
    // point with a=c=1 (u,d unit) and w0 = A - P. False if the ray is ~parallel to
    // the axis (denom -> 0; closest point ill-defined).
    bool axisParamFromRay(const D3DXVECTOR3& P, const D3DXVECTOR3& d,
                          const D3DXVECTOR3& A, const D3DXVECTOR3& u, float& outT)
    {
        const D3DXVECTOR3 w0 = A - P;
        const float b     = D3DXVec3Dot(&u, &d);
        const float dCoef = D3DXVec3Dot(&u, &w0);
        const float eCoef = D3DXVec3Dot(&d, &w0);
        const float denom = 1.0f - b * b;   // a*c - b^2
        if (denom < 1e-5f) return false;
        outT = (b * eCoef - dCoef) / denom; // param along u from A
        return true;
    }
}

// Screen-uniform gizmo sizing (was eye-distance*0.12, "v1"). See GizmoSizing.h.
float Engine::ReferenceGizmoHandleLength() const
{
    const D3DXVECTOR3 eye = m_eye.Position;
    D3DXVECTOR3 fwd = m_eye.Target - eye;                 // view forward (matches LookAtRH)
    D3DXVec3Normalize(&fwd, &fwd);
    D3DXVECTOR3 toRef = m_referencePosition - eye;
    const float depth   = D3DXVec3Dot(&toRef, &fwd);      // VIEW-SPACE depth
    const float eyeDist = D3DXVec3Length(&toRef);
    return GizmoHandleLengthWorld(depth, m_sceneFovY, m_sceneViewportH, m_sceneViewportActive, eyeDist);
}

// Ray-pick the manipulator handle under the cursor: 3 translate arrows + 3
// rotate rings. Each candidate is scored by its miss distance divided by its own
// pick threshold (so the arrow's ray-to-segment gap and the ring's |rho-R| band are
// comparable); the smallest score < 1 wins, and an arrow wins ties (it's tested
// first with a strict `<`). Returns kind=NONE on a miss. Same visible+resolved gate
// as the render so a pick is only attempted on a clickable object.
Engine::ManipHandle Engine::PickManipulatorHandle(short screenX, short screenY) const
{
    ManipHandle hit;   // NONE / -1
    if (!m_referenceObjectSelected) return hit;   // gizmo only grabbable when selected
    if (!m_referenceObjectVisible) return hit;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return hit;
    if (m_pDevice == NULL) return hit;

    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    const D3DXVECTOR3 A = m_referencePosition;
    const float baseLen = ReferenceGizmoHandleLength();

    float bestScore = 1.0f;   // miss/threshold; a candidate must beat 1 to pick

    // --- 3 translate arrows: ray-to-segment gap, threshold = len * kAxisPickScale.
    // The hovered arrow grows by kHoverGrow, so its pick uses the grown extent too
    // (hover was set on the prior mouse-move, so the grown extent is the live one --
    // the outer/arrowhead region of the hovered axis becomes grabbable).
    for (int i = 0; i < 3; ++i)
    {
        const bool  hot = (m_hoverManip.kind == ManipHandle::TRANSLATE && m_hoverManip.axis == i);
        const float len = hot ? baseLen * kHoverGrow : baseLen;
        const float threshold = len * kAxisPickScale;
        const D3DXVECTOR3 u = unitAxis(i);
        D3DXVECTOR3 axisPt;
        float t;
        if (axisParamFromRay(P, d, A, u, t))
        {
            if (t < 0.0f)      t = 0.0f;
            else if (t > len)  t = len;     // clamp to the handle segment [0,len]
            axisPt = A + u * t;
        }
        else
        {
            axisPt = A;                     // ray ~parallel: test the anchor
        }
        // Closest point on the ray to axisPt, then the gap between them.
        D3DXVECTOR3 toAxis = axisPt - P;
        float tc = D3DXVec3Dot(&toAxis, &d);
        if (tc < 0.0f) tc = 0.0f;
        D3DXVECTOR3 rayPt = P + d * tc;
        D3DXVECTOR3 gap   = axisPt - rayPt;
        const float score = D3DXVec3Length(&gap) / threshold;
        if (score < bestScore) { bestScore = score; hit.kind = ManipHandle::TRANSLATE; hit.axis = i; }
    }

    // --- 3 rotate rings: intersect the ray with the ring's world-axis plane, then
    // score |in-plane radius - R| against the band. A ray grazing the plane
    // (denom ~0) or hitting it behind the camera is skipped.
    const float R    = baseLen * kRingRadiusScale;
    const float band = R * kRingPickBand;
    for (int i = 0; i < 3; ++i)
    {
        const D3DXVECTOR3 n = unitAxis(i);
        const float denom = D3DXVec3Dot(&d, &n);
        if (fabsf(denom) < 1e-4f) continue;          // grazing: edge-on ring, skip
        D3DXVECTOR3 oToP = A - P;
        const float t = D3DXVec3Dot(&oToP, &n) / denom;
        if (t < 0.0f) continue;                       // plane behind the ray
        const D3DXVECTOR3 H = P + d * t;              // hit point (lies in the plane)
        D3DXVECTOR3 g = H - A;
        const float rho   = D3DXVec3Length(&g);
        const float score = fabsf(rho - R) / band;
        if (score < bestScore) { bestScore = score; hit.kind = ManipHandle::ROTATE; hit.axis = i; }
    }

    // --- ground-plane (XY) handle: STRICT FALLBACK. Considered only when no arrow or
    // ring was picked above (hit.kind still NONE). The square sits in the +X/+Y
    // diagonal, spatially clear of the on-axis arrows and the 1.15*baseLen rings, so a
    // genuine square click never puts an axis candidate within threshold -- which is
    // exactly why "axes always win, the plane catches the rest" can neither steal an
    // axis click nor be stolen from. No score contest: inside the square == picked.
    // (A score-based contest mis-fires because the pick ray pierces the INFINITE
    // ground plane: an oblique click on an arrow tip / ring arc can land inside the
    // square footprint and a centred hit would win wrongly. Fallback avoids that.)
    if (hit.kind == ManipHandle::NONE)
    {
        float pu, pv, pscore;
        // hit-test against the CURRENT object position (A) -- no drag in progress here.
        if (ManipulatorPlaneOffset(screenX, screenY, /*normalAxis=*/2, A, pu, pv) &&
            planehandle::HandleHit(pu, pv, baseLen * kPlaneInner, baseLen * kPlaneOuter, pscore))
        {
            hit.kind = ManipHandle::PLANE; hit.axis = 2;   // pscore unused for selection (sole candidate)
        }
    }

    return hit;
}

// Signed distance along `axis` from `anchor` to the cursor ray's
// closest point. The host snapshots this at grab time, then accumulates
// precision-scaled per-move deltas of this param (sum of (now - prevMove) * factor)
// and applies new position = anchor + axis*accumulated. (With factor==1 the sum
// telescopes to (now - grab); a Shift-held factor<1 only rescales later deltas.)
// False when degenerate.
bool Engine::ManipulatorAxisParam(short screenX, short screenY, int axis,
                                  const D3DXVECTOR3& anchor, float& outParam) const
{
    if (axis < 0 || axis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);
    return axisParamFromRay(P, d, anchor, unitAxis(axis), outParam);
}

// The cursor's angle (radians) around rotate ring `axis`, measured in that
// ring's world-axis plane (centred at the object origin). The plane normal is the
// world axis; the in-plane basis (a,b) is chosen so increasing angle matches the
// positive Euler rotation that axis drives: ring Z=yaw basis (X,Y), ring X=pitch
// basis (Y,Z), ring Y=roll basis (Z,X) -- i.e. a=axis+1, b=axis+2 (mod 3). The
// host snapshots this at grab and accumulates wrapped per-move deltas (no-jump,
// multi-turn). False when the ray grazes the plane or hits it behind the camera.
bool Engine::ManipulatorRingAngle(short screenX, short screenY, int axis,
                                  float& outAngleRad) const
{
    if (axis < 0 || axis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    const D3DXVECTOR3 n = unitAxis(axis);
    const float denom = D3DXVec3Dot(&d, &n);
    if (fabsf(denom) < 1e-4f) return false;          // ray ~parallel to the plane
    const D3DXVECTOR3 o = m_referencePosition;
    D3DXVECTOR3 oToP = o - P;
    const float t = D3DXVec3Dot(&oToP, &n) / denom;
    if (t < 0.0f) return false;                       // plane behind the ray
    const D3DXVECTOR3 H = P + d * t;
    D3DXVECTOR3 g = H - o;                             // in-plane vector from centre
    const D3DXVECTOR3 a = unitAxis((axis + 1) % 3);
    const D3DXVECTOR3 b = unitAxis((axis + 2) % 3);
    outAngleRad = atan2f(D3DXVec3Dot(&g, &b), D3DXVec3Dot(&g, &a));
    return true;
}

// See header. BuildCursorRay (engine-space ray) then the pure
// planehandle::RayPlaneOffset against the EXPLICIT `anchor` (NOT m_referencePosition --
// a drag must anchor to the fixed grab position, else the moving origin flickers).
// D3DXVECTOR3 is {float x,y,z} so &v.x is a float[3].
bool Engine::ManipulatorPlaneOffset(short screenX, short screenY, int normalAxis,
                                    const D3DXVECTOR3& anchor, float& outU, float& outV) const
{
    if (normalAxis < 0 || normalAxis > 2) return false;
    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);
    return planehandle::RayPlaneOffset(&P.x, &d.x, &anchor.x,
                                       normalAxis, outU, outV);
}

// Draw the combined manipulator (X=red/Y=green/Z=blue), ALWAYS-ON-TOP
// (depth-test off) so it is never hidden inside the object. Per axis: a translate
// arrow (shaft + 4-sided head) from the object origin, plus a rotate ring (a
// world-axis circle just outside the arrowheads). The hovered handle brightens;
// a hovered ARROW also grows (its pick uses the grown extent); a hovered RING
// brightens only (no radius pop). Only shown when the object is selected.
void Engine::RenderReferenceManipulator()
{
    if (!m_referenceObjectSelected) return;
    if (!m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;

    const D3DXVECTOR3 o = m_displayPosition;   // eased origin so the gizmo glides with the object
    const float baseLen = ReferenceGizmoHandleLength();
    const bool dragging = (m_activeManip.kind != ManipHandle::NONE);
    const D3DCOLOR axisCol[3]  = { D3DCOLOR_RGBA(255,  91,  91, 255),   // X #ff5b5b
                                   D3DCOLOR_RGBA( 73, 227,  95, 255),   // Y #49e35f
                                   D3DCOLOR_RGBA( 78, 163, 255, 255) }; // Z #4ea3ff (accent)
    const D3DCOLOR hoverCol[3] = { D3DCOLOR_RGBA(255, 150, 150, 255),
                                   D3DCOLOR_RGBA(150, 255, 170, 255),
                                   D3DCOLOR_RGBA(150, 200, 255, 255) };
    const D3DXVECTOR3 camPos = m_eye.Position;          // for ribbon billboarding + ring fade
    const float ribHalf = baseLen * kRibbonWidth;
    const float ribOut  = baseLen * kRibbonOutline;
    std::vector<RibbonSeg> rib;                          // accumulate all always-on-top ribbons
    auto rseg = [&](const D3DXVECTOR3& a, const D3DXVECTOR3& b, D3DCOLOR c){ rib.push_back({a,b,c}); };

    constexpr int kRingSegs = 48;
    std::vector<EmitterInstance::Vertex> v;
    v.reserve((4 + 2) * 2);   // up to 2 drag-guide lines (PLANE); arrows/rings/sweep radials/plane-border are ribbons
    auto line = [&](const D3DXVECTOR3& a, const D3DXVECTOR3& b, D3DCOLOR c)
    {
        EmitterInstance::Vertex v0 = {}, v1 = {};
        v0.Position = a; v0.Color = c;
        v1.Position = b; v1.Color = c;
        v.push_back(v0);
        v.push_back(v1);
    };
    // During a drag, FADE the non-active handles via alpha (hue kept) so
    // they ghost out softly instead of darkening toward black. The alpha-blended
    // ribbon/tri paths make alpha the right lever now (the old RGB*0.4 darkened).
    auto dim = [&](D3DCOLOR c) -> D3DCOLOR {
        const BYTE A=(BYTE)(((c>>24)&0xFF)*0.30f);
        return (c & 0x00FFFFFF) | ((DWORD)A<<24);
    };

    // Translate arrows.
    for (int i = 0; i < 3; ++i)
    {
        const bool hot = (m_hoverManip.kind == ManipHandle::TRANSLATE && m_hoverManip.axis == i);
        const float    len = hot ? baseLen * kHoverGrow : baseLen;   // hovered arrow grows
        D3DCOLOR c         = hot ? hoverCol[i] : axisCol[i];         // ... and brightens
        const bool isActive = (m_activeManip.kind == ManipHandle::TRANSLATE && m_activeManip.axis == i);
        if (dragging && !isActive) c = dim(c);
        const D3DXVECTOR3 dir = unitAxis(i);
        const D3DXVECTOR3 p1  = unitAxis((i + 1) % 3);   // perpendiculars for the head
        const D3DXVECTOR3 p2  = unitAxis((i + 2) % 3);
        const D3DXVECTOR3 tip  = o + dir * len;
        const D3DXVECTOR3 base = o + dir * (len * 0.82f);
        const float r = len * 0.06f;
        rseg(o, tip, c);                 // shaft
        rseg(tip, base + p1 * r, c);     // 4-sided arrowhead
        rseg(tip, base - p1 * r, c);
        rseg(tip, base + p2 * r, c);
        rseg(tip, base - p2 * r, c);
    }

    {   // faint neutral reference ring (ground plane), behind the rotate arcs
        const D3DCOLOR refc = D3DCOLOR_RGBA(200,205,210, 80);
        const float ringR = baseLen * kRingRadiusScale;
        const D3DXVECTOR3 ax = unitAxis(0), ay = unitAxis(1);
        D3DXVECTOR3 prevp = o + ax * ringR;
        for (int s = 1; s <= kRingSegs; ++s)
        {
            const float a = (2.0f * D3DX_PI) * s / kRingSegs;
            const D3DXVECTOR3 cur = o + (ax * cosf(a) + ay * sinf(a)) * ringR;
            rseg(prevp, cur, refc);
            prevp = cur;
        }
    }

    // Rotate rings: a closed world-axis circle of radius R in the plane perpendicular
    // to each axis, built from the same (a,b) in-plane basis the angle pick uses.
    // Each segment goes through rseg with camera-facing alpha fade so the
    // back-facing half of each ring fades to kRingBackAlpha instead of full opacity.
    const float R = baseLen * kRingRadiusScale;
    for (int i = 0; i < 3; ++i)
    {
        const bool hot = (m_hoverManip.kind == ManipHandle::ROTATE && m_hoverManip.axis == i);
        D3DCOLOR base = hot ? hoverCol[i] : axisCol[i];
        const bool isActive = (m_activeManip.kind == ManipHandle::ROTATE && m_activeManip.axis == i);
        if (dragging && !isActive) base = dim(base);
        // idle rings sit back; the hovered (or actively-dragged) ring comes
        // forward to full clarity. Arrows/plane keep full presence -- rings only.
        const float idleK = (!dragging && !hot) ? kRingIdle : 1.0f;
        const D3DXVECTOR3 a = unitAxis((i+1)%3), b = unitAxis((i+2)%3);
        D3DXVECTOR3 prev = o + a * R;
        for (int s = 1; s <= kRingSegs; ++s)
        {
            const float ang = (2.0f*D3DX_PI)*(float)s/(float)kRingSegs;
            const D3DXVECTOR3 cur = o + (a*cosf(ang)+b*sinf(ang))*R;
            const D3DXVECTOR3 midp = (prev+cur)*0.5f;
            const float fa = ringfade::FacingAlpha(&midp.x, &o.x, &camPos.x, kRingBackAlpha);
            const BYTE A=(BYTE)(((base>>24)&0xFF)*fa*idleK);
            rseg(prev, cur, (base & 0x00FFFFFF) | ((DWORD)A<<24));
            prev = cur;
        }
    }

    // Ground-plane (XY) handle: filled translucent quad (alpha-blended,
    // its own draw call) + a ribbon border (4 segments via rseg, drawn in the always-on-top ribbon pass).
    // Sits in the +X/+Y quadrant, [kPlaneInner,kPlaneOuter]*baseLen on each axis.
    {
        const int planeN = 2;   // normal = world Z (ground); only this plane ships now
        const bool hot      = (m_hoverManip.kind  == ManipHandle::PLANE && m_hoverManip.axis  == planeN);
        const bool isActive = (m_activeManip.kind == ManipHandle::PLANE && m_activeManip.axis == planeN);
        const float inL = baseLen * kPlaneInner, outL = baseLen * kPlaneOuter;
        float qc[4][3];
        planehandle::QuadCorners(&o.x, planeN, inL, outL, qc);   // the unit-tested placement math
        const D3DXVECTOR3 c00(qc[0][0], qc[0][1], qc[0][2]);     // (in,in)
        const D3DXVECTOR3 c10(qc[1][0], qc[1][1], qc[1][2]);     // (out,in)
        const D3DXVECTOR3 c11(qc[2][0], qc[2][1], qc[2][2]);     // (out,out)
        const D3DXVECTOR3 c01(qc[3][0], qc[3][1], qc[3][2]);     // (in,out)
        const BYTE fillA = hot ? 75 : 40;                               // softer now the quad reaches the origin
        D3DCOLOR fill   = D3DCOLOR_RGBA(120, 200, 255, fillA);          // cool translucent blue (Z-normal family)
        D3DCOLOR border = hot ? D3DCOLOR_RGBA(170, 220, 255, 255)
                              : D3DCOLOR_RGBA(120, 200, 255, 255);
        if (dragging && !isActive) { fill = dim(fill); border = dim(border); }
        // Fill: 2 triangles in their own buffer, alpha-blended, always-on-top.
        EmitterInstance::Vertex q[6];
        const D3DXVECTOR3 tri[6] = { c00, c10, c11,  c00, c11, c01 };
        for (int i = 0; i < 6; ++i) { q[i] = EmitterInstance::Vertex{}; q[i].Position = tri[i]; q[i].Color = fill; }
        DrawWorldTris(m_pDevice, m_pDeclaration, q, 2, /*depthTest=*/false);
        // Border: 4 ribbon segments (routed through rseg so they match arrow/ring stroke width).
        rseg(c00, c10, border); rseg(c10, c11, border); rseg(c11, c01, border); rseg(c01, c00, border);
    }

    // Active-drag guides. faint() dims the RGB (lines draw alpha-blend
    // OFF, so alpha is a no-op -- scale the colour, as dim() does). TRANSLATE: one
    // faint axis line. PLANE: both in-plane (X,Y) axis lines, faint. ROTATE sweep
    // unchanged (full colour).
    constexpr float kGuideExtent = 700.0f;   // readability, not a clip bound (infinite far plane)
    auto faint = [](D3DCOLOR c, float k) -> D3DCOLOR {
        const BYTE A=(c>>24)&0xFF, R=(BYTE)(((c>>16)&0xFF)*k), G=(BYTE)(((c>>8)&0xFF)*k), B=(BYTE)((c&0xFF)*k);
        return D3DCOLOR_RGBA(R,G,B,A);
    };
    if (dragging && m_activeManip.kind == ManipHandle::TRANSLATE) {
        const D3DXVECTOR3 dir = unitAxis(m_activeManip.axis);
        line(o - dir * kGuideExtent, o + dir * kGuideExtent, faint(axisCol[m_activeManip.axis], 0.55f));
    }
    else if (dragging && m_activeManip.kind == ManipHandle::PLANE) {
        const int n = m_activeManip.axis;                       // normal axis (2 = ground)
        for (int k = 1; k <= 2; ++k) {                          // the two in-plane axes
            const int ax = (n + k) % 3;
            const D3DXVECTOR3 dir = unitAxis(ax);
            line(o - dir * kGuideExtent, o + dir * kGuideExtent, faint(axisCol[ax], 0.55f));
        }
    }
    else if (dragging && m_activeManip.kind == ManipHandle::ROTATE) {
        const int ax = m_activeManip.axis;
        const D3DXVECTOR3 a = unitAxis((ax + 1) % 3);
        const D3DXVECTOR3 b = unitAxis((ax + 2) % 3);
        // translucent "pie slice" of the swept angle (grab -> applied): a
        // triangle fan from the origin, alpha-blended (DrawWorldTris) UNDER the radial
        // lines, which flush later via the ribbon batch.
        const float delta = m_activeAppliedAngle - m_activeGrabAngle;
        int segn = (int)(fabsf(delta) / (D3DX_PI / 90.0f)); if (segn < 1) segn = 1;   // ~2deg steps
        const float Rf = R * 0.92f;
        const D3DCOLOR pie = (axisCol[ax] & 0x00FFFFFF) | ((DWORD)(BYTE)(70.0f * kGizmoAlpha) << 24);
        std::vector<EmitterInstance::Vertex> fan; fan.reserve(segn * 3);
        for (int s = 0; s < segn; ++s) {
            const float t0 = m_activeGrabAngle + delta * (float)s / (float)segn;
            const float t1 = m_activeGrabAngle + delta * (float)(s + 1) / (float)segn;
            const D3DXVECTOR3 p0 = o + (a*cosf(t0) + b*sinf(t0)) * Rf;
            const D3DXVECTOR3 p1 = o + (a*cosf(t1) + b*sinf(t1)) * Rf;
            EmitterInstance::Vertex v0 = {}, v1 = {}, v2 = {};
            v0.Position = o;  v0.Color = pie;
            v1.Position = p0; v1.Color = pie;
            v2.Position = p1; v2.Color = pie;
            fan.push_back(v0); fan.push_back(v1); fan.push_back(v2);
        }
        if (!fan.empty())
            DrawWorldTris(m_pDevice, m_pDeclaration, fan.data(), (int)fan.size() / 3, /*depthTest=*/false);
        auto radial = [&](float ang){ rseg(o, o + (a*cosf(ang)+b*sinf(ang))*R, axisCol[ax]); };
        radial(m_activeGrabAngle);
        radial(m_activeAppliedAngle);
    }

    if (!rib.empty())
        DrawWorldRibbons(m_pDevice, m_pDeclaration, rib.data(), (int)rib.size(),
                         camPos, ribHalf, ribOut, kOutlineRGB, /*depthTest=*/false, kGizmoAlpha);

    DrawWorldLines(m_pDevice, m_pDeclaration, v.data(), (int)(v.size() / 2), /*depthTest=*/false);
}

// Selection box: the object's object-space AABB (over the kept/drawn
// geometry) transformed by the live world, drawn as dashed edges plus bright corner
// brackets via the camera-facing ribbon renderer (DrawWorldRibbons), depth-tested.
// Depth-tested (it's part of the scene -- the object's near
// faces occlude the far box edges), drawn before the always-on-top gizmo. Shown
// only when the object is selected. The SAME AABB + world drive PickReferenceObject
// below, so the same AABB drives the pick region.
void Engine::RenderReferenceSelectionBox()
{
    if (!m_referenceObjectSelected) return;
    if (!m_referenceObjectVisible) return;
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return;
    D3DXVECTOR3 mn, mx;
    if (!m_referenceObjectMesh.GetBoundingBox(mn, mx)) return;

    const D3DXMATRIX world = ReferenceObjectDisplayWorld();   // eased (render); pick uses committed
    D3DXVECTOR3 c[8];   // corner i: bit0=x, bit1=y, bit2=z (min/max)
    for (int i = 0; i < 8; ++i)
    {
        D3DXVECTOR3 o((i & 1) ? mx.x : mn.x,
                      (i & 2) ? mx.y : mn.y,
                      (i & 4) ? mx.z : mn.z);
        D3DXVec3TransformCoord(&c[i], &o, &world);
    }
    static const int edges[12][2] = {
        {0,1},{1,3},{3,2},{2,0},   // min-z face loop
        {4,5},{5,7},{7,6},{6,4},   // max-z face loop
        {0,4},{1,5},{2,6},{3,7},   // verticals
    };
    const D3DXVECTOR3 camPos = m_eye.Position;
    const float ribHalf = ReferenceGizmoHandleLength() * kRibbonWidth;
    const float ribOut  = ReferenceGizmoHandleLength() * kRibbonOutline;
    // box scale for dash/bracket sizing: the AABB diagonal (corner 0 -> corner 7)
    D3DXVECTOR3 diag = c[7] - c[0]; const float dlen = D3DXVec3Length(&diag);
    std::vector<RibbonSeg> rib;

    // faint SOLID edges (full box outline at the faded opacity the dashes used)
    const D3DCOLOR edgec = D3DCOLOR_RGBA(53,210,210, 70);
    for (int e=0;e<12;++e)
        rib.push_back({ c[edges[e][0]], c[edges[e][1]], edgec });

    // bright corner brackets: each corner's 3 neighbours differ in exactly one bit
    for (int i=0;i<8;++i) {
        D3DXVECTOR3 nb[3]; int k=0;
        for (int bit=0;bit<3;++bit) nb[k++] = c[i ^ (1<<bit)];
        std::vector<selboxstyle::Seg> br;
        selboxstyle::CornerBracketSegs(&c[i].x, &nb[0].x, &nb[1].x, &nb[2].x, dlen*kBracketFrac, br);
        for (size_t s=0;s<br.size();++s)
            rib.push_back({ D3DXVECTOR3(br[s].a[0],br[s].a[1],br[s].a[2]),
                            D3DXVECTOR3(br[s].b[0],br[s].b[1],br[s].b[2]), kSelBoxColor });
    }

    if (!rib.empty())
        DrawWorldRibbons(m_pDevice, m_pDeclaration, rib.data(), (int)rib.size(),
                         camPos, ribHalf, ribOut, kOutlineRGB, /*depthTest=*/true, kGizmoAlpha);
}

// Body pick for click-to-select: ray vs the object's object-space AABB (the
// same box RenderReferenceSelectionBox draws), so the visual box == the clickable
// region. Ray is transformed into object space (inverse world) then slab-tested.
// Not gated on selection (you click an unselected object to select it), but DOES
// gate on visibility (a hidden object draws nothing, so it must not be clickable --
// matches the render/box/gizmo gates). False when no object / hidden / no device /
// no bounds.
bool Engine::PickReferenceObject(short screenX, short screenY) const
{
    if (!m_referenceObjectVisible) return false;   // hidden -> nothing drawn -> not pickable
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved()) return false;
    if (m_pDevice == NULL) return false;
    D3DXVECTOR3 bmin, bmax;
    if (!m_referenceObjectMesh.GetBoundingBox(bmin, bmax)) return false;

    D3DXVECTOR3 P, d;
    BuildCursorRay(screenX, screenY, P, d);

    // Ray into object space (inverse world). The direction is transformed as a
    // vector (not renormalized) so the slab params stay consistent with the box.
    D3DXMATRIX world = ReferenceObjectWorld(), invWorld;
    D3DXMatrixInverse(&invWorld, NULL, &world);
    D3DXVECTOR3 Po, Do;
    D3DXVec3TransformCoord(&Po, &P, &invWorld);
    D3DXVec3TransformNormal(&Do, &d, &invWorld);

    // Slab method. Components near zero are parallel to that slab -> only a hit if
    // the origin is already inside the slab.
    const float origin[3] = { Po.x, Po.y, Po.z };
    const float dir[3]    = { Do.x, Do.y, Do.z };
    const float lo[3]     = { bmin.x, bmin.y, bmin.z };
    const float hi[3]     = { bmax.x, bmax.y, bmax.z };
    float tmin = -1e30f, tmax = 1e30f;
    for (int i = 0; i < 3; ++i)
    {
        if (fabsf(dir[i]) < 1e-8f)
        {
            if (origin[i] < lo[i] || origin[i] > hi[i]) return false;
        }
        else
        {
            const float inv = 1.0f / dir[i];
            float t1 = (lo[i] - origin[i]) * inv;
            float t2 = (hi[i] - origin[i]) * inv;
            if (t1 > t2) { const float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    return tmax >= 0.0f;   // box is ahead of (or around) the eye
}

// Re-drive Load->Resolve->CreateBuffers for both slots from the current
// selected Names. Used on selection change and mod-switch (ReloadTextures, after
// ShaderManager::Clear ran, so getShader picks up the new mod's .fxo). Resolve +
// CreateBuffers no-op until the device is valid; Load (CPU) runs regardless.
void Engine::RebuildSkydomeMeshes()
{
    MapEnvironment env;
    ResolveMapEnvironment(EnsureSkydomeLists(), m_skydomeContext,
                          m_skydomePrimaryName, m_skydomeSecondaryName, env);

    SkydomeMesh* meshes[2]    = { &m_skydomePrimaryMesh, &m_skydomeSecondaryMesh };
    const SkydomeRef* refs[2]  = { &env.primary, &env.secondary };
    const bool has[2]         = { env.hasPrimary, env.hasSecondary };
    // The chosen Name decides None-vs-LoadFailed: an empty Name is simply
    // "no dome", a non-empty Name that doesn't resolve/load is a failure the
    // picker must surface (it otherwise silently falls back to solid colour).
    const std::string* names[2] = { &m_skydomePrimaryName, &m_skydomeSecondaryName };
    SkydomeSlotStatus* status[2] = { &m_skydomePrimaryStatus, &m_skydomeSecondaryStatus };

    for (int i = 0; i < 2; ++i)
    {
        if (names[i]->empty())
        {
            meshes[i]->Clear();   // deselected slot -> empty (no FileManager probe)
            *status[i] = SkydomeSlotStatus::None;
            continue;
        }
        if (!has[i] || refs[i]->modelPath.empty())
        {
            // Name chosen but the *Skydomes.xml lookup yielded no model path
            // (absent from this context's list / unreadable) -> load failure.
            meshes[i]->Clear();
            *status[i] = SkydomeSlotStatus::LoadFailed;
            continue;
        }
        const std::string aloPath = "Data\\Art\\Models\\" + refs[i]->modelPath;
        if (!meshes[i]->Load(m_fileManager, aloPath))   // Load() Clear()s on failure
        {
            *status[i] = SkydomeSlotStatus::LoadFailed;
            continue;
        }
        meshes[i]->SetScaleFactor(refs[i]->scaleFactor);
        if (m_pDevice != NULL)
        {
            // Mirror SetReferenceObject: Resolve() returns false only when NO
            // sub-mesh resolved a shader, so HasResolved() stays false and
            // RenderSkydomes gates the dome out (it falls back to the simple
            // background). Report that as LoadFailed, not a silent Ok that
            // renders nothing. Pre-device (m_pDevice == NULL) Load success alone
            // is Ok; the device-up rebuild path re-runs this and downgrades if
            // the shaders then fail to resolve.
            if (!meshes[i]->Resolve(m_shaderManager, m_pDevice))
            {
                meshes[i]->Clear();
                *status[i] = SkydomeSlotStatus::LoadFailed;
                continue;
            }
            meshes[i]->CreateBuffers(m_pDevice, m_fileManager);
        }
        *status[i] = SkydomeSlotStatus::Ok;
    }
}

// Public selection entry (bridge + startup restore). Stores the choice
// and rebuilds both meshes immediately.
void Engine::SetSkydomeEnvironment(SkydomeContext context,
                                   const std::string& primaryName,
                                   const std::string& secondaryName)
{
    m_skydomeContext       = context;
    m_skydomePrimaryName   = primaryName;
    m_skydomeSecondaryName = secondaryName;
    RebuildSkydomeMeshes();
}

// Enumerate the primary + secondary dome Names for a battle context from
// the game/mod's skydome lists (device-free; safe before the device exists). Reads
// the EnsureSkydomeLists() cache so a picker-open right after a mod switch costs no
// extra GameObjectFiles scan.
void Engine::EnumerateSkydomeNames(SkydomeContext context,
                                   std::vector<std::string>& outPrimary,
                                   std::vector<std::string>& outSecondary)
{
    outPrimary.clear();
    outSecondary.clear();
    const int primAxis = (context == SkydomeContext::Land) ? (int)SkydomeAxis::LandPrimary   : (int)SkydomeAxis::SpacePrimary;
    const int secAxis  = (context == SkydomeContext::Land) ? (int)SkydomeAxis::LandSecondary : (int)SkydomeAxis::SpaceSecondary;

    const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& lists = EnsureSkydomeLists();
    for (const SkydomeRef& r : lists[primAxis]) outPrimary.push_back(r.name);
    for (const SkydomeRef& r : lists[secAxis])  outSecondary.push_back(r.name);
}

// Build (one LoadAllSkydomeLists pass) and cache the four axis skydome lists,
// rebuilding only when the FileManager's mod/submod context has changed since the
// cache was last built. This collapses the ~4 GameObjectFiles scans per mod switch
// (RebuildSkydomeMeshes' two axes + the picker query's two) into a single pass.
// Context is re-checked every call, so it's correct regardless of which consumer runs
// first after a switch (RebuildSkydomeMeshes runs before ReloadTextures' catalog
// invalidation, so a flag set there alone would be one switch stale).
const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& Engine::EnsureSkydomeLists()
{
    // Key the cache on the full content-root stack (was mod + submods).
    const std::vector<std::wstring>& roots = m_fileManager.GetContentRoots();
    if (!m_skydomeListsValid || roots != m_skydomeListsCtxRoots)
    {
        LoadAllSkydomeLists(m_fileManager, m_skydomeLists);
        m_skydomeListsCtxRoots = roots;
        m_skydomeListsValid    = true;
    }
    return m_skydomeLists;
}

// The host calls this once at startup to ARM the eager reference-object
// catalog prefetch: set the persistent m_catalogWanted latch and the next
// Update()->StartCatalogBuildIfNeeded() kicks a background build immediately --
// so the catalog is likely ready before the user ever opens the picker, and
// every later mod/submod switch rebuilds eagerly (the switch invalidates the
// catalog, the latch stays set, Update() re-kicks) with no picker-open needed.
//
// Ordering invariant (HostWindow): ModManager::RestoreLastLayerStack() runs in the
// host impl ctor and applies the saved layer stack to the FileManager
// SYNCHRONOUSLY, BEFORE the host calls ArmCatalogPrefetch(). So the first build
// this arms snapshots the FileManager already reflecting the then-selected mod +
// submods -- it prioritizes the active content, not a wasted base-game pass. If a
// future refactor moves the mod restore AFTER this call, the startup build would
// target base game and a later ReloadTextures would rebuild for the mod (correct,
// just one wasted pass) -- keep restore before ArmCatalogPrefetch().
void Engine::ArmCatalogPrefetch()
{
    m_catalogWanted = true;
}

// [reference-model-shadows] Synchronous catalog build for headless --capture-ref.
// Unlike StartCatalogBuildIfNeeded (which builds on a worker with an ISOLATED
// FileManager to avoid freezing the UI thread / racing its MEG handles), a
// one-shot headless run has no UI thread and no concurrent FileManager access,
// so we build directly against m_fileManager on the calling thread. No-op once
// built. Lets SetReferenceObject resolve INLINE rather than deferring to a later
// Update() that the one-shot run exits before reaching.
void Engine::BuildCatalogSync()
{
    if (m_referenceCatalogBuilt) return;
    BuildGameObjectCatalog(m_fileManager, m_referenceCatalog);
    m_referenceCatalogBuilt = true;
    m_catalogWanted         = false;
}

// Kick a background catalog (re)build when one is wanted and not already
// built or in flight. BuildGameObjectCatalog parses every object XML the active
// content exposes (O(content)); on a big mod that froze the whole window when run
// synchronously on the WebView2 UI thread. Snapshot the FileManager's content roots
// on THIS (UI) thread, then build on a worker with an ISOLATED FileManager (its own
// MEG handles -> no seek-race against the UI thread's FileManager). Update() harvests
// the finished catalog. Safe to call every frame; early-returns when built/building.
void Engine::StartCatalogBuildIfNeeded()
{
    if (!m_catalogWanted || m_referenceCatalogBuilt || m_catalogBuilding.load())
        return;

    const std::vector<std::wstring> basepaths = m_fileManager.GetBasepaths();
    const std::vector<std::wstring> roots     = m_fileManager.GetContentRoots();
    const uint64_t                  gen       = m_catalogGeneration;

    // Record the content-root stack this build reflects, so a later
    // texture-only ReloadTextures (F5 / file open) sees an unchanged context and
    // does NOT invalidate.
    m_catalogContextRoots = roots;

    if (m_catalogThread.joinable()) m_catalogThread.join();   // a prior build, already harvested
    m_catalogBuilding.store(true);
    m_catalogThread = std::thread([this, basepaths, roots, gen]()
    {
        auto cat = std::make_unique<GameObjectCatalog>();
        try
        {
            FileManager isoFm(basepaths);          // own MEG handles; ctor throws on empty MEGs
            isoFm.SetLayers(roots);                // replicate the FULL content-root stack
            BuildGameObjectCatalog(isoFm, *cat);   // O(content) XML parse, OFF the UI thread
        }
        catch (...)
        {
            // Isolated-FM construction / parse failed -> publish an empty catalog
            // (the picker shows an empty list, never a crash in the worker).
        }
        {
            std::lock_guard<std::mutex> lock(m_catalogMutex);
            m_pendingCatalog    = std::move(cat);
            m_pendingCatalogGen = gen;
        }
        m_catalogBuilding.store(false);
    });
}

// Host calls this right after Update(): true once when a finished catalog was
// just swapped in, so the host fires engine/state/changed (the picker re-queries).
// Only ever set when ArmCatalogPrefetch() ran (a worker built a catalog); a harmless
// one-shot bool even if unconsumed (only re-set to true on the next install -- no
// accumulation).
bool Engine::ConsumeCatalogReadyFlag()
{
    if (!m_catalogJustReady) return false;
    m_catalogJustReady = false;
    return true;
}

// Enumerate selectable game objects (Name + category) for the picker.
// Filtered to FIELDABLE units + structures via IsPickerListed (profile role !=
// Excluded && fieldable; heroes exempt) -- backdrops / props / projectiles / templates /
// non-fieldable variants are dropped so the list isn't thousands of entries. The catalog
// itself still holds every object (so a hardpoint / variant lookup against the full set is
// unaffected); only the picker payload is trimmed.
void Engine::EnumerateReferenceObjects(std::vector<GameObjectRef>& out)
{
    // Mark the catalog wanted; the actual (re)build is launched ONLY by Update()
    // -- AFTER it harvests any finished build -- so a build is never started outside the
    // harvest path (which would let a second worker spawn and the next harvest's join()
    // block the UI thread on it, reintroducing the freeze). Return whatever's ready:
    // while still building, out is empty + IsReferenceCatalogReady() is false, so the
    // bridge reports "building" and the picker shows "Loading objects…".
    m_catalogWanted = true;
    out.clear();
    for (const GameObjectRef& r : m_referenceCatalog.objects)
        if (IsPickerListed(r))   // profile role != Excluded && fieldable (heroes exempt)
            out.push_back(r);
}

// Select a reference object by its in-game Name; clears it when empty.
// A fresh selection is shown by default (reset visibility) so a previously-hidden
// object doesn't make a newly-picked one silently invisible.
void Engine::SetReferenceObject(const std::string& name)
{
    // Per-object transform memory. The reference transform (m_referencePosition /
    // m_referenceRotation) is otherwise ONE global state that leaks across object
    // swaps: a Z set while framing object A (e.g. lifting a space unit) stays put and
    // floats a land unit B picked after it. Here, on a real swap to a DIFFERENT
    // object, stash the outgoing object's transform under its name and load the
    // incoming object's remembered transform (origin if it was never moved), so each
    // object keeps its own placement and a freshly-picked unit starts grounded.
    //
    // Edge handling (see the unit test tests/test_reference_transform_memory.cpp):
    //  * Initial load ("" -> A), incl. the startup restore which sets the transform
    //    THEN the name: no memory for A and no outgoing object -> KEEP the current
    //    (restored) transform rather than zeroing it.
    //  * Leaving to None (A -> ""): remember A, then clear the live transform so a
    //    later None -> X can't inherit A's Z.
    //  * Re-select of the SAME object: no-op.
    //
    // The outgoing key is the DESIRED name, NOT the resolved m_referenceObjectName:
    // a deferred catalog rebuild (mod/submod switch) clears m_referenceObjectName to
    // "" while a moved transform is still live, and keying off that empty resolved
    // name would mis-read the next pick as a startup "keep current" and re-float the
    // new object. m_referenceDesiredName holds the last picked object through the
    // defer (it is only cleared by entering None) and is set AFTER this block, so it
    // still names the object the live transform belongs to -- and is genuinely empty
    // only at true startup, where keeping the restored transform is correct.
    {
        auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
        const std::string oldKey = lower(m_referenceDesiredName);  // object the live transform belongs to
        const std::string newKey = lower(name);                    // incoming pick
        bool changed = false;
        const reftransform::Xform next = reftransform::OnSwap(
            m_referenceTransforms, oldKey, newKey,
            reftransform::Xform{ m_referencePosition, m_referenceRotation }, changed);
        if (changed)
        {
            m_referencePosition = next.pos;
            m_referenceRotation = next.rot;
            // Snap the eased display transform to the (possibly restored) committed
            // value so the incoming object appears AT its placement instead of gliding
            // in from the previous object's position.
            m_displayPosition = m_referencePosition;
            m_displayRotation = m_referenceRotation;
        }
    }

    m_referenceDesiredName = name;   // the INTENT (persisted); shown value is resolved below
    if (!name.empty())
        m_referenceObjectVisible = true;
    // Auto-select on an explicit pick so the manipulator gizmo appears immediately. A
    // desired name that resolves present keeps this true (ResolveDesiredReference's
    // present branch doesn't touch selection); an absent/None/deferred desired is
    // force-deselected inside ResolveDesiredReference. The lock is intentionally STICKY +
    // persisted (do NOT clear m_referenceLocked here, or the startup restore is wiped).
    m_referenceObjectSelected = RefLockResolveSelected(!name.empty(), m_referenceLocked);
    m_hoverManip = ManipHandle();
    ResolveDesiredReference();
}

// [capture] Sum of shadow-volume sub-meshes across the primary mesh and all
// hardpoint attachments. Used by --capture-ref to warn when the loaded object
// carries no shadow geometry (so the operator knows the image will show no shadow).
size_t Engine::ReferenceShadowSubMeshCount() const
{
    size_t count = m_referenceObjectMesh.ShadowSubMeshes().size();
    for (const auto& att : m_referenceAttachments)
        count += att->mesh.ShadowSubMeshes().size();
    return count;
}

// [capture] World-space AABB of the loaded reference object. The mesh's
// GetBoundingBox is OBJECT-space; transform all 8 corners by ReferenceObjectWorld()
// (scale + rotation + translation) and take the enclosing min/max so a fit camera
// can frame the actual on-screen extent (rotation/scale-aware).
bool Engine::GetReferenceObjectBounds(D3DXVECTOR3& outMin, D3DXVECTOR3& outMax) const
{
    if (m_referenceObjectMesh.IsEmpty() || !m_referenceObjectMesh.HasResolved())
        return false;
    D3DXVECTOR3 omin, omax;
    if (!m_referenceObjectMesh.GetBoundingBox(omin, omax)) return false;

    const D3DXMATRIX world = ReferenceObjectWorld();
    const D3DXVECTOR3 corners[8] = {
        D3DXVECTOR3(omin.x, omin.y, omin.z), D3DXVECTOR3(omax.x, omin.y, omin.z),
        D3DXVECTOR3(omin.x, omax.y, omin.z), D3DXVECTOR3(omax.x, omax.y, omin.z),
        D3DXVECTOR3(omin.x, omin.y, omax.z), D3DXVECTOR3(omax.x, omin.y, omax.z),
        D3DXVECTOR3(omin.x, omax.y, omax.z), D3DXVECTOR3(omax.x, omax.y, omax.z),
    };
    D3DXVECTOR3 wmin( 1e30f,  1e30f,  1e30f);
    D3DXVECTOR3 wmax(-1e30f, -1e30f, -1e30f);
    for (const auto& c : corners)
    {
        D3DXVECTOR3 w;
        D3DXVec3TransformCoord(&w, &c, &world);
        wmin.x = (std::min)(wmin.x, w.x); wmin.y = (std::min)(wmin.y, w.y); wmin.z = (std::min)(wmin.z, w.z);
        wmax.x = (std::max)(wmax.x, w.x); wmax.y = (std::max)(wmax.y, w.y); wmax.z = (std::max)(wmax.z, w.z);
    }
    outMin = wmin;
    outMax = wmax;
    return true;
}

// Full teardown of the render-state fields RebuildReferenceObjectMesh clears on its
// empty-name / deferred exits: mesh, hardpoint attachments, render scale, status. Does
// NOT touch m_referenceMeshDeferred -- that flag stays owned by the caller
// (ResolveDesiredReference), which sets it per branch -- so a clear path never leaves a
// stale attachment node or a stale render scale behind.
void Engine::ResetReferenceRenderState()
{
    m_referenceObjectMesh.Clear();
    m_referenceAttachments.clear();
    m_referenceScaleFactor  = 1.0f;
    m_referenceObjectStatus = ReferenceObjectStatus::None;
}

// Resolve the desired (intended/persisted) reference name into the shown selection,
// existence-gated against the catalog. Clears to None + deselects when the object is
// absent or nothing is desired; defers (shown None now) while the catalog rebuilds and
// is retried by Update() on catalog-ready. A successful resolve does NOT auto-select --
// a restore is inert (gizmo appears only on a user click); an explicit pick keeps its
// gizmo because SetReferenceObject sets m_referenceObjectSelected before calling here and
// the present branch leaves that flag untouched.
void Engine::ResolveDesiredReference()
{
    // Not built yet (startup, or just-invalidated by a mod switch) AND something is
    // desired: show None NOW (no stale id during the async build) and arm the retry.
    if (!m_referenceCatalogBuilt && !m_referenceDesiredName.empty())
    {
        m_referenceObjectName.clear();
        SetReferenceObjectSelected(false);   // deselect: hides gizmo + aborts any in-flight drag
        ResetReferenceRenderState();
        m_catalogWanted         = true;
        m_referenceMeshDeferred = true;      // Update() retries this fn on catalog-ready
        return;
    }

    m_referenceMeshDeferred = false;
    const std::string resolved = m_referenceCatalogBuilt
        ? ResolveReferenceName(m_referenceCatalog.objects, m_referenceDesiredName)
        : std::string();                     // desired empty -> "" regardless
    m_referenceObjectName = resolved;

    if (resolved.empty())                    // explicit None, or absent-from-this-stack
    {
        SetReferenceObjectSelected(false);   // honest None: deselect
        ResetReferenceRenderState();
        return;
    }
    RebuildReferenceObjectMesh();            // present: load it (may set LoadFailed for a bad .alo)
}

// Resolve the selected Name -> model path -> load. The probe (only on the
// load-failure path, so the common case parses the .alo once) distinguishes a
// skinned/unsupported object from a missing/corrupt one for the picker status.
// Resolve + CreateBuffers no-op until the device is valid; Load (CPU) always runs.
void Engine::RebuildReferenceObjectMesh()
{
    m_referenceAttachments.clear();   // rebuilt below iff the unit mounts hardpoint models
    // Reset the render scale at the TOP so every exit path (empty-name clear,
    // deferred catalog-not-built return, LoadFailed, successful resolve) leaves no
    // stale scale from a previously-selected object. (A mod/submod switch now clears via
    // ResetReferenceRenderState through ResolveDesiredReference, so Rebuild's deferred
    // return is a fallback.) Overwritten from the catalog only on a successful resolve below.
    m_referenceScaleFactor = 1.0f;

    if (m_referenceObjectName.empty())
    {
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::None;
        m_referenceMeshDeferred = false;
        return;
    }

    // The catalog resolves Name -> model path; if it isn't built yet (startup
    // restore, or just-invalidated on a mod/submod switch), DEFER until the background
    // build finishes -- Update() retries this once the catalog is ready. Never build
    // synchronously here, or a submod switch with a selected object would freeze the UI.
    if (!m_referenceCatalogBuilt)
    {
        m_catalogWanted         = true;   // Update() launches the build after its harvest
        m_referenceMeshDeferred = true;
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::None;   // nothing renders until ready
        return;
    }
    m_referenceMeshDeferred = false;

    // Case-INSENSITIVE match: the catalog folds Names to lower-case keys (the
    // Alamo engine resolves Names case-insensitively) but stores original casing
    // for display, so a persisted/cross-mod name with different casing must still
    // resolve. Adopt the catalog's canonical casing so the snapshot converges.
    std::string modelPath;
    const GameObjectRef* selected = nullptr;
    for (const GameObjectRef& r : m_referenceCatalog.objects)
        if (_stricmp(r.name.c_str(), m_referenceObjectName.c_str()) == 0)
        {
            modelPath = r.modelPath;
            m_referenceObjectName = r.name;
            selected = &r;
            // Successful catalog resolve -> adopt the per-object render scale.
            m_referenceScaleFactor = r.scaleFactor;
#ifndef NDEBUG
            fprintf(stderr, "[refscale] '%s' Scale_Factor=%.3f\n", r.name.c_str(), r.scaleFactor);
#endif
            break;
        }
    if (modelPath.empty())
    {
        m_referenceObjectMesh.Clear();
        m_referenceObjectStatus = ReferenceObjectStatus::LoadFailed;
        return;
    }

    // Gather this unit's hardpoint geometry from the catalog: bones whose meshes
    // are damaged-state (hide them so the unit renders intact) + the attach models to
    // mount. Bone names match the .alo CASE-INSENSITIVELY -> fold to lower for the
    // ReferenceObjectMesh hide-set + bone lookup.
    auto lower = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
    std::set<std::string> hideBones;
    struct AttachReq { std::string model; std::string bone; };
    std::vector<AttachReq> attach;
    if (selected)
        for (const std::string& hpName : selected->hardpointNames)
        {
            auto it = m_referenceCatalog.hardpoints.find(lower(hpName));
            if (it == m_referenceCatalog.hardpoints.end()) continue;
            const HardPointDef& d = it->second;
            if (!d.damageDecalBone.empty())     hideBones.insert(lower(d.damageDecalBone));
            if (!d.damageParticlesBone.empty()) hideBones.insert(lower(d.damageParticlesBone));
            if (!d.collisionMeshBone.empty())   hideBones.insert(lower(d.collisionMeshBone));
            // A hardpoint with no Model_To_Attach mounts nothing; one with no
            // Attachment_Bone has nowhere to mount (GetBoneObjectMatrix("") would
            // also alias an unnamed .alo bone) -- skip both rather than push a useless
            // or mis-placeable request.
            if (!d.modelToAttach.empty() && !d.attachmentBone.empty())
                attach.push_back({ d.modelToAttach, d.attachmentBone });
        }
    // A bone that is a live mount point (some hardpoint's Attachment_Bone) must
    // never be hidden -- even when another hardpoint lists the SAME bone as its
    // Collision_Mesh/Damage_* bone. Vanilla FoC does exactly this: the Star Destroyer
    // fighter-bay/tractor hardpoints set Collision_Mesh == Attachment_Bone (SPAWN_00 /
    // HP_trac_bone). Hiding a mount bone would drop the hull geometry the attach model
    // sits on. Mount points win, so subtract them from the hide-set after gathering.
    for (const AttachReq& a : attach) hideBones.erase(lower(a.bone));

    const std::string aloPath = "Data\\Art\\Models\\" + modelPath;
    if (m_referenceObjectMesh.Load(m_fileManager, aloPath, hideBones))
    {
        if (m_pDevice != NULL)
        {
            // Resolve degrades per-sub-mesh on a throwing/missing shader (see
            // ReferenceObjectMesh::Resolve, which catches the wexception getShader
            // can raise), so it returns false only when NOTHING resolved. Report
            // that as LoadFailed rather than a silent "Ok" that renders nothing
            // (HasResolved() would be false -> RenderReferenceObject draws nothing,
            // no error shown). No try/catch here: the per-sub-mesh guard already
            // contains the throw, so this call can't throw a wexception.
            if (!m_referenceObjectMesh.Resolve(m_shaderManager, m_pDevice))
            {
                m_referenceObjectMesh.Clear();
                m_referenceObjectStatus = ReferenceObjectStatus::LoadFailed;
                return;
            }
            m_referenceObjectMesh.CreateBuffers(m_pDevice, m_fileManager);
        }

        // Mount each hardpoint's attach model at its Attachment_Bone on the unit.
        // Graceful: skip a hardpoint whose bone isn't in the unit skeleton, or whose
        // attach .alo is missing / corrupt / skinned-only / unresolvable (the unit still
        // renders; that attachment just doesn't). childPlacement * boneMatrix * objectWorld
        // places each attach sub-mesh (RenderReferenceObject).
        for (const AttachReq& a : attach)
        {
            D3DXMATRIX boneMat;
            if (!m_referenceObjectMesh.GetBoneObjectMatrix(a.bone, boneMat))
            {
                // The single highest-value diagnostic: a bone that SHOULD match but
                // doesn't (case/whitespace/encoding skew, or a stale XML bone name)
                // leaves the unit silently weaponless. Surface it in Debug feel-tests.
#ifndef NDEBUG
                fprintf(stderr, "[RefObj] attach '%s': Attachment_Bone '%s' not in unit skeleton -- skipped\n",
                        a.model.c_str(), a.bone.c_str());
#endif
                continue;
            }
            auto att = std::make_unique<ReferenceAttachment>();
            if (!att->mesh.Load(m_fileManager, "Data\\Art\\Models\\" + a.model))
            {
#ifndef NDEBUG
                fprintf(stderr, "[RefObj] attach '%s' failed to load (missing/corrupt/skinned-only) -- skipped\n",
                        a.model.c_str());
#endif
                continue;
            }
            if (m_pDevice != NULL)
            {
                if (!att->mesh.Resolve(m_shaderManager, m_pDevice))
                    continue;   // nothing resolved -> don't mount an invisible attachment (Resolve logs per-sub-mesh in Debug)
                att->mesh.CreateBuffers(m_pDevice, m_fileManager);
            }
            att->boneMatrix = boneMat;
            m_referenceAttachments.push_back(std::move(att));
        }
        // Aggregate signal for feel-testing: a unit that should be fully armed but
        // mounted fewer than expected models is visible at a glance without per-skip noise.
#ifndef NDEBUG
        if (!attach.empty())
            fprintf(stderr, "[RefObj] '%s': mounted %zu of %zu hardpoint attach model(s)\n",
                    m_referenceObjectName.c_str(), m_referenceAttachments.size(), attach.size());
#endif

        // Load-time warning: if the primary object has shadow sub-meshes but
        // none resolved to a real shadow-volume effect, the stencil pass will silently
        // draw nothing. Emit once here (not per-frame) so it appears in host.log /
        // OutputDebugStringA without spamming. Primary object only; attachments are a
        // follow-up if needed. Uses OutputDebugStringA + printf — the same Release-visible
        // pair BloomLog uses (printf reaches the Debug console; OutputDebugStringA reaches
        // any attached debugger / DebugView in Release).
        {
            const auto& shadowSubs = m_referenceObjectMesh.ShadowSubMeshes();
            if (!shadowSubs.empty())
            {
                size_t resolved = 0;
                for (const RefSubMeshGpu& s : shadowSubs)
                    if (s.effect && s.effect->isShadowVolume()) ++resolved;
                if (resolved == 0)
                {
                    char buf[512];
                    snprintf(buf, sizeof(buf),
                        "[shadow] reference object '%s': %zu shadow-volume sub-mesh(es) but none "
                        "resolved (MeshShadowVolume.fx/RSkinShadowVolume.fx not found in active "
                        "content) - no model shadow will render\n",
                        m_referenceObjectName.c_str(), shadowSubs.size());
                    OutputDebugStringA(buf);
                    printf("%s", buf);
                }
            }
        }

        m_referenceObjectStatus = ReferenceObjectStatus::Ok;
        return;
    }

    // Load failed (skinned-only / collision-only / missing / corrupt) -> probe to
    // tell the user which, so the picker shows the right message: a genuinely
    // absent file (NotFound) reads "model file not found", a skinned-only object
    // reads "not supported", and anything else (corrupt / non-mesh) "couldn't load".
    m_referenceObjectMesh.Clear();
    const ModelProbeResult probe = ProbeModelSkinned(m_fileManager, modelPath);
    m_referenceObjectStatus =
        (probe == ModelProbeResult::SkinnedUnsupported) ? ReferenceObjectStatus::Skinned
      : (probe == ModelProbeResult::NotFound)           ? ReferenceObjectStatus::ModelMissing
      :                                                   ReferenceObjectStatus::LoadFailed;
}

// Grid spacing must stay positive (the line loop steps by it).
void Engine::SetGridSpacing(float spacing)
{
    m_gridSpacing = (spacing > 0.0f) ? spacing : 1.0f;
}

// compile IDR_SHADER_GROUND_LIT, cache parameter handles, select the
// best-validating technique (bump → gloss), and build the tangent-space ground
// vertex declaration. Graceful-degrade: on any failure m_pGroundEffect stays
// NULL and Render() falls back to the unlit fixed-function ground quad.
void Engine::InitGroundEffect()
{
    if (m_pGroundDecl == NULL)
    {
        static const D3DVERTEXELEMENT9 decl[] = {
            {0, offsetof(GroundVertex, Position), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, offsetof(GroundVertex, Normal),   D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0},
            {0, offsetof(GroundVertex, TexCoord), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            {0, offsetof(GroundVertex, Tangent),  D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT,  0},
            {0, offsetof(GroundVertex, Binormal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
            D3DDECL_END()
        };
        m_pDevice->CreateVertexDeclaration(decl, &m_pGroundDecl);
    }

    HMODULE hMod   = GetModuleHandle(NULL);
    HRSRC   hRes   = FindResource(hMod, MAKEINTRESOURCE(IDR_SHADER_GROUND_LIT), RT_RCDATA);
    if (!hRes) return;
    HGLOBAL hData  = LoadResource(hMod, hRes);
    DWORD   dwSize = SizeofResource(hMod, hRes);
    void*   pData  = hData ? LockResource(hData) : NULL;
    if (!pData || !dwSize) return;

    LPD3DXBUFFER pErrors = NULL;
    HRESULT hr = D3DXCreateEffect(m_pDevice, pData, dwSize, NULL, NULL, 0, NULL,
                                  &m_pGroundEffect, &pErrors);
    if (FAILED(hr))
    {
#ifndef NDEBUG
        if (pErrors) fprintf(stderr, "[GroundLit] effect compile failed: %s\n",
                             (const char*)pErrors->GetBufferPointer());
#endif
        SAFE_RELEASE(pErrors);
        m_pGroundEffect = NULL;
        return;
    }
    SAFE_RELEASE(pErrors);

    m_hGroundWVP           = m_pGroundEffect->GetParameterByName(NULL, "g_WorldViewProj");
    m_hGroundWorld         = m_pGroundEffect->GetParameterByName(NULL, "g_World");
    m_hGroundSphFill       = m_pGroundEffect->GetParameterByName(NULL, "g_SphFill");
    m_hGroundLightObjVec   = m_pGroundEffect->GetParameterByName(NULL, "g_LightObjVec");
    m_hGroundLightDiffuse  = m_pGroundEffect->GetParameterByName(NULL, "g_LightDiffuse");
    m_hGroundLightSpecular = m_pGroundEffect->GetParameterByName(NULL, "g_LightSpecular");
    m_hGroundEyeObjPos     = m_pGroundEffect->GetParameterByName(NULL, "g_EyeObjPos");
    m_hGroundBaseTex       = m_pGroundEffect->GetParameterByName(NULL, "g_BaseTexture");
    m_hGroundNormalTex     = m_pGroundEffect->GetParameterByName(NULL, "g_NormalTexture");

    // Single vs_2_0/ps_2_0 bump technique. If the device can't validate it,
    // drop the effect entirely so Render() uses the unlit FF ground quad.
    // SetTechnique state survives device Reset (effect state, not device state).
    D3DXHANDLE tech = m_pGroundEffect->GetTechniqueByName("bump");
    if (tech == NULL || FAILED(m_pGroundEffect->ValidateTechnique(tech)))
    {
#ifndef NDEBUG
        fprintf(stderr, "[GroundLit] bump technique failed validation; using FF fallback\n");
#endif
        SAFE_RELEASE(m_pGroundEffect);
        m_pGroundEffect = NULL;
        return;
    }
    m_pGroundEffect->SetTechnique(tech);
#ifndef NDEBUG
    fprintf(stderr, "[GroundLit] effect loaded ok; technique=bump\n");
#endif
}

// 1x1 neutral tangent-space normal (0,0,1) packed RGB(128,128,255).
// Dynamic+default pool so it's lockable under D3D9Ex (see CreateSolidColorTexture).
void Engine::CreateGroundFlatNormal()
{
    SAFE_RELEASE(m_pGroundFlatNormalTexture);
    if (m_pDevice == NULL) return;
    if (FAILED(m_pDevice->CreateTexture(1, 1, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &m_pGroundFlatNormalTexture, NULL)))
    {
        m_pGroundFlatNormalTexture = NULL;
        return;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(m_pGroundFlatNormalTexture->LockRect(0, &lr, NULL, D3DLOCK_DISCARD)))
    {
        // RGB = flat tangent-space normal (0,0,1); ALPHA = 0 so a slot with no
        // real _bc gloss map is matte (no specular) instead of fully glossy
        // (gloss lives in the _bc map's alpha — see GroundLit.fx).
        *(DWORD*)lr.pBits = D3DCOLOR_ARGB(0, 128, 128, 255);
        m_pGroundFlatNormalTexture->UnlockRect(0);
    }
}

// resolve the active slot's companion `<base>_bc` normal map from the
// game/mod via FileManager. Only the three vanilla-textured slots have a known
// base name; dirt/solid/empty-custom slots get the flat-normal fallback (lit,
// no relief). Re-run on slot change and after device Reset.
void Engine::ReloadGroundNormalTexture()
{
    SAFE_RELEASE(m_pGroundNormalTexture);
    if (m_pDevice == NULL) return;

    std::string normalLeaf;     // bundled-vanilla slots: known base + _bc.dds
    switch (m_groundTextureIndex)
    {
        case 1: normalLeaf = "W_TEMPGRND00_bc.dds"; break;  // grass
        case 2: normalLeaf = "W_SAND00_bc.dds";     break;  // sand
        case 3: normalLeaf = "W_SNOW_RGH_bc.dds";   break;  // snow
        default: break;
    }
    std::string customDerived;  // custom slots: <custombase>_bc.<ext> next to it
    if (normalLeaf.empty()
        && m_groundTextureIndex >= 0 && m_groundTextureIndex < kGroundTextureCount
        && !m_groundSlotCustomPaths[m_groundTextureIndex].empty())
    {
        const std::wstring& base = m_groundSlotCustomPaths[m_groundTextureIndex];
        size_t dot = base.find_last_of(L'.');
        std::wstring derived = (dot == std::wstring::npos)
            ? base + L"_bc"
            : base.substr(0, dot) + L"_bc" + base.substr(dot);
        customDerived = WideToAnsi(derived);
    }

    if (!normalLeaf.empty())
    {
        std::string path = "DATA\\ART\\TEXTURES\\" + normalLeaf;
        m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, path);
        if (m_pGroundNormalTexture == NULL)   // mod may stash it loose by leaf name
            m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, normalLeaf);
    }
    else if (!customDerived.empty())
    {
        m_pGroundNormalTexture = LoadTextureViaFileManager(m_pDevice, m_fileManager, customDerived);
    }
#ifndef NDEBUG
    fprintf(stderr, "[GroundLit] slot=%d normal=%s\n", m_groundTextureIndex,
            m_pGroundNormalTexture ? "resolved" : "flat-fallback");
#endif
}

// draw the lit ground quad through m_pGroundEffect. World is identity
// (the quad is already in world space), so object space == world space for the
// per-pixel light/half vectors — matching the game's object-space bump path.
void Engine::RenderGroundLit()
{
    static const D3DXMATRIX Identity(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    D3DXMATRIX wvp = Identity * m_view * m_projection;

    static const float TEXTURE_SCALE  = 256;
    static const float MAP_SIZE       = 80;
    static const float UNITS_PER_CELL = 20;
    const float z = m_groundZ;
    const float h = UNITS_PER_CELL * MAP_SIZE / 2.0f;
    const float u = MAP_SIZE * UNITS_PER_CELL / TEXTURE_SCALE;
    const D3DXVECTOR3 N(0,0,1), T(1,0,0), B(0,1,0);
    const GroundVertex quad[4] = {
        {D3DXVECTOR3(-h,-h,z), N, D3DXVECTOR2(0,0), T, B},
        {D3DXVECTOR3( h,-h,z), N, D3DXVECTOR2(u,0), T, B},
        {D3DXVECTOR3(-h, h,z), N, D3DXVECTOR2(0,u), T, B},
        {D3DXVECTOR3( h, h,z), N, D3DXVECTOR2(u,u), T, B},
    };

    D3DXVECTOR3 lightVec(m_lights[0].Position.x, m_lights[0].Position.y, m_lights[0].Position.z);
    D3DXVec3Normalize(&lightVec, &lightVec);
    D3DXVECTOR3 eyePos(m_eye.Position.x, m_eye.Position.y, m_eye.Position.z);

    m_pGroundEffect->SetMatrix     (m_hGroundWVP,           &wvp);
    m_pGroundEffect->SetMatrix     (m_hGroundWorld,         &Identity);
    m_pGroundEffect->SetMatrixArray(m_hGroundSphFill,       m_sphLightFill, 3);
    m_pGroundEffect->SetValue      (m_hGroundLightObjVec,   &lightVec, sizeof(D3DXVECTOR3));
    m_pGroundEffect->SetVector     (m_hGroundLightDiffuse,  &m_lights[0].Diffuse);
    m_pGroundEffect->SetVector     (m_hGroundLightSpecular, &m_lights[0].Specular);
    m_pGroundEffect->SetValue      (m_hGroundEyeObjPos,     &eyePos, sizeof(D3DXVECTOR3));
    m_pGroundEffect->SetTexture    (m_hGroundBaseTex,       m_pGroundTexture);
    m_pGroundEffect->SetTexture    (m_hGroundNormalTex,
        m_pGroundNormalTexture ? m_pGroundNormalTexture : m_pGroundFlatNormalTexture);

    // The vertex declaration is NOT captured by the effect state block, so it
    // must be restored or the particle draws lose their diffuse-colour stream
    // Render states the effect changes ARE saved/restored by Begin/End.
    IDirect3DVertexDeclaration9* oldDecl = NULL;
    m_pDevice->GetVertexDeclaration(&oldDecl);

    UINT passes = 0;
    m_pGroundEffect->Begin(&passes, 0);
    for (UINT i = 0; i < passes; ++i)
    {
        m_pGroundEffect->BeginPass(i);
        m_pDevice->SetVertexDeclaration(m_pGroundDecl);
        m_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(GroundVertex));
        m_pGroundEffect->EndPass();
    }
    m_pGroundEffect->End();

    m_pDevice->SetVertexDeclaration(oldDecl);
    if (oldDecl) oldDecl->Release();
}

bool Engine::SetSkydomeSlot(int newIndex)
{
    if (newIndex < 0 || newIndex >= kSkydomeSlotCount) return false;
    if (newIndex == m_skydomeIndex) return true;
    if (!ReloadSkydomeTexture(newIndex))
    {
        // Fall back to Off on failure
        m_skydomeIndex = kSkydomeOffSlot;
        SAFE_RELEASE(m_pSkydomeTexture);
        return false;
    }
    m_skydomeIndex = newIndex;
#ifndef NDEBUG
    fprintf(stdout, "[Skydome] select slot=%d\n", newIndex);
#endif
    return true;
}

bool Engine::SetSkydomeCustomPath(int slot, const std::wstring& path)
{
    if (slot < kSkydomeFirstCustomSlot || slot >= kSkydomeSlotCount) return false;
    m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot] = path;
    if (m_skydomeIndex == slot)
    {
        return ReloadSkydomeTexture(slot);
    }
    return true;
}

const std::wstring& Engine::GetSkydomeCustomPath(int slot) const
{
    static const std::wstring empty;
    if (slot < kSkydomeFirstCustomSlot || slot >= kSkydomeSlotCount) return empty;
    return m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot];
}

bool Engine::IsSkydomeSlotEmpty(int slot) const
{
    if (slot == kSkydomeOffSlot) return false;       // Off is "selectable", not empty
    if (slot < kSkydomeBundledCount) return false;   // bundled always populated
    if (slot < kSkydomeSlotCount)
        return m_skydomeCustomSlotPaths[slot - kSkydomeFirstCustomSlot].empty();
    return true;
}

Engine::Engine(HWND hFocus, HWND hDevice, ITextureManager& textureManager, IShaderManager& shaderManager, IFileManager& fileManager)
    : m_textureManager(textureManager), m_shaderManager(shaderManager), m_fileManager(fileManager)
{
	// Zero shader pointers up front so partial-failure cleanup is safe
	m_pDistortShader = NULL;
	for (int i = 0; i < NUM_SHADERS; i++) m_pShaders[i] = NULL;
	m_pBloomEffect = NULL;
	m_pBloomPing   = NULL;
	m_pBloomPong   = NULL;
	// [soft-shadows] mask RTs + blur effect (zeroed for safe partial-failure cleanup)
	m_pShadowMask       = NULL;
	m_pShadowMaskMsaa   = NULL;
	m_pShadowBlurEffect = NULL;
	// skydome geometry — pre-init so partial-failure cleanup is safe
	m_pSkydomeVB        = NULL;
	m_pSkydomeIB        = NULL;
	m_pSkydomeDecl      = NULL;
	m_skydomeIndexCount = 0;
	// skydome effect + texture state
	m_pSkydomeEffect    = NULL;
	m_hSkydomeWVP       = NULL;
	m_hSkydomeTex       = NULL;
	m_pSkydomeTexture   = NULL;
	m_skydomeIndex      = kSkydomeOffSlot;
	// ground-lighting effect + tangent-space decl + normal-map state
	m_pGroundEffect            = NULL;
	m_pGroundDecl              = NULL;
	m_pGroundNormalTexture     = NULL;
	m_pGroundFlatNormalTexture = NULL;
	m_hGroundWVP = m_hGroundWorld = m_hGroundSphFill = NULL;
	m_hGroundLightObjVec = m_hGroundLightDiffuse = m_hGroundLightSpecular = NULL;
	m_hGroundEyeObjPos = m_hGroundBaseTex = m_hGroundNormalTex = NULL;
	m_hBloomStrength = m_hBloomCutoff = m_hBloomSize = NULL;
	m_hBloomIteration = m_hBloomSceneTextureParam = NULL;
	m_hBloomResolutionConstants = NULL;
	m_hBloomTechnique = NULL;
	m_bloomPassCount  = 0;

	// Initialize members
	m_showGround     = true;
	m_groundZ        = 0.0f;
	m_groundTextureIndex = 0;                 // dirt by default
	m_groundSolidColor   = RGB(128, 128, 128); // flat grey default
	m_pGroundTexture = NULL;      // must be NULL before first ReloadGroundTexture()
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
    // [shadow] Grey default matching the registry default (RGB 100,100,110 / 255).
    // m_shadow.xyz is the darken tint (result = dest * m_shadow.rgb); pure-black
    // would make shadows fully opaque black if SetShadow is ever skipped.
    m_shadow         = D3DXVECTOR4(100.0f/255.0f, 100.0f/255.0f, 110.0f/255.0f, 0.0f);
    m_background     = RGB(0x14,0x08,0x34);

	//
	// Initialize Direct3D9Ex
	//
	// D3D9 → D3D9Ex. D3D9Ex is required for
	// the shared-handle render-target path; the spike validated
	// the entire engine→D3D11→DComp pipeline on this rig. The
	// decision: hard-fail if D3D9Ex is unavailable — there is no
	// in-process fallback to vanilla D3D9.
	{
		HRESULT createHr = Direct3DCreate9Ex(D3D_SDK_VERSION, &m_pDirect3D);
		if (FAILED(createHr) || m_pDirect3D == NULL)
		{
			throw runtime_error("Unable to initialize Direct3D9Ex");
		}
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

	if ((m_presentationParameters.AutoDepthStencilFormat = GetDepthStencilFormat(DisplayMode.Format, true)) == D3DFMT_UNKNOWN)
	{
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to find a matching depth/stencil buffer format (D24S8 required for model shadows)");
	}

	m_presentationParameters.MultiSampleType = GetMultiSampleType(&m_presentationParameters.MultiSampleQuality, DisplayMode.Format, m_presentationParameters.AutoDepthStencilFormat, m_presentationParameters.Windowed);

	// Create device (first try hardware, then software).
	// D3D9 CreateDevice → D3D9Ex CreateDeviceEx
	// (extra trailing nullptr for fullscreen display mode — we are always
	// windowed) + D3DCREATE_MULTITHREADED, which is required for
	// cross-device shared-handle textures and costs ~5% per-frame
	// overhead in exchange. Verified across 189k frames in the dxgi_spike
	// without anomaly.
	{
		DWORD const baseFlags = D3DCREATE_MULTITHREADED;
		DWORD       vertexFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
		const char* vpModeName  = "HWVP";
		HRESULT     createDevHr = m_pDirect3D->CreateDeviceEx(
			D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocus,
			baseFlags | vertexFlags, &m_presentationParameters,
			NULL, &m_pDevice);
		if (FAILED(createDevHr))
		{
			vertexFlags = D3DCREATE_SOFTWARE_VERTEXPROCESSING;
			vpModeName  = "SOFTWARE_VP";
			createDevHr = m_pDirect3D->CreateDeviceEx(
				D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hFocus,
				baseFlags | vertexFlags, &m_presentationParameters,
				NULL, &m_pDevice);
		}
		if (FAILED(createDevHr))
		{
			SAFE_RELEASE(m_pDirect3D);
			throw runtime_error("Unable to create render device");
		}

		// Adapter info for multi-GPU LUID match debugging.
		D3DADAPTER_IDENTIFIER9 adapterIdent = {};
		m_pDirect3D->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterIdent);
		printf("[D3D9Ex] device created (%s multithreaded) adapter=%s "
		       "VendorId=0x%lX DeviceId=0x%lX\n",
		       vpModeName, adapterIdent.Description,
		       (unsigned long)adapterIdent.VendorId,
		       (unsigned long)adapterIdent.DeviceId);
		fflush(stdout);
	}

	// Create vertex declaration
	if (FAILED(m_pDevice->CreateVertexDeclaration(ParticleElements, &m_pDeclaration)))
	{
		SAFE_RELEASE(m_pDevice);
		SAFE_RELEASE(m_pDirect3D);
		throw runtime_error("Unable to create vertex declaration");
	}

	// Create ground texture. routed through ReloadGroundTexture
	// so the same code path is shared with SetGroundTexture and the
	// lost-device recovery branches below. m_groundTextureIndex was
	// initialized to 0 (dirt) in the constructor; main.cpp's startup
	// flow may call SetGroundTexture(savedIndex) shortly after engine
	// construction to swap in the user's persisted choice.
	if (!ReloadGroundTexture())
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

	// build the UV sphere mesh used by the skydome render pass.
	// m_pDevice is guaranteed valid at this point.
	InitSkydomeMesh();
	// compile the skydome HLSL effect and cache its parameter handles.
	// Graceful-degrade: if compile fails m_pSkydomeEffect stays NULL and the
	// render pass (Task 4) will guard on it and skip skydome rendering.
	InitSkydomeEffect();
	// ground-lighting effect + tangent-space decl + flat-normal fallback.
	// Graceful-degrade identically: on compile failure m_pGroundEffect stays
	// NULL and Render() falls back to the unlit fixed-function ground quad.
	CreateGroundFlatNormal();
	InitGroundEffect();
	ReloadGroundNormalTexture();

#ifndef NDEBUG
	// bring-up driver (debug only): force-load a real game dome by Name
	// so the render core can be feel-tested before the M3 picker exists. Default
	// launch is unaffected -- with no env var both slots stay Off.
	// TODO(M3): remove once the React picker drives SetSkydomeEnvironment.
	//   set ALO_MT15_TEST_DOME=<PrimaryName> [ALO_MT15_TEST_SEC=<SecondaryName>]
	//       [ALO_MT15_TEST_CTX=land|space]   (default space)
	{
		char buf[256];
		if (GetEnvironmentVariableA("ALO_MT15_TEST_DOME", buf, sizeof(buf)) > 0)
		{
			std::string prim = buf, sec;
			if (GetEnvironmentVariableA("ALO_MT15_TEST_SEC", buf, sizeof(buf)) > 0) sec = buf;
			SkydomeContext ctx = SkydomeContext::Space;
			if (GetEnvironmentVariableA("ALO_MT15_TEST_CTX", buf, sizeof(buf)) > 0
			    && _stricmp(buf, "land") == 0)
				ctx = SkydomeContext::Land;
			fprintf(stderr, "[SkyEnv] test driver: ctx=%s primary='%s' secondary='%s'\n",
			        ctx == SkydomeContext::Land ? "land" : "space", prim.c_str(), sec.c_str());
			SetSkydomeEnvironment(ctx, prim, sec);
		}
	}
	// bring-up driver (debug only): load a reference object by .alo path so
	// the rigid-multi-part render core can be feel-tested before the picker.
	//   set ALO_LT7_TEST_OBJECT=Data\Art\Models\AI_Bunker_Turret1.alo
	{
		char buf[512];
		if (GetEnvironmentVariableA("ALO_LT7_TEST_OBJECT", buf, sizeof(buf)) > 0)
		{
			std::string aloPath = buf;
			fprintf(stderr, "[RefObj] test driver: loading '%s'\n", aloPath.c_str());
			if (m_referenceObjectMesh.Load(m_fileManager, aloPath))
			{
				m_referenceObjectMesh.Resolve(m_shaderManager, m_pDevice);
				m_referenceObjectMesh.CreateBuffers(m_pDevice, m_fileManager);
				m_referenceObjectSelected = RefLockResolveSelected(true, m_referenceLocked);   // gizmo for bring-up (honours lock)
				fprintf(stderr, "[RefObj] loaded: %zu sub-meshes, skippedSkinned=%d\n",
				        m_referenceObjectMesh.SubMeshes().size(),
				        m_referenceObjectMesh.SkippedSkinned() ? 1 : 0);
			}
			else
			{
				fprintf(stderr, "[RefObj] load FAILED for '%s'\n", aloPath.c_str());
			}
		}
	}
#endif
}

Engine::~Engine()
{
	// Join the catalog worker BEFORE any member is torn down -- the worker
	// captures `this` and writes m_pendingCatalog / m_catalogMutex on completion, so
	// it must finish before those members are destroyed. (Blocks until the in-flight
	// build returns; at most the cost of one catalog parse, only at shutdown.)
	if (m_catalogThread.joinable()) m_catalogThread.join();

	ReleaseBloomTargets();
	SAFE_RELEASE(m_pBloomEffect);
	// [soft-shadows] mask RTs + the dedicated blur effect handle
	ReleaseShadowMaskTargets();
	SAFE_RELEASE(m_pShadowBlurEffect);
    for (int i = 0; i < NUM_SHADERS; i++)
    {
        SAFE_RELEASE(m_pShaders[i]);
    }
    SAFE_RELEASE(m_pDepthStencilSurface);
	// Released in Reset()/ResetForResize() but was leaked
	// at shutdown when no final Reset ran.
	SAFE_RELEASE(m_pEndFrameQuery);
	// MSAA surfaces
	SAFE_RELEASE(m_pMsaaColor);
	SAFE_RELEASE(m_pMsaaDepth);
	m_msaaActive = false;
	SAFE_RELEASE(m_pDistortShader);
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
	SAFE_RELEASE(m_pGroundTexture);
	// skydome effect + texture (released before geometry for symmetry)
	SAFE_RELEASE(m_pSkydomeEffect);
	SAFE_RELEASE(m_pSkydomeTexture);
	// skydome geometry (D3DPOOL_MANAGED — only released here, not on Reset)
	SAFE_RELEASE(m_pSkydomeVB);
	SAFE_RELEASE(m_pSkydomeIB);
	SAFE_RELEASE(m_pSkydomeDecl);
	// ground-lighting effect + decl + normal textures
	SAFE_RELEASE(m_pGroundEffect);
	SAFE_RELEASE(m_pGroundDecl);
	SAFE_RELEASE(m_pGroundNormalTexture);
	SAFE_RELEASE(m_pGroundFlatNormalTexture);
	SAFE_RELEASE(m_pDeclaration);
	SAFE_RELEASE(m_pDevice);
	SAFE_RELEASE(m_pDirect3D);
}
