// engine_device.cpp — the device recovery/reset cluster of the Engine class,
// moved verbatim out of engine.cpp (a translation-unit split). SAME class, same header
// (engine.h); this is a file split, not a class split. Helpers shared across
// TUs are declared in engine_internal.h with one definition.

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
#include "engine_internal.h"
#include "common/exceptions.h"
#include "common/ResourceLimits.h"   // kMaxTextureAssetBytes (asset-read size caps)
#include "Resources/resource.h"
#include "simulation/ParticleSystemInstance.h"
#include "simulation/EmitterInstance.h"
#include "SphericalHarmonics.h"
#include "common/utils.h"     // WideToAnsi for custom-slot path bridging
#include "host/AlphaCompositor.h"
#include "host/Compositor.h"
using namespace std;


devicerecovery::Result Engine::ProbeDeviceRecovery()
{
	++m_deviceStateProbeCount;
	devicerecovery::D3D9ExRecoveryPort<IDirect3DDevice9Ex, Engine>
	    port(m_pDevice, *this);
	return devicerecovery::RunDeviceRecoveryStep(
	    m_deviceRecovery,
	    port,
	    m_pAlphaCompositor != nullptr);
}

bool Engine::IsDeviceRecoveryThread() const
{
	return m_deviceThreadId != 0 && GetCurrentThreadId() == m_deviceThreadId;
}

bool Engine::IsTerminalDeviceState() const
{
	return m_fatalDeviceState ||
	       m_deviceRecovery.phase == devicerecovery::Phase::Terminal;
}

bool Engine::TextureReloadCanContinue() const
{
	return !IsTerminalDeviceState() &&
	       !DeviceCallsBlocked() &&
	       !m_presentSuspect;
}

bool Engine::SetDeviceRecoveryWorkHoldForTesting(bool hold)
{
	if (!hold)
	{
		m_deviceRecoveryWorkTestHold = false;
		return true;
	}
	if (m_deviceRecoveryWorkTestHold) return true;
	if (DeviceCallsBlocked() || m_presentSuspect) return false;
	m_deviceRecoveryWorkTestHold = true;
	return true;
}

bool Engine::PrepareDeviceForFrame()
{
	if (m_fatalDeviceState ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Recovering ||
	    m_deviceResetInProgress)
		return false;
	if (m_presentSuspect ||
	    m_deviceRecovery.phase == devicerecovery::Phase::ResetExFailed ||
	    m_fullResetPending)
	{
		if (!RecoverDeviceIfNeeded()) return false;
	}
	if (DeviceCallsBlocked()) return false;
	if (!ReplayPendingTextureReload()) return false;
	return ReplayPendingParticleSystemChange();
}

bool Engine::PrepareComposedFrame()
{
	++m_composedFramePrepareCount;
	return PrepareDeviceForFrame();
}

