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
#include "exceptions.h"
#include "ResourceLimits.h"   // kMaxTextureAssetBytes (asset-read size caps, #415)
#include "resource.h"
#include "ParticleSystemInstance.h"
#include "EmitterInstance.h"
#include "SphericalHarmonics.h"
#include "utils.h"     // WideToAnsi for custom-slot path bridging
#include "host/AlphaCompositor.h"
#include "host/Compositor.h"
using namespace std;


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
    if (DeviceCallsBlocked()) return nullptr;

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

// Both entry points take a RAW BORROW of an m_instances entry, and every
// Clear() frees the lot with no per-holder invalidation hook — see
// HasInstance() in engine.h. A holder that missed a Clear() therefore hands us
// freed memory, and both bodies used to deref it immediately (2026-07 audit:
// engine/action/clear, the SetEstimatedLoad overload hard-guard, and a
// gate-refused SpawnParticleSystem all reach Clear() without passing through
// the file/new + file/open teardown that nulls the host's Shift-preview slot).
//
// Re-validating here fixes the whole class at the point of deref rather than at
// each call site: a stale pointer becomes a no-op instead of undefined
// behavior, for every current AND future borrower. The scan is linear over a
// list the overload guard already keeps small, and neither entry point runs
// per-frame — both are user-gesture driven.
void Engine::DetachParticleSystem(ParticleSystemInstance* instance)
{
    if (!HasInstance(instance)) return;
    instance->Detach();
}

void Engine::KillParticleSystem(ParticleSystemInstance* instance)
{
	if (!HasInstance(instance)) return;
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
	// [D2] The killed instance's emitter teardown + count decay happen
	// inside Update's instance pass — make sure a paused frame runs it
	// (the kill doesn't change the top-level list size until then).
	InvalidatePausedIdleSkip();
}

