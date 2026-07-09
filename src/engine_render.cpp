// engine_render.cpp — the render/update/post-processing/shader-reload cluster of the Engine class,
// moved verbatim out of engine.cpp (Phase B translation-unit split —
// tasks/2026-07-06-heavyweight-refactor-plan.md). SAME class, same header
// (engine.h); this is a file split, not a class split. Cluster-local
// file-scope statics moved with their consumers; helpers shared across
// TUs are declared in engine_internal.h with one definition.

#include <algorithm>   // sort (instance depth ordering in Update)
#include <cstdarg>     // va_list (BloomLog)
#include <cstdio>      // FILE/vsnprintf/fopen (bloom diagnostics)

#include "engine.h"
#include "engine_internal.h"
#include "exceptions.h"
#include "utils.h"
#include "resource.h"
#include "ParticleSystemInstance.h"  // instance Update/Render*/IsDead in the frame loop
#include "EmitterInstance.h"         // EmitterInstance::Vertex (ground/bloom/compose quads)
#include "ParticleMipFilter.h"       // #481 ALO_PARTICLE_MIPFILTER override (MODE_*)
#include "host/AlphaCompositor.h"    // present path
#include "SphericalHarmonics.h"      // ambient SPH floor in the scene pass

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

	// Diagnostic introspection (the parameter/technique dump + verdict, written
	// to bloom-diagnostic.log and the console) is gated behind ALO_SHADER_DIAG so
	// a normal user run generates nothing. Set the env var to capture the paper
	// trail when investigating a "bloom greyed out" report. The handle-caching
	// and m_bloomReady logic below always runs, regardless of this flag.
	static int s_bloomDiag = -1;
	if (s_bloomDiag < 0) { char b[8]; s_bloomDiag = (GetEnvironmentVariableA("ALO_SHADER_DIAG", b, sizeof(b)) > 0) ? 1 : 0; }
	const bool diag = (s_bloomDiag != 0);

	// Open the diagnostic file only under the diag flag. Failure here is
	// non-fatal; we'll still introspect and just skip the file output.
	FILE* f = NULL;
	if (diag)
	{
		std::wstring logPath = ExeDirectory() + L"bloom-diagnostic.log";
		_wfopen_s(&f, logPath.c_str(), L"w");
	}

	auto logf = [&](const char* fmt, ...)
	{
		if (!diag) return;
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
	if (diag) for (UINT i = 0; i < desc.Parameters; ++i)
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
	if (diag) for (UINT i = 0; i < desc.Techniques; ++i)
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

    // Update existing instances.
    //
    // [D2] Paused-idle skip: when the sim clock is FROZEN (paused preview,
    // GetTimeF returns the pause anchor) this loop re-walks every particle
    // with an unchanged currentTime — kills/resets/curve evaluations all
    // recompute last frame's results (pure CPU waste that scales with the
    // population). Skip it once one full pass has run at this exact time,
    // unless the instance LIST changed (a paused spawner trigger / kill
    // must still get its first pass so new particles appear). Any stepped
    // frame (record / frame-step) advances currentTime and runs normally —
    // weather reset-on-death timing is unaffected. m_numParticles is a
    // running delta, so skipping leaves it correctly frozen.
    if (currentTime != m_lastUpdatedSimTime
        || m_instances.size() != m_lastUpdatedInstanceCount)
    {
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
        m_lastUpdatedSimTime        = currentTime;
        m_lastUpdatedInstanceCount  = m_instances.size();
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
	// Now render to the heat texture.
	//
	// [D3] Zero-heat skip: with no live heat particles this pass only
	// re-clears the distort RT to the neutral normal (129,128,255) and
	// scans instances to draw nothing — skip it once the RT is ALREADY
	// neutral. The composite below is untouched: it still samples the
	// (neutral) distort texture through the game's real SceneHeat.fx
	// (ZFunc=ALWAYS, so the skipped Z-clear can't affect it; the scene
	// pass owns its own Z-clear at the top of Render). m_distortRtNeutral
	// starts false and is re-armed false wherever the distort RT is
	// (re)created (ResetParameters), so a fresh/reset RT always gets one
	// explicit neutral clear before the skip may engage.
	bool anyHeat = false;
	for (auto& instance : m_instances)
	{
		if (instance->HasLiveHeat()) { anyHeat = true; break; }
	}
	if (anyHeat || !m_distortRtNeutral)
	{
		IDirect3DSurface9* pDistortSurface;
		m_pDistortTexture->GetSurfaceLevel(0, &pDistortSurface);
		m_pDevice->SetRenderTarget(0, pDistortSurface);
		SAFE_RELEASE(pDistortSurface);

		m_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(129,128,255), 1.0f, 0);
		for (auto& instance : m_instances)
		{
			instance->RenderHeat(m_pDevice);
		}
		// Heat drew → the RT holds distortion (re-clear next frame);
		// nothing drew → it is freshly neutral and stays skippable.
		m_distortRtNeutral = !anyHeat;
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