bool Engine::RecoverDeviceIfNeeded()
{
	if (m_pDevice == NULL || m_fatalDeviceState ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Recovering ||
	    m_deviceResetInProgress)
		return false;

	devicerecovery::Result result = ProbeDeviceRecovery();
	switch (result.outcome)
	{
		case devicerecovery::Outcome::Render:
			if (m_fullResetPending)
			{
				if (!IsDeviceRecoveryThread()) return false;
				try { Reset(); }
				catch (...)
				{
					m_presentSuspect = true;
					return false;
				}
			}
			m_presentSuspect = false;
			return true;

		case devicerecovery::Outcome::ResetRequired:
			// D3D reset calls have the same creation-thread requirement as
			// ResetEx. The current LayoutBroker caller is on that UI/render
			// thread; retain a safe defer if a future caller is not.
			if (!IsDeviceRecoveryThread())
			{
				m_presentSuspect = true;
				return false;
			}
			try { Reset(); }
			catch (...)
			{
				m_presentSuspect = true;
				return false;
			}
			result = ProbeDeviceRecovery();
			if (result.outcome == devicerecovery::Outcome::Render)
			{
				m_presentSuspect = false;
				return true;
			}
			if (result.outcome == devicerecovery::Outcome::Fatal)
			{
				ReportFatalDeviceState(result.observedState,
				                       result.recoveryResult);
			}
			return false;

		case devicerecovery::Outcome::RetryResetEx:
			if (!IsDeviceRecoveryThread()) return false;
			try
			{
				if (!ResetForResize()) return false;
			}
			catch (...)
			{
				// ResetEx succeeded but its size-keyed rebuild failed. The
				// resize method restored the pending phase, so a later frame
				// can retry without admitting ordinary D3D work.
				m_presentSuspect = true;
				return false;
			}
			m_presentSuspect = false;
			return true;

		case devicerecovery::Outcome::Fatal:
			ReportFatalDeviceState(result.observedState,
			                       result.recoveryResult);
			return false;

		case devicerecovery::Outcome::SkipFrame:
		default:
			// A HUNG seen off the device thread remains retryable; the next
			// render frame performs the one permitted attempt.
			if (result.observedState == D3DERR_DEVICEHUNG)
				m_presentSuspect = true;
			return false;
	}
}

void Engine::ReportFatalDeviceState(HRESULT hr, HRESULT recoveryHr)
{
	if (m_fatalDeviceState) return;
	m_deferredParticleSystemChange.Reset();
	m_textureReloadAppliedGeneration = m_textureReloadRequestGeneration;
	m_fatalDeviceState = true;
	if (hr == D3DERR_DEVICEHUNG)
	{
		fprintf(stderr,
		        "[engine] FATAL DEVICEHUNG 0x%08lx — bounded ResetEx recovery "
		        "failed or was already consumed (recovery hr=0x%08lx); "
		        "rendering stops until restart\n",
		        (unsigned long)hr, (unsigned long)recoveryHr);
	}
	else
	{
		fprintf(stderr,
		        "[engine] FATAL device state 0x%08lx (%s) — device recreation "
		        "is required; rendering stops until restart\n",
		        (unsigned long)hr,
		        hr == D3DERR_DEVICEREMOVED ? "DEVICEREMOVED" : "unknown");
	}
	fflush(stderr);
}

void Engine::NotifyPresentResult(HRESULT hr)
{
	if (devicestate::ShouldCheckDeviceAfterPresent(hr))
	{
		m_presentSuspect = true;
	}
}

void Engine::ReleaseDeviceResourcesForReset()
{
	if (m_deviceResourcesReleased) return;
	m_deviceResourcesReleased = true;

	ReleaseBloomTargets();
	ReleaseShadowMaskTargets();   // [soft-shadows] DEFAULT-pool mask RTs
	SAFE_RELEASE(m_pDistortTexture);
	SAFE_RELEASE(m_pSceneTexture);
    SAFE_RELEASE(m_pDepthStencilSurface);
	// MSAA surfaces are D3DPOOL_DEFAULT — must be released before device Reset.
	SAFE_RELEASE(m_pMsaaColor);
	SAFE_RELEASE(m_pMsaaDepth);
	m_msaaActive = false;

	// ShaderManager retains historically loaded effects after their active
	// Engine/mesh refs change. Fan out once across every unique cached Effect so
	// no inactive D3DX state block remains live across ResetEx.
	m_pDistortShader->OnLostDevice();   // direct Engine-owned effect
	m_shaderManager.OnLostDevice();     // all manager-owned effects, deduplicated
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
	// Release mesh DEFAULT-pool resources only. Their manager-owned effects
	// were included in the deduplicated fanout above.
	m_skydomePrimaryMesh.ReleaseGpuResources();
	m_skydomeSecondaryMesh.ReleaseGpuResources();
	m_referenceObjectMesh.ReleaseGpuResources();
	for (auto& a : m_referenceAttachments) if (a) a->mesh.ReleaseGpuResources();
	SAFE_RELEASE(m_pGroundTexture);
	// The D3D11 compositor owns an alias of AlphaCompositor's shared D3D9
	// texture. Drop that alias first so the underlying video-memory object has
	// no cross-device owner when the D3D9 side is released below.
	if (m_pCompositionCompositor)
		m_pCompositionCompositor->ReleaseEngineSharedHandle();
	// The compositor's off-screen RT is D3DPOOL_DEFAULT, so
	// it must be released before m_pDevice->Reset — otherwise Reset
	// fails with D3DERR_INVALIDCALL and the engine is left in a
	// half-broken state (textures null, shaders OnLost'd but device
	// never reset). The Resize() call at the end of this function
	// recreates the RT against the new back-buffer size.
	if (m_pAlphaCompositor) m_pAlphaCompositor->ReleaseGpuResources();
	// Each EmitterInstance owns separate +1 references to its color and normal
	// textures. Drop those before the texture manager drops its cache refs;
	// otherwise DEFAULT-pool textures remain live across Reset and Reset fails
	// with D3DERR_INVALIDCALL.
	ReleaseInstanceTextures();
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
}