void Engine::Clear()
{
	++m_particleSystemDocumentEpoch;
	m_deferredParticleSystemChange.Reset();
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

devicerecovery::Result Engine::ProbeDeviceRecovery()
{
	++m_deviceStateProbeCount;
	devicerecovery::D3D9ExRecoveryPort<IDirect3DDevice9Ex, Engine>
	    port(m_pDevice, *this);
	return devicerecovery::RunDeviceRecoveryStep(m_deviceRecovery, port);
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
	return PrepareDeviceForFrame(false);
}

bool Engine::PrepareComposedFrame()
{
	++m_composedFramePrepareCount;
	return PrepareDeviceForFrame(true);
}

bool Engine::PrepareDeviceForFrame(bool probeHealthyDevice)
{
	if (m_fatalDeviceState ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Recovering ||
	    m_deviceResetInProgress)
		return false;
	if (probeHealthyDevice ||
	    m_presentSuspect ||
	    m_deviceRecovery.phase == devicerecovery::Phase::ResetExFailed ||
	    m_fullResetPending)
	{
		if (!RecoverDeviceIfNeeded()) return false;
	}
	if (DeviceCallsBlocked()) return false;
	if (!ReplayPendingTextureReload()) return false;
	return ReplayPendingParticleSystemChange();
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

// [PERF] round-2 sub-profiling helpers — QPC microsecond deltas for the
// per-pass timing in Render(). Frequency is fixed for the process; cache it.
// Non-static: declared in engine_internal.h (render + reference TUs use it too).
LONGLONG EngQpcNow()
{
	LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart;
}
double EngQpcUs(LONGLONG a, LONGLONG b)
{
	static LONGLONG f = 0;
	if (f == 0) { LARGE_INTEGER q; if (QueryPerformanceFrequency(&q)) f = q.QuadPart; }
	return f ? static_cast<double>(b - a) * 1.0e6 / static_cast<double>(f) : 0.0;
}

IDirect3DTexture9* Engine::GetTexture(const string& name) const
{
	if (DeviceCallsBlocked()) return NULL;
	return GetTextureForDeviceReset(name);
}

IDirect3DTexture9* Engine::GetTextureForDeviceReset(const string& name) const
{
	return m_textureManager.getTexture(m_pDevice, name);
}

// Called from Reset() only. Deliberately NOT OnParticleSystemChanged(-1): that
// also recomputes composites and calls SyncRootEmitters, which can SPAWN
// emitters — a device reset must restore resources, not change the simulation.
void Engine::ReleaseInstanceTextures()
{
	for (auto& instance : m_instances)
	{
		instance->ReleaseDeviceTextures();
	}
}

void Engine::ReacquireInstanceTextures()
{
	for (auto& instance : m_instances)
	{
		instance->ReacquireDeviceTextures(*this);
	}
}

void Engine::OnParticleSystemChanged(int track)
{
	if (m_fatalDeviceState ||
	    m_deviceRecovery.phase == devicerecovery::Phase::Terminal)
		return;
	if (m_deferredParticleSystemChange.DeferIfBlocked(
	        DeviceCallsBlocked() || m_presentSuspect ||
	        m_textureReloadApplying, track))
		return;
	ApplyParticleSystemChanged(track);
}

void Engine::ApplyParticleSystemChanged(int track)
{
	++m_particleSystemChangeApplyCount;
	// track < 0 is the "everything changed" broadcast, which is what the
	// structural emitter handlers send. Re-sync placed instances against the
	// authored root list there: an instance only spawns roots in its
	// constructor, so Add Root / Paste / Import / Duplicate / reparent-to-root
	// never reached one placed earlier (2026-07 audit, an-audit-finding). Gated on track < 0
	// so a per-track curve edit doesn't pay for the scan.
	//
	// Safe on the system-REPLACEMENT paths (file/new, file/open, recover,
	// undo-apply): all four call Clear() first, so m_instances is empty here
	// and this is a no-op — they must, since each instance holds the
	// ParticleSystem by reference.
	const TimeF now = (track < 0) ? GetTimeF() : 0.0f;
	for (auto& instance : m_instances)
    {
		instance->onParticleSystemChanged(*this, track);
		if (track < 0) instance->SyncRootEmitters(now);
	}
	// [D2] Bust the paused-idle skip: while paused, on-screen particles
	// only reflect an edit when the Update loop re-evaluates their curves
	// at the frozen time — a paused curve/property edit must repaint now,
	// not on unpause. (-1 can never equal a real GetTimeF value.)
	m_lastUpdatedSimTime = -1.0f;
}

bool Engine::ReplayPendingTextureReload()
{
	if (!TextureReloadPendingForTesting()) return true;
	if (IsTerminalDeviceState())
	{
		m_textureReloadAppliedGeneration = m_textureReloadRequestGeneration;
		return false;
	}
	if (m_textureReloadApplying ||
	    DeviceCallsBlocked() ||
	    m_presentSuspect)
		return false;

	const uint64_t targetGeneration = m_textureReloadRequestGeneration;
	const uint64_t documentEpoch = m_particleSystemDocumentEpoch;
	int deferredTrack = -1;
	const bool hadDeferredChange =
	    m_deferredParticleSystemChange.Take(deferredTrack);

	m_textureReloadApplying = true;
	bool completed = false;
	try
	{
		completed = PerformTextureReload();
	}
	catch (...)
	{
		completed = false;
	}
	m_textureReloadApplying = false;

	if (!completed || !TextureReloadCanContinue())
	{
		if (IsTerminalDeviceState())
		{
			m_textureReloadAppliedGeneration =
			    m_textureReloadRequestGeneration;
			return false;
		}
		if (hadDeferredChange &&
		    documentEpoch == m_particleSystemDocumentEpoch)
			m_deferredParticleSystemChange.Queue(deferredTrack);
		return false;
	}

	m_textureReloadAppliedGeneration = targetGeneration;
	++m_textureReloadApplyCount;
	if (TextureReloadPendingForTesting()) return false;
	if (m_deferredParticleSystemChange.Pending()) return false;
	return true;
}

bool Engine::ReplayPendingParticleSystemChange()
{
	return m_deferredParticleSystemChange.Replay(
	    [this](int track) { ApplyParticleSystemChanged(track); },
	    [this]() { return DeviceCallsBlocked() || m_presentSuspect; });
}

void Engine::GetViewPort(D3DVIEWPORT9* viewport) const
{
	if (viewport == NULL) return;
	if (DeviceCallsBlocked())
	{
		ZeroMemory(viewport, sizeof(*viewport));
		return;
	}
	m_pDevice->GetViewport(viewport);
}

const Engine::Camera& Engine::GetCamera() const
{
	return m_eye;
}

void Engine::SetCamera( const Camera& camera )
{
	if (DeviceCallsBlocked()) return;

	// A camera move changes GetBillboardMatrix(), and screen-oriented
	// (!isWorldOriented) particles bake that matrix into their vertices on the
	// CPU in EmitterInstance::UpdateParticle. While the preview is paused the
	// sim clock is frozen, so the [D2] paused-idle skip in UpdateParticles would
	// otherwise elide the re-bake and the quads freeze edge-on as the view
	// orbits (#576). Force one more Update pass when the camera actually moves.
	// Guarded on a REAL change so a static paused frame (no orbit) still skips
	// and the idle-recompute elision is preserved.
	if (camera.Position != m_eye.Position
		|| camera.Target != m_eye.Target
		|| camera.Up     != m_eye.Up)
	{
		InvalidatePausedIdleSkip();
	}

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
    if (!m_pDirect3D || DeviceCallsBlocked()) return result;
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
	// with D3DERR_INVALIDCALL (2026-07 re-audit an-audit-finding).
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

// forwarder to the AlphaCompositor's shared
// HANDLE. Returns nullptr when the compositor isn't installed (canvas-
// jpeg mode skips the layered-window path) or before Resize has run.
// The DComp visual tree will consume this via a D3D11 OpenSharedResource;
// today nothing reads it but the standalone
// shared_texture_test exe verifies the handle is openable.
HANDLE Engine::GetSharedTextureHandle() const
{
	if (DeviceCallsBlocked()) return nullptr;
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
	if (DeviceCallsBlocked()) return;
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
	if (DeviceCallsBlocked()) return 0;
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
	if (m_pDirect3D == NULL || m_pDevice == NULL || DeviceCallsBlocked())
		return luid;

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
// (~5 lines) per the repo's "surgical changes" guidance rather than
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
	if (DeviceCallsBlocked()) return;
	SetSceneViewportUnchecked(x, y, w, h);
}

void Engine::SetSceneViewportUnchecked(int x, int y, int w, int h)
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
		// [D3] Fresh RT contents are undefined — force one neutral clear
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
		// Reset/ResetEx must run on the thread that created the device even
		// though D3DCREATE_MULTITHREADED permits ordinary cross-thread calls.
		m_deviceThreadId = GetCurrentThreadId();

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