void Engine::ResetDeviceEffectsAfterReset()
{
	// D3DX requires every effect's OnResetDevice before any other post-reset
	// resource work. ShaderManager covers current and inactive cached effects
	// once by identity; the three direct Engine effects remain explicit.
	m_pDistortShader->OnResetDevice();
	m_shaderManager.OnResetDevice();
	if (m_pSkydomeEffect != NULL) m_pSkydomeEffect->OnResetDevice();
	if (m_pGroundEffect  != NULL) m_pGroundEffect->OnResetDevice();
}

HRESULT Engine::RefreshPresentationParametersAfterReset()
{
	IDirect3DSurface9* backBuffer = NULL;
	HRESULT hr = m_pDevice->GetBackBuffer(
	    0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
	if (FAILED(hr)) return hr;

	D3DSURFACE_DESC desc = {};
	hr = backBuffer->GetDesc(&desc);
	SAFE_RELEASE(backBuffer);
	if (FAILED(hr)) return hr;
	if (desc.Width == 0 || desc.Height == 0) return E_FAIL;

	// Reset/ResetEx zero these in/out fields before returning. Rehydrate them
	// from the actual swap-chain surface before any size-keyed allocation.
	m_presentationParameters.BackBufferWidth  = desc.Width;
	m_presentationParameters.BackBufferHeight = desc.Height;
	m_presentationParameters.BackBufferCount  = 1;
	m_presentationParameters.Windowed         = TRUE;
	return D3D_OK;
}

void Engine::ReacquireDeviceResourcesAfterReset()
{

	// BindShaderTextures stores TextureManager handles inside D3DX effect
	// parameters. The cache was destroyed before reset, so refill those active
	// annotations now rather than leaving effects pointing at released textures.
	for (int i = 0; i < NUM_SHADERS; ++i)
		BindShaderTextures(m_pShaders[i]);

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
	// Phase two of the live-emitter texture dance. The owning references were
	// released before TextureManager::OnLostDevice + Reset; re-fetch only now,
	// against the successfully reset device.
	ReacquireInstanceTextures();

	ResetParameters();

	// The alpha compositor owns D3D9 resources (RT + sysmem
	// surface) sized to the popup client area. Refresh them so the
	// off-screen RT keeps pace with the swap-chain's back-buffer
	// size, which the engine's render chain (m_pSceneTexture etc.)
	// is already keyed off via BackBufferWidth/Height.
	const LONGLONG _rpAlpha0 = EngQpcNow();
	if (m_pAlphaCompositor && m_presentationParameters.BackBufferWidth > 0
	    && m_presentationParameters.BackBufferHeight > 0)
	{
		m_pAlphaCompositor->Resize(
		    static_cast<int>(m_presentationParameters.BackBufferWidth),
		    static_cast<int>(m_presentationParameters.BackBufferHeight));
	}
	m_resetPerf.lastAlphaResizeMs =
	    EngQpcUs(_rpAlpha0, EngQpcNow()) / 1000.0;

	// re-apply the cached scene
	// viewport so its projection aspect ratio survives Reset.
	// ResetParameters() above rebuilt m_projection at FULL-RT aspect via
	// D3DXMatrixPerspectiveFovRH, overwriting whatever
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
		SetSceneViewportUnchecked(sx, sy, sw, sh);
	}

	m_deviceResourcesReleased = false;
	m_fullResetPending = false;
}

void Engine::Reset()
{
	if (m_pDevice == NULL || m_fatalDeviceState ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal ||
	    m_deviceRecovery.phase == devicerecovery::Phase::ResetExFailed ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Recovering ||
	    m_deviceResetInProgress)
	{
		throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));
	}

	// [resize-perf] sub-stage QPC brackets filled into m_resetPerf at
	// the end; the host logs them at 1 Hz. HUNG recovery uses the same
	// release/reacquire halves but ResetEx is driven by DeviceRecovery.h.
	const LONGLONG _rpT0 = EngQpcNow();
	LONGLONG _rpT1 = _rpT0;
	LONGLONG _rpT2 = _rpT0;
	LONGLONG _rpT3 = _rpT0;
	bool deviceResetSucceeded = false;
	m_fullResetPending = true;
	m_deviceResetInProgress = true;
	try
	{
		ReleaseDeviceResourcesForReset();
		_rpT1 = EngQpcNow();

		D3DPRESENT_PARAMETERS parameters =
		    GetDeviceRecoveryPresentationParameters();
		if (FAILED(m_pDevice->Reset(&parameters)))
		{
			throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));
		}
		deviceResetSucceeded = true;
		_rpT2 = EngQpcNow();

		ResetDeviceEffectsAfterReset();
		if (FAILED(RefreshPresentationParametersAfterReset()))
		{
			throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));
		}
		ReacquireDeviceResourcesAfterReset();
		_rpT3 = EngQpcNow();
	}
	catch (...)
	{
		// A successful device reset followed by a partial rebuild must release
		// that partial graph again on the next full attempt. The separate
		// m_fullResetPending gate keeps external D3D callers blocked meanwhile.
		if (deviceResetSucceeded) m_deviceResourcesReleased = false;
		m_presentSuspect = true;
		m_deviceResetInProgress = false;
		throw;
	}
	m_deviceResetInProgress = false;

	// count increments only on a completed reset (the device-Reset throw above
	// skips this), so the host's delta-per-second reads as successful resets.
	m_resetPerf.lastLostMs        = EngQpcUs(_rpT0, _rpT1) / 1000.0;
	m_resetPerf.lastDeviceResetMs = EngQpcUs(_rpT1, _rpT2) / 1000.0;
	const double reloadAndAlphaMs = EngQpcUs(_rpT2, _rpT3) / 1000.0;
	const double reloadOnlyMs =
	    reloadAndAlphaMs - m_resetPerf.lastAlphaResizeMs;
	m_resetPerf.lastReloadMs = reloadOnlyMs > 0.0 ? reloadOnlyMs : 0.0;
	m_resetPerf.lastTotalMs       = EngQpcUs(_rpT0, _rpT3) / 1000.0;
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
	if (m_pDevice == NULL || m_fatalDeviceState || m_fullResetPending ||
	    m_deviceResourcesReleased || m_deviceResetInProgress ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Recovering)
		return false;

	const LONGLONG _rpT0 = EngQpcNow();
	const bool retryingFailedResetEx =
	    m_deviceRecovery.phase == devicerecovery::Phase::ResetExFailed;
	m_deviceResetInProgress = true;

	D3DPRESENT_PARAMETERS parameters =
	    GetDeviceRecoveryPresentationParameters();
	const LONGLONG _rpT1 = EngQpcNow();
	HRESULT hr = m_pDevice->ResetEx(&parameters, NULL);
	if (FAILED(hr))
	{
		// After a failed ResetEx only CheckDeviceState, ResetEx, and Release are
		// legal. Record a distinct pending state so LayoutBroker cannot fall
		// through to ordinary Reset and no external D3D door can reopen.
		devicerecovery::RecordResetExFailure(m_deviceRecovery, hr);
		m_presentSuspect = true;
		m_deviceResetInProgress = false;
		char buf[96];
		sprintf(buf, "[Engine] ResetForResize: ResetEx failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
		OutputDebugStringA(buf);
		return false;
	}
	const LONGLONG _rpT2 = EngQpcNow();

	LONGLONG _rpT3 = _rpT2;
	LONGLONG _rpT4 = _rpT2;
	try
	{
		if (FAILED(RefreshPresentationParametersAfterReset()))
			throw wruntime_error(LoadString(IDS_ERROR_RENDERER_RESET));

		// ResetEx preserves these objects, so release them only after a
		// successful reset. A failed ResetEx therefore leaves the old render
		// graph intact while the pending coordinator waits to retry.
		ReleaseBloomTargets();
		ReleaseShadowMaskTargets();
		SAFE_RELEASE(m_pDistortTexture);
		SAFE_RELEASE(m_pSceneTexture);
		SAFE_RELEASE(m_pDepthStencilSurface);
		SAFE_RELEASE(m_pMsaaColor);
		SAFE_RELEASE(m_pMsaaDepth);
		m_msaaActive = false;
		SAFE_RELEASE(m_pEndFrameQuery);

		ResetParameters();
		_rpT3 = EngQpcNow();

		if (m_pAlphaCompositor &&
		    m_presentationParameters.BackBufferWidth > 0 &&
		    m_presentationParameters.BackBufferHeight > 0)
		{
			m_pAlphaCompositor->Resize(
			    static_cast<int>(m_presentationParameters.BackBufferWidth),
			    static_cast<int>(m_presentationParameters.BackBufferHeight));
		}
		_rpT4 = EngQpcNow();

		if (m_sceneViewportActive)
		{
			int sx = m_sceneViewportX;
			int sy = m_sceneViewportY;
			int sw = m_sceneViewportW;
			int sh = m_sceneViewportH;
			m_sceneViewportActive = false;
			SetSceneViewportUnchecked(sx, sy, sw, sh);
		}
	}
	catch (...)
	{
		// The ResetEx itself succeeded. A normal resize may safely fall back to
		// full Reset; a pending retry stays pending so ordinary Reset remains
		// forbidden and the coordinator can retry ResetEx later.
		m_deviceResetInProgress = false;
		if (!retryingFailedResetEx)
			devicerecovery::CompleteResetExRetry(m_deviceRecovery);
		throw;
	}
	if (retryingFailedResetEx)
		devicerecovery::CompleteResetExRetry(m_deviceRecovery);
	m_presentSuspect = false;
	m_deviceResetInProgress = false;

	m_resetPerf.lastLostMs        = EngQpcUs(_rpT0, _rpT1) / 1000.0;
	m_resetPerf.lastDeviceResetMs = EngQpcUs(_rpT1, _rpT2) / 1000.0;
	m_resetPerf.lastReloadMs      = EngQpcUs(_rpT2, _rpT3) / 1000.0;
	m_resetPerf.lastAlphaResizeMs = EngQpcUs(_rpT3, _rpT4) / 1000.0;
	m_resetPerf.lastTotalMs       = EngQpcUs(_rpT0, EngQpcNow()) / 1000.0;
	++m_resetPerf.count;
	++m_resetPerf.cheapCount;
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
		// Fresh RT contents are undefined — force one neutral clear
		// before the zero-heat skip may engage (see Render's heat pass).
		m_distortRtNeutral = false;

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

