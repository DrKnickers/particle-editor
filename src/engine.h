#ifndef ENGINE_H
#define ENGINE_H

#include <string>
#include <vector>

#include "managers.h"
#include "ParticleSystem.h"
#include "utils.h"
#include "SkydomeEnvironment.h"   // SkydomeContext
#include "SkydomeMesh.h"          // game-faithful dome render core
#include "ReferenceObjectMesh.h"  // imported game-object render core
#include "GameObjectCatalog.h"    // enumerate game objects by Name
#include "RefLock.h"
#include <memory>
#include <atomic>    // off-UI-thread catalog build
#include <mutex>
#include <thread>

// Renderability verdict for the currently-selected reference object,
// surfaced in the engine-state snapshot so the picker can show "skinned -- not
// supported" / "couldn't load" for the chosen object (the .alo is probed lazily
// on select; see Engine::SetReferenceObject).
enum class ReferenceObjectStatus { None, Ok, Skinned, LoadFailed, ModelMissing };

///S49] Per-slot load outcome for a game skydome, surfaced in the
// engine-state snapshot so the Background picker can distinguish "no dome
// chosen" from "dome chosen but its .alo wouldn't load" (the latter silently
// falls through to the solid-colour background; see Engine::RenderSkydomes).
// Domes are always rigid, so there is no Skinned case.
enum class SkydomeSlotStatus { None, Ok, LoadFailed };

// One hardpoint attach model mounted on the selected reference object: its own
// rigid mesh + the UNIT Attachment_Bone's object-space matrix. Each of the attach
// mesh's sub-meshes draws at `sub.placement * boneMatrix * objectWorld`. Held by
// unique_ptr because ReferenceObjectMesh owns D3D resources (non-copyable/non-movable).
struct ReferenceAttachment
{
    ReferenceObjectMesh mesh;
    D3DXMATRIX          boneMatrix;
    ReferenceAttachment() { D3DXMatrixIdentity(&boneMatrix); }
};

namespace host { class AlphaCompositor; }

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

	// Preview overload guard: ceilings on the live simulation so no
	// authored spawn parameters (or chain multiplication — every spawned
	// particle with a life/death child allocates a whole child
	// EmitterInstance) can OOM the editor. Over budget the engine
	// SUPPRESSES spawning (existing particles live out their lives) and
	// latches an overload flag the UI surfaces; spawning resumes when the
	// population decays below the resume threshold (hysteresis so the
	// boundary doesn't flicker at the 4 Hz stats rate). Authored .alo
	// values are never clamped or modified.
	//
	// [guard-config] The budgets are RUNTIME state (SetOverloadGuard),
	// user-configurable from Preferences via engine/set/overload-guard.
	// Default 10k (user feel-tested down from 15k, which still made the
	// editor struggle on a large simultaneous burst; the old fixed 100k
	// survived the OOM but let the preview get far too heavy on the
	// climb). Disabled = fully uncapped (an explicit power-user choice —
	// CAN OOM on extreme chain effects; the per-instance uint16 index cap
	// below is a data-structure limit, not part of this guard, so the
	// unbounded dimension is instance count).
	static constexpr int kDefaultMaxPreviewParticles = 10'000;
	// One knob: the instance ceiling derives from the particle cap,
	// preserving #121's 100k:5k ratio (10k → 500 live instances —
	// vanilla effects run tens; raising the particle knob raises this).
	static constexpr int kInstancesDivisor           = 20;
	// Defensive clamp bounds for SetOverloadGuard — engine invariants
	// must not depend on UI-side validation (cap 0 would zero the spawn
	// budget forever and read as "editor broken"). 1M lets a power user
	// exceed the old 100k without going fully uncapped.
	static constexpr int kMinConfigurableParticles   = 1'000;
	static constexpr int kMaxConfigurableParticles   = 1'000'000;
	// Debounce on the latched overload flag: refusals only happen on
	// frames where a spawn round actually fires (e.g. every 0.1 s at
	// rate 10 while pinned at a cap), so the raw per-frame flag would
	// flicker ON/OFF between rounds. The latch clears only after this
	// long with no refusal at all.
	static constexpr float kOverloadClearDelaySec  = 0.5f;

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
	// Combined view*projection (recomputed wherever view or projection
	// changes — SetCamera, SetSceneViewport). Used to project the gizmo origin to
	// the React overlay for the in-drag readout pill.
	const D3DXMATRIX& GetViewProjection()     const { return m_viewProjection; }
	const D3DXMATRIX& GetViewMatrix()         const { return m_view; }
	const D3DXMATRIX& GetViewRotationMatrix() const { return m_viewRotation; }
	const D3DXMATRIX& GetBillboardMatrix()    const { return m_billboard; }
	void  GetViewPort(D3DVIEWPORT9* viewport) const;

	const Camera& GetCamera() const;
	void  SetCamera(const Camera& camera);

	bool     GetGround() const		{ return m_showGround; }
	// [seam fix] "Smooth skydome seams" preference. The setter re-applies to the
	// currently-loaded dome (re-runs RebuildSkydomeMeshes) so the toggle is live.
	bool     GetSkydomeSeamFix() const { return m_skydomeSeamFix; }
	void     SetSkydomeSeamFix(bool enable);
	// [shadow] "Model shadows" render preference (default on). Pure view toggle —
	// never marks the document dirty. Persisted web-side (localStorage).
	bool     GetModelShadows() const { return m_modelShadowsEnabled; }
	void     SetModelShadows(bool enable) { m_modelShadowsEnabled = enable; }
	// [soft-shadows] "Soft shadows" render preference (default on). Pure view
	// toggle — never marks the document dirty. Persisted web-side (localStorage).
	// Only meaningful when model shadows are on; falls back to the hard darken
	// path when off, or when the blur effect / mask RT is unavailable.
	bool     GetSoftShadows() const { return m_softShadowsEnabled; }
	void     SetSoftShadows(bool enable) { m_softShadowsEnabled = enable; }
	float    GetGroundZ() const		{ return m_groundZ; }
	int      GetGroundTexture() const { return m_groundTextureIndex; }
	//: main.cpp's thumbnail generator needs the D3D9 device to
	// create scratch textures via D3DXCreateTextureFromFile*Ex with
	// width/height clamped to 64×64. Exposed read-only.
	IDirect3DDevice9* GetDevice() const { return m_pDevice; }

	// Idempotent device-state guard. Mirrors the recovery dance the
	// Render() loop runs at the top of every frame:
	//   - TestCooperativeLevel == D3D_OK              → returns true (no-op).
	//   - TestCooperativeLevel == D3DERR_DEVICELOST   → returns false (caller
	//                                                   should retry later).
	//   - TestCooperativeLevel == D3DERR_DEVICENOTRESET → calls Reset() and
	//                                                     returns the result.
	// Call before any code path that creates D3D9 / D3DX9 resources off
	// the render thread. In --test-host mode the render loop isn't pumped
	// (hidden viewport HWND, no WM_PAINT), so resources allocated outside
	// of Render() must guard themselves. In interactive mode this is a
	// belt-and-suspenders no-op because Render() runs the same dance
	// every frame.
	bool RecoverDeviceIfNeeded();

	// install/clear the shared-RT compositor. When non-null, Render()
	// redirects slot-0 RT to the compositor's off-screen ARGB surface (the
	// shared-handle source the host's DComp path presents) and skips the
	// engine-side Present(). Pass nullptr to fall back to the swap-chain
	// Present path (used by viewport_poc / --capture and any host without a
	// compositor).
	void SetAlphaCompositor(host::AlphaCompositor* c) { m_pAlphaCompositor = c; }

	// Arm the eager game-object-catalog prefetch. The host calls this
	// once at startup so the reference-object picker's catalog builds in the
	// background before it's first opened. Defined in engine.cpp.
	void ArmCatalogPrefetch();

	// Phase 3 Stage 2: NT-handle alias of the engine's primary
	// render-target texture, openable from a parallel D3D11 device via
	// OpenSharedResource. Forwarded from m_pAlphaCompositor->GetShared
	// Handle() — the AlphaCompositor's offscreen RT is now a shared-
	// handle texture (Stage 2a promotion). Returns nullptr when the
	// compositor isn't installed (e.g. canvas-jpeg mode where
	// the engine renders to its native swap-chain back buffer) or when
	// Resize hasn't run yet. Stage 4 wires this into the DXGI / DComp
	// path; Stage 2 only exposes + verifies the handle.
	HANDLE GetSharedTextureHandle() const;

	// Phase 3 Stage 4a — cross-device GPU sync helpers.
	// Under composition mode (Stage 4+), HostWindow's per-frame loop
	// calls these between engine->Render() (D3D9 draws into the shared
	// texture) and m_compositor->CompositeEngineFrame() (D3D11
	// CopyResource from alias to swapchain back buffer). Without the
	// spin, the D3D11 read may race against in-flight D3D9 writes —
	// symptoms: tearing, one-frame-stale appearance, half-frame updates.
	//
	// Production port of dxgi_spike.cpp:687-697 with the same 100k-
	// iteration spin cap. Spike measured 0.30 ms total at 3440x1440;
	// the spin doesn't dominate. Sub-plan §3.3 path (b): Engine owns
	// the query (it has the D3D9 device anyway), host orchestrates the
	// call sites under composition mode only — zero overhead on the
	// non-composition paths (arch-A, canvas-jpeg) which never call.
	//
	// Lazy creation on first Issue. m_pEndFrameQuery is released in
	// Engine::Reset before m_pDevice->Reset (queries aren't D3DPOOL_*
	// but DO get invalidated by IDirect3DDevice9::Reset under D3D9Ex).
	// Next Issue lazy-recreates against the post-Reset device.
	void IssueEndFrameQuery();
	// Returns the number of GetData spins it busy-waited (0 = signalled on
	// the first poll), so the host can log GPU-wait pressure ([PERF]).
	int  WaitEndFrameQuery();

	// [PERF] round-2 sub-profiling — per-pass CPU-submit timing (us) of the
	// last Render() call; the host folds these into the [PERF2] host.log
	// line. `present` includes the AlphaCompositor::Composite() synchronous
	// readback. Diagnostic-only; see tasks/todo.md.
	struct RenderPassTimingsUs { double scene = 0, bloom = 0, distort = 0, composite = 0, present = 0; };
	RenderPassTimingsUs GetLastRenderTimings() const { return m_lastRenderTimings; }
	RenderPassTimingsUs m_lastRenderTimings = {};

	// [resize-perf] Phase-0 probe — per-Reset() sub-stage wall-clock (ms)
	// plus a monotonic call counter, so the host's 1 Hz [resize-perf]
	// log line can show the device-reset storm during window resize and
	// size the A2 (cheap settle-reset) payoff. Same diagnostic pattern as
	// RenderPassTimingsUs above; see tasks/resize-perf-investigation.md.
	// `lost` = OnLostDevice + releases + texture-cache wipe (pre-Reset);
	// `reload` = shader OnReset + skydome/ground re-decode + ResetParameters;
	// `alpha` = AlphaCompositor::Resize (shared RT + SYSTEMMEM + DIB rebuild).
	struct ResetPerf
	{
		unsigned count      = 0;       // completed resets (full + cheap)
		unsigned cheapCount = 0;       // of which: ResetForResize (ResetEx path)
		// For a cheap reset: lost = size-keyed releases, dev = ResetEx,
		// reload = ResetParameters (RT/depth/bloom rebuild), alpha = same.
		double lastTotalMs       = 0.0;
		double lastLostMs        = 0.0;
		double lastDeviceResetMs = 0.0;
		double lastReloadMs      = 0.0;
		double lastAlphaResizeMs = 0.0;
	};
	const ResetPerf& GetResetPerf() const { return m_resetPerf; }
	ResetPerf m_resetPerf = {};

	// Phase 3 Stage 4b — adapter LUID for the multi-GPU
	// guard. Compositor::AttachEngineVisual compares this against
	// the D3D11 device's adapter LUID; on mismatch (hybrid laptops
	// where D3D9Ex and D3D11 picked different physical GPUs),
	// shared-handle opens silently return a wrong texture, so
	// AttachEngineVisual logs + skips engine attach. Single-GPU
	// systems (engine's RTX 3080 target) return matching LUIDs;
	// the check is a no-op there. Returns LUID{0,0} on failure
	// (no device, GetCreationParameters fails, GetAdapterLUID
	// fails) — Compositor treats zero LUID as "caller doesn't
	// know" and skips the comparison.
	LUID GetAdapterLuid() const;

	// Phase 3 Stage 5 — scene-rect viewport (Variant B-γ).
	//
	// Under composition mode, LayoutBroker calls this on every
	// React-side layout/scene-rect dispatch (gated on a non-null
	// DComp Compositor pointer per LayoutBroker R9 mitigation). The
	// (x, y, w, h) is in main-host-client coords, which equals the
	// engine RT's coordinate space (the engine RT is currently sized
	// to full host client per the popup-spans-window invariant).
	//
	// Side effects:
	//   1. Cache the rect + activate flag.
	//   2. Recompute m_projection with the scene-rect aspect ratio
	//      (sceneW / sceneH) via D3DXMatrixPerspectiveFovRH — otherwise
	//      the scene gets stretched when scene-rect aspect ≠ RT aspect.
	//   3. Next Engine::Render's scene pass will SetViewport(scene-rect)
	//      after the full-RT Clear (the D12 ordering rule from sub-plan
	//      §3.4 — Clear-then-SetViewport prevents post-process bleed
	//      across the scene-rect boundary).
	//
	// Passing w<=0 or h<=0 clears the scene viewport: m_projection
	// is recomputed at full-RT aspect (matches Engine::Reset's default
	// setup) and Render skips the SetViewport call. Used by callers
	// when composition mode detaches.
	//
	// Idempotent on identical args. Emits [engine] SetSceneViewport
	// log lines on actual changes via host.log (when wired).
	//
	// Survives Engine::Reset (Reset re-applies the cached rect after
	// rebuilding m_projection at full-RT aspect — sub-plan R8
	// mitigation). Non-composition transports (canvas-jpeg, arch-A)
	// never call this so m_sceneViewportActive stays false and Render
	// behaves identically to today.
	void SetSceneViewport(int x, int y, int w, int h);

	// Diagnostic accessor — returns true and populates the outs when
	// a scene viewport is active; returns false (outs untouched)
	// otherwise.
	bool GetSceneViewport(int& x, int& y, int& w, int& h) const;

	const std::wstring& GetGroundSlotCustomPath(int slot) const;
	// Does the slot currently have a loadable texture (either bundled
	// default or user-supplied custom path)? Used by the picker dialog
	// to decide whether single-click selects vs. opens the file picker
	// and whether the toolbar preview button is enabled.
	bool     IsGroundSlotEmpty(int slot) const;

	//: number of ground texture slots — 5 bundled defaults
	// (Dirt, Grass, Sand, Snow, Solid Color) + 3 user-customisable
	// slots. Total 8, laid out as a 4×2 grid in the picker dialog.
	// Slot 4 is the procedural solid-colour ground driven by
	// m_groundSolidColor, with a colour picker as its "edit" gesture
	// instead of a file picker. Slot index 0 is the v1 dirt default.
	static const int kGroundTextureCount        = 8;
	static const int kGroundTextureBundledCount = 5;
	static const int kGroundSolidColorSlot      = 4;   // 0-based; "Solid Color" slot
	static const int kGroundThumbnailSize       = 64;

	//: skydome slot layout (dialog and engine share these).
	// 0=Off, 1-8=bundled scenes, 9-11=user-supplied custom paths.
	static const int kSkydomeSlotCount       = 12;
	static const int kSkydomeBundledCount    = 9;   // Off + 8 scenes
	static const int kSkydomeFirstCustomSlot = 9;
	static const int kSkydomeOffSlot         = 0;
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

	//: read-only access to lighting state for the Lighting dialog's
	// startup seed and WM_USER reseed-from-engine path after Reset View
	// Settings. The dialog itself owns the UI representation (RGB +
	// intensity + angles); these getters are used only to read back what
	// was last pushed.
	const Light&       GetLight(LightType which) const;
	const D3DXVECTOR4& GetAmbient() const { return m_ambient; }
	const D3DXVECTOR4& GetShadow()  const { return m_shadow;  }

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
    // numParticles is a (negative) live-particle delta for paths that
    // destroy an instance which still holds live particles (see
    // ParticleSystemInstance::RemoveEmitter). Death-by-decay paths pass
    // the default 0: IsDead() implies m_primitives is already empty.
    void OnEmitterDestroyed(int numParticles = 0) { m_numEmitters--; m_numParticles += numParticles; }

    // --- Preview overload guard (see kDefaultMaxPreviewParticles) ---
    // Per-particle gate: spend one unit of the per-frame spawn budget.
    // Refusal flags this frame as overloaded; the caller drops the spawn.
    // Disabled guard: always allow — uncapped is uncapped.
    bool TryConsumeSpawnBudget()
    {
        if (!m_overloadGuardEnabled) return true;
        if (m_spawnBudget > 0) { m_spawnBudget--; return true; }
        m_overloadThisFrame = true;
        return false;
    }
    // Per-instance gate: refuse new EmitterInstances past the cap. No
    // decrement needed — m_numEmitters is kept live by OnEmitterCreated /
    // OnEmitterDestroyed (instance-death erase paths call the latter).
    bool TryConsumeInstanceBudget()
    {
        if (!m_overloadGuardEnabled) return true;
        if (m_numEmitters < m_maxPreviewInstances) return true;
        m_overloadThisFrame = true;
        return false;
    }
    // Cheap loop-exit check for spawn catch-up loops: once the budget is
    // gone there is no point iterating spawn rounds that can't spawn.
    bool SpawnBudgetExhausted() const
    {
        return m_overloadGuardEnabled && m_spawnBudget <= 0;
    }
    // Catch-up loops that bail via SpawnBudgetExhausted() never reach a
    // TryConsume* refusal, so they must register the suppression here or
    // the latch would clear while spawning is still being suppressed.
    void NoteSpawnSuppressed() { m_overloadThisFrame = true; }
    // Latched flag the UI reads (stats/tick). True while any spawn was
    // suppressed during the last completed Update.
    bool IsSpawnOverloadActive() const { return m_overloadActive; }
    // [guard-config] Configure the preview overload guard at runtime.
    // maxParticles is clamped DEFENSIVELY to
    // [kMinConfigurableParticles, kMaxConfigurableParticles] — engine
    // invariants must not depend on UI-side validation. Disabling clears
    // the latch immediately so the overload banner doesn't linger after
    // the user opts out.
    void SetOverloadGuard(bool enabled, int maxParticles);

    // [hard-guard] One-shot spawn-refusal record polled by the host's
    // 4 Hz stats path and emitted as engine/overload/refused. Declared
    // here (public) so the dispatcher can name Engine::SpawnRefusal; the
    // pending flag + record live with the other guard state members.
    struct SpawnRefusal { double estimated; int cap; int attemptedCount; };
    // [hard-guard] Set the web-computed estimate of alive particles per
    // placed instance (clamped >= 0, non-finite -> 0). Re-runs the
    // edit-time check: if the guard is enabled and the already-placed
    // preview now exceeds the cap, Clear() + record a refusal.
    void SetEstimatedLoad(double perInstance);
    // [hard-guard] Returns true once per refusal and clears the record
    // (4 Hz poll). out may be null.
    bool TakeSpawnRefusal(SpawnRefusal* out);

	void SetBackground(COLORREF color);
	void SetLight(LightType which, const Light& light);
	void SetAmbient(const D3DXVECTOR4& color);
	void SetShadow(const D3DXVECTOR4& color);
	void SetWind(const D3DXVECTOR3& wind);
	void SetGravity(const D3DXVECTOR3& gravity);
	void SetGround(bool enable);
	void SetGroundZ(float z);
	//: pick one of the ground texture slots (0..kGroundTextureCount-1).
	// Returns true on success; false if index is out of range, the slot
	// is empty (no bundled default AND no user-supplied path), or the
	// texture failed to load. On failure of a non-default index, the
	// engine retries with index 0 (dirt) once; UI should call
	// GetGroundTexture() afterward to re-sync visuals to the
	// actually-loaded slot.
	bool SetGroundTexture(int index);

	//: the procedural solid-colour ground (slot kGroundSolidColorSlot).
	// SetGroundSolidColor regenerates a 1×1 D3D texture at the new
	// colour and, if that slot is currently selected, refreshes the
	// engine's m_pGroundTexture. Persisted by main.cpp via
	// HKCU\Software\AloParticleEditor\GroundSolidColor (REG_DWORD).
	COLORREF GetGroundSolidColor() const { return m_groundSolidColor; }
	bool     SetGroundSolidColor(COLORREF color);

	//: assign a user-supplied texture file to the given slot.
	// Slots 0-5 already have bundled defaults; setting a custom path
	// on them overrides the default. Slots 6-11 start empty; a custom
	// path is what populates them. Setting an empty path reverts to
	// the bundled default (slots 0-5) or empties the slot (slots 6-11).
	// If the slot is currently selected, the engine re-loads the
	// texture immediately so the preview reflects the new content.
	// Returns true on success (or success of the fallback if the new
	// path failed to load); false on out-of-range slot index.
	bool SetGroundSlotCustomPath(int slot, const std::wstring& path);

	//: skydome slot selection and custom-path management.
	// Slot 0 = Off, slots 1-8 = bundled scenes, slots 9-11 = user-supplied paths.
	int  GetSkydomeSlot() const { return m_skydomeIndex; }
	bool SetSkydomeSlot(int index);

	// Select real game/mod skydomes by GameObject Name for the given
	// battle context. An empty name clears that slot. Loads + resolves both
	// meshes immediately when the device is up (no-op resolve otherwise).
	void SetSkydomeEnvironment(SkydomeContext context,
	                           const std::string& primaryName,
	                           const std::string& secondaryName);
	SkydomeContext     GetSkydomeContext()       const { return m_skydomeContext; }
	const std::string& GetSkydomePrimaryName()   const { return m_skydomePrimaryName; }
	const std::string& GetSkydomeSecondaryName() const { return m_skydomeSecondaryName; }
	// Load outcome of each selected dome (set in RebuildSkydomeMeshes); the
	// picker surfaces LoadFailed instead of silently falling back to solid colour.
	SkydomeSlotStatus  GetSkydomePrimaryStatus()   const { return m_skydomePrimaryStatus; }
	SkydomeSlotStatus  GetSkydomeSecondaryStatus() const { return m_skydomeSecondaryStatus; }
	// Enumerate selectable dome Names for a battle context (primary + secondary).
	// Non-const: reads the lazily-built EnsureSkydomeLists() cache.
	void EnumerateSkydomeNames(SkydomeContext context,
	                           std::vector<std::string>& outPrimary,
	                           std::vector<std::string>& outSecondary);
	// Drop the cached skydome lists so the next access re-reads the skydome XML from
	// disk. The cache otherwise rebuilds only on a mod/submod switch; the explicit
	// reload-shaders/reload-textures actions call this so a modder who edited a
	// *Skydomes.xml (or GameObjectFiles.xml) sees the change without a mod switch.
	void InvalidateSkydomeListCache() { m_skydomeListsValid = false; }
	const std::wstring& GetSkydomeCustomPath(int slot) const;
	bool SetSkydomeCustomPath(int slot, const std::wstring& path);
	bool IsSkydomeSlotEmpty(int slot) const;
	// Returns the file-scope RCDATA resource-ID table (length kSkydomeBundledCount).
	// Slot 0 entry is 0 (Off — no texture); slots 1-8 map to IDR_SKYDOME_* constants.
	static const int* GetSkydomeBundledResources();

	// follow-up: parallel table of in-archive paths for slots 1-8 — what
	// the FileManager should look up first when restoring the slot's texture.
	// Slot 0 entry is NULL (Solid colour — no asset). Used by the picker
	// thumbnail builder so its resolution chain matches Engine's.
	static const char* const* GetSkydomeBundledGamePaths();

	// Imported reference object (a game/mod object placed in the preview as
	// a scale reference). SetReferenceObject resolves `name` via the GameObject
	// catalog, lazily probes the .alo for renderability (skinned / load-failed are
	// reported via GetReferenceObjectStatus and NOT rendered), and rebuilds the
	// mesh. An empty name clears the object. Transform rotation is degrees,
	// ordered [yaw, pitch, roll] (D3DXMatrixRotationYawPitchRoll).
	void SetReferenceObject(const std::string& name);
	void SetReferenceObjectVisible(bool visible) { m_referenceObjectVisible = visible; }
	void SetReferenceObjectTransform(const D3DXVECTOR3& position, const D3DXVECTOR3& rotationDeg)
	     { m_referencePosition = position; m_referenceRotation = rotationDeg; }
	const std::string&    GetReferenceObjectName()    const { return m_referenceObjectName; }
	bool                  GetReferenceObjectVisible() const { return m_referenceObjectVisible; }
	const D3DXVECTOR3&    GetReferencePosition()      const { return m_referencePosition; }
	const D3DXVECTOR3&    GetReferenceRotation()      const { return m_referenceRotation; }
	ReferenceObjectStatus GetReferenceObjectStatus()  const { return m_referenceObjectStatus; }
	// [capture] Number of shadow-volume sub-meshes on the loaded reference object
	// (plus any attachments). Used by --capture-ref to warn when the object has no
	// shadow geometry so the operator knows the captured image will show no shadow.
	size_t ReferenceShadowSubMeshCount() const;

	// [capture] World-space AABB of the loaded reference object: takes the
	// object-space AABB from ReferenceObjectMesh::GetBoundingBox, transforms all
	// 8 corners by ReferenceObjectWorld(), and returns the enclosing world-space
	// min/max. Used by --capture-ref to frame the whole object with a fit camera.
	// False when no object is loaded / unresolved / has no bounds.
	bool GetReferenceObjectBounds(D3DXVECTOR3& outMin, D3DXVECTOR3& outMax) const;

	// Enumerate selectable game objects (Name + category) for the active mod/base.
	// Kicks the background build on first call; returns the units/structures
	// subset that's ready (empty while still building).
	void EnumerateReferenceObjects(std::vector<GameObjectRef>& out);

	// Whether the game-object catalog has finished building. The bridge returns
	// "building" (picker shows "Loading objects…") until this is true; it flips false
	// again on a mod/submod switch while the background rebuild runs.
	bool IsReferenceCatalogReady() const { return m_referenceCatalogBuilt; }

	// [reference-model-shadows] Build the GameObject catalog SYNCHRONOUSLY on
	// the calling thread (no worker, no UI handoff). For headless --capture-ref
	// runs only: there's no UI thread to freeze and no concurrent FileManager
	// access to race, so the isolated-FileManager dance the async path uses
	// isn't needed. No-op once built. Lets SetReferenceObject resolve inline
	// instead of deferring to a later Update() the one-shot run never reaches.
	void BuildCatalogSync();

	// Whether a catalog build is wanted but not yet ready -- surfaced in the
	// engine-state snapshot (referenceCatalogBuilding) so the picker shows "Loading
	// objects…" AND knows when to re-query (the build-ready transition), without
	// re-querying on every unrelated engine/state/changed event.
	bool IsReferenceCatalogBuilding() const { return m_catalogWanted && !m_referenceCatalogBuilt; }

	// Called by the host right after Update(): returns (and clears) true once
	// when a finished catalog was just swapped in, so the host emits engine/state/changed
	// and an open picker re-queries its now-ready object list.
	bool ConsumeCatalogReadyFlag();

	// Unit grid. State + setters land here (); RenderUnitGrid is.
	void  SetGridVisible(bool visible) { m_gridVisible = visible; }
	void  SetGridSpacing(float spacing);
	bool  GetGridVisible()  const { return m_gridVisible; }
	float GetGridSpacing()  const { return m_gridSpacing; }

	// Persistent gizmo snap toggle. State + round-trip land here; the
	// drag-time snap apply (reads GetSnapEnabled) is a separate task.
	void  SetSnapEnabled(bool v) { m_snapEnabled = v; }
	bool  GetSnapEnabled()  const { return m_snapEnabled; }

	// / S47] In-viewport manipulator (grab a handle, drag to move/rotate
	// the reference object). A handle is one of 3 translate arrows or 3 rotate rings,
	// identified by a (kind, axis) pair; axis 0=X/1=Y/2=Z.
	struct ManipHandle
	{
		enum Kind { NONE, TRANSLATE, ROTATE, PLANE };   // PLANE: ground-plane (XY) drag; axis = NORMAL axis
		Kind kind = NONE;
		int  axis = -1;
	};

	// RenderReferenceManipulator draws the 3 arrows + 3 rings (called from Render).
	// PickManipulatorHandle ray-picks the handle under the cursor, scoring all 6
	// candidates by miss/threshold and returning the best (kind=NONE on a miss; an
	// arrow wins ties). ManipulatorAxisParam returns the signed distance along a
	// translate `axis` from `anchor` to the cursor ray's closest point (false when
	// the ray is ~parallel to the axis). ManipulatorRingAngle returns the cursor's
	// angle (radians) around a rotate ring in that ring's world-axis plane (false
	// when the ray grazes the plane). The host uses these for the no-jump grab.
	// BuildCursorRay is the shared screen->world unproject (also used by
	// GetCursorPos3D). Screen coords are popup-client physical px.
	void RenderReferenceManipulator();
	ManipHandle PickManipulatorHandle(short screenX, short screenY) const;
	bool ManipulatorAxisParam(short screenX, short screenY, int axis,
	                          const D3DXVECTOR3& anchor, float& outParam) const;
	bool ManipulatorRingAngle(short screenX, short screenY, int axis,
	                          float& outAngleRad) const;
	// In-plane offset (u,v) of the cursor ray's intersection with the plane
	// through `anchor` whose normal is world axis `normalAxis`, in that plane's
	// (normalAxis+1, normalAxis+2) basis. False on grazing / behind-camera. Like
	// ManipulatorAxisParam, the anchor is EXPLICIT: pick passes the current object
	// position, but a drag MUST pass the FIXED grab position (m_manipStartPos) -- using
	// the live, moving object origin feeds its own motion back in and flickers.
	bool ManipulatorPlaneOffset(short screenX, short screenY, int normalAxis,
	                            const D3DXVECTOR3& anchor, float& outU, float& outV) const;
	void BuildCursorRay(short screenX, short screenY,
	                    D3DXVECTOR3& outOrigin, D3DXVECTOR3& outDir) const;
	// polish] The gizmo only shows + is grabbable when the object is
	// SELECTED (auto-selected on pick; click the object body to re-select; click
	// empty to deselect). PickReferenceObject (S46) ray-tests the object's
	// object-space AABB for the body-click. SetManipulatorHover lets the host
	// highlight the handle under the cursor (set each idle mouse-move; NONE = none).
	void SetReferenceObjectSelected(bool selected) {
		m_referenceObjectSelected = RefLockResolveSelected(selected, m_referenceLocked);
		if (!m_referenceObjectSelected) { m_hoverManip = ManipHandle(); m_activeManip = ManipHandle(); }
	}
	bool IsReferenceLocked() const { return m_referenceLocked; }
	// [gizmo-drag-teardown] So the host can self-abort an in-flight gizmo drag when
	// an out-of-band mutation (clear / deselect / lock) deselects the object.
	bool IsReferenceObjectSelected() const { return m_referenceObjectSelected; }
	// freeze/lock] Setting the lock re-resolves the CURRENT selection under the
	// new state: locking deselects (hides the gizmo + clears manip hover/active);
	// unlocking leaves the object deselected until the user clicks it again.
	void SetReferenceLocked(bool locked) {
		m_referenceLocked = locked;
		SetReferenceObjectSelected(m_referenceObjectSelected);
	}
	void SetManipulatorHover(ManipHandle h)         { m_hoverManip = h; }
	// The host pushes the active drag handle on grab + per rotate-move so the renderer can draw
	// the axis guide line / rotate sweep and dim the other handles; cleared (NONE) at every drag-end.
	void SetManipulatorActiveDrag(ManipHandle h, float grabRad, float appliedRad)
	{ m_activeManip = h; m_activeGrabAngle = grabRad; m_activeAppliedAngle = appliedRad; }
	bool PickReferenceObject(short screenX, short screenY) const;

	void SetHeatDebug(bool debug);
	void SetBloom(bool enable);
	void SetBloomStrength(float v);
	void SetBloomCutoff(float v);
	void SetBloomSize(float v);

	// [runtime-MSAA] Store the user's preferred MSAA sample count and mark
	// surfaces dirty for rebuild on the next render frame. Safe to call from
	// any thread — no D3D calls made here. Valid values: 0 (off), 2, 4, 8;
	// any other value is silently treated as 0.
	void             SetMsaaLevel(int samples);
	// [runtime-MSAA] Returns {0} (Off always) plus each of {2,4,8} that the
	// device supports for both D3DFMT_A8R8G8B8 colour and the current depth
	// format, in ascending order. Returns {0} if m_pDirect3D is null.
	std::vector<int> GetSupportedMsaaLevels() const;
	// [runtime-MSAA] Currently-applied sample count (0 = off / not yet applied).
	int              GetCurrentMsaaLevel() const { return m_currentMsaaLevel; }

	void				Reset();

	// [resize-perf revised Fix A] Cheap RESIZE-ONLY reset via
	// IDirect3DDevice9Ex::ResetEx. Per first-party docs (ResetEx, d3d9.h):
	// "Resets the type, size, and format of the swap chain with all other
	// surfaces persistent" / "does not cause surfaces, textures or state
	// information to be lost" / shaders "do not need to be re-created".
	// So unlike Reset() this skips the OnLostDevice dance, the
	// texture-cache wipe, and the ground/skydome re-decode (~20 ms of the
	// ~24 ms full reset) — only the size-keyed targets are rebuilt
	// (scene/distort/bloom RTs + depth-stencil via ResetParameters, the
	// AlphaCompositor shared RT via Resize). ~3-5 ms, cheap enough to run
	// on EVERY sizemove tick so the scene always renders at the correct
	// size (no settle snap). Returns false on ResetEx failure — the device
	// is then in the lost state and the caller falls back to the full
	// Reset() / RecoverDeviceIfNeeded path. NOT for device-loss recovery;
	// Reset() remains the recovery primitive.
	bool				ResetForResize();

	Engine(HWND hFocus, HWND hDevice, ITextureManager& textureManager, IShaderManager& shaderManager, IFileManager& fileManager);
	~Engine();

private:
	D3DMULTISAMPLE_TYPE GetMultiSampleType(DWORD* MultiSampleQuality, D3DFORMAT DisplayFormat, D3DFORMAT DepthStencilFormat, BOOL Windowed);
	D3DFORMAT           GetDepthStencilFormat(D3DFORMAT AdapterFormat, bool withStencilBuffer);
	void				ResetParameters();
	// [runtime-MSAA] Release + recreate MSAA surfaces resolving m_msaaPreferredLevel
	// to the highest supported level <= the preference. Render-thread only.
	void				ApplyMsaaLevelNow();

	///Screen-uniform gizmo handle length (world units), sized so the
	// reference-object gizmo holds a constant on-screen pixel size at its origin.
	// Reads m_sceneFovY / m_sceneViewportH / m_sceneViewportActive + the camera; see GizmoSizing.h.
	float				ReferenceGizmoHandleLength() const;

	// Helper used by both the constructor and ReloadShaders(): scans the
	// freshly-loaded shader's parameters for "texture_filename" annotations
	// and binds the named textures.
	void				BindShaderTextures(Effect* shader);

	//: shared loader used by the constructor, lost-device recovery,
	// and SetGroundTexture. Releases m_pGroundTexture (if any) and
	// re-creates from the resource ID at kResourceIds[m_groundTextureIndex].
	// On non-default-index failure, retries with index 0 once. Returns
	// false only if the default also fails (engine is in trouble).
	bool				ReloadGroundTexture();

	// Introspects the freshly-loaded SceneBloom effect to (a) verify it
	// isn't the ShaderManager default fallback, (b) cache D3DXHANDLEs
	// for the parameters we drive each frame, and (c) classify each
	// technique by name pattern. Sets m_bloomReady on success.
	void				InitBloomEffect();

	// Releases any half-resolution bloom RTs. Called from Reset() and
	// from ResetParameters() before reallocation.
	void				ReleaseBloomTargets();

	// [soft-shadows] Introspects the freshly-loaded StencilDarkenFinalBlur
	// effect: verifies it isn't the ShaderManager default, caches the blurAmt /
	// WORLDVIEWPROJECTION handles + the first technique that validates, and sets
	// m_shadowBlurReady. Mirrors InitBloomEffect's all-or-nothing handle gate.
	void				InitShadowBlurEffect();

	// [soft-shadows] Releases the screen-space shadow-mask render targets.
	// Mirrors ReleaseBloomTargets; called from every site that releases the
	// bloom targets (Reset / ResetForResize / ResetParameters / dtor).
	void				ReleaseShadowMaskTargets();

	//: build the UV sphere VB/IB/Decl once at engine init.
	void				InitSkydomeMesh();
	// Phase 3 Stage 1: split out the VB/IB allocation + fill so
	// Engine::Reset can recreate them post-device-Reset (D3DPOOL_DEFAULT
	// no longer survives Reset, unlike the original D3DPOOL_MANAGED).
	void				CreateSkydomeMeshBuffers();
	void				ReleaseSkydomeMeshBuffers();
	//: compile IDR_SHADER_SKYDOME from RCDATA and cache parameter handles.
	void				InitSkydomeEffect();
	//: release m_pSkydomeTexture and re-load from slot (bundled or custom).
	bool				ReloadSkydomeTexture(int slot);
	//: draw the skydome sphere, camera-locked, depth off, cull CW.
	// Called from Render() when slot != Off and effect/texture are ready.
	void				RenderSkydome();

	// Game-faithful skydome. Load the real primary/secondary .alo dome
	// meshes for the current selection and render each sub-mesh with its own
	// named game shader 1:1 (Skydome.fx / MeshGloss.fxo / MeshAdditive.fx, ...).
	// RebuildSkydomeMeshes re-drives Load->Resolve->CreateBuffers for both slots
	// from m_skydome*Name (called on selection change + mod-switch). RenderSkydomes
	// replaces the single RenderSkydome() call site: it draws the game domes when
	// present, else falls back to the simple-background sphere.
	void				RebuildSkydomeMeshes();
	void				RenderSkydomeMesh(SkydomeMesh& mesh, const D3DXMATRIX& world);
	void				RenderSkydomes();

	// [#NN] Return the cached per-axis skydome lists, rebuilding them (one
	// GameObjectFiles pass via LoadAllSkydomeLists) only when the FileManager's
	// mod/submod context has changed since the cache was last built. Order-
	// independent: re-checks context on every call, so it is safe whether the first
	// post-switch caller is RebuildSkydomeMeshes or EnumerateSkydomeNames.
	const std::array<std::vector<SkydomeRef>, kNumSkydomeAxes>& EnsureSkydomeLists();

	// Resolve m_referenceObjectName -> catalog model path -> lazy skinned
	// probe -> Load/Resolve/CreateBuffers (clone of RebuildSkydomeMeshes). Sets
	// m_referenceObjectStatus. If the catalog isn't built yet (startup restore
	// / just-invalidated on a mod switch), defers until the background build finishes
	// (Update() retries) -- never builds synchronously, so it can't freeze the UI thread.
	void				RebuildReferenceObjectMesh();

	// Kick a background catalog (re)build if one is wanted and not already
	// built/in-flight. Snapshots the FileManager roots on the calling (UI) thread and
	// spawns a worker with an ISOLATED FileManager. Called from Update() once a build
	// is wanted; safe to call every frame (early-returns when built/building).
	void				StartCatalogBuildIfNeeded();

	// Draw the imported reference object in two phases (opaque then
	// transparent) -- each rigid sub-mesh placed by its bone, running its own game
	// shader 1:1, blended per its phase/blend class. No-op when empty/unresolved.
	void				RenderReferenceObject();
	void				RenderReferenceShadows();        // [shadow] stencil shadow-volume pass

	// Live reference-object world (Z-up yaw/pitch/roll then translate). The
	// PICK uses the committed transform (ReferenceObjectWorld); the RENDER uses the
	// eased "display" transform (ReferenceObjectDisplayWorld) -- smooth motion.
	D3DXMATRIX			ReferenceObjectWorld() const;          // committed (pick)
	D3DXMATRIX			ReferenceObjectDisplayWorld() const;   // eased (render)
	D3DXMATRIX			ReferenceObjectWorldFrom(const D3DXVECTOR3& pos, const D3DXVECTOR3& rotDeg) const;
	void				EaseReferenceDisplay();                // per-frame ease display -> committed

	// Draw the object's AABB as a depth-tested wireframe when selected -- the
	// same box the click-pick (PickReferenceObject) hit-tests. No-op when unselected.
	void				RenderReferenceSelectionBox();

	// Unit grid (): the engine's first line-list primitive. RenderUnitGrid
	// draws axis-aligned world lines at m_gridSpacing over a fixed extent, co-planar
	// with the ground (z-test on, z-write off), with a brighter line every 5 cells.
	// (The reusable fixed-function D3DPT_LINELIST helper, DrawWorldLines, is a
	// file-static in engine.cpp -- it references EmitterInstance::Vertex, which is
	// only forward-declared here.)
	void				RenderUnitGrid();

	//: compile IDR_SHADER_GROUND_LIT from RCDATA, cache parameter
	// handles, and build the tangent-space ground vertex declaration.
	void				InitGroundEffect();
	//: release + reload the companion `<base>_bc` normal map for the
	// active ground slot (game/mod via FileManager); flat-normal on miss.
	void				ReloadGroundNormalTexture();
	//: create the 1px (128,128,255) neutral tangent-space normal used
	// when a slot has no companion normal map.
	void				CreateGroundFlatNormal();
	//: draw the lit ground quad through m_pGroundEffect. Called from
	// Render() when the effect is ready; else the unlit FF quad is used.
	void				RenderGroundLit();

	//
	// Data members
	//

	// Particle management
    std::vector<std::unique_ptr<ParticleSystemInstance>> m_instances;
    int m_numParticles;
    int m_numEmitters;

    // Preview overload guard state (see kDefaultMaxPreviewParticles).
    // m_spawnBudget refills at the top of Update(); m_overloadThisFrame
    // accumulates refusals from the end of one Update to the end of the
    // next (so inter-frame refusals — bridge/spawner-driven instance
    // construction — count too), is folded into the latched
    // m_overloadActive at the end of Update(), then reset there.
    // [guard-config] enabled/max are runtime config (SetOverloadGuard).
    bool m_overloadGuardEnabled = true;
    int  m_maxPreviewParticles  = kDefaultMaxPreviewParticles;
    int  m_maxPreviewInstances  = kDefaultMaxPreviewParticles / kInstancesDivisor;
    int  m_spawnBudget       = kDefaultMaxPreviewParticles;
    bool m_overloadActive    = false;
    bool m_overloadThisFrame = false;
    // Time of the most recent refused spawn — drives the
    // kOverloadClearDelaySec debounce on m_overloadActive.
    TimeF m_lastOverloadTime = -1.0f;

    // [hard-guard] Estimated alive particles for ONE placed instance,
    // pushed by the web (engine/set/estimated-load; chain-load.ts owns
    // the formula). 0 = no estimate yet → the gate is INERT (never refuse
    // on a number we don't have; the runtime budget above is the backstop).
    double m_estimatedPerInstance = 0.0;
    // One-shot spawn-refusal record for the dispatcher's 4 Hz poll.
    // (SpawnRefusal type is declared public beside TakeSpawnRefusal.)
    bool m_spawnRefusalPending = false;
    SpawnRefusal m_spawnRefusal{};

	// Viewing
	Camera		m_eye;
	D3DXMATRIX	m_view;
    D3DXMATRIX	m_viewInverse;
	D3DXMATRIX  m_viewRotation;
	D3DXMATRIX	m_billboard;
	D3DXMATRIX	m_projection;
	D3DXMATRIX	m_viewProjection;

	// Phase 3 Stage 5 — scene viewport cache (Variant B-γ).
	// Active flag false means "use full RT" (default — matches all
	// non-composition transports). When active, Engine::Render
	// SetViewports the device to (X, Y, W, H) before the scene
	// pass (after the full-RT Clear per the D12 ordering rule);
	// m_projection is computed at W/H aspect by SetSceneViewport.
	// Survives Reset (re-applied at end of Reset to overwrite the
	// full-RT-aspect projection rebuild at engine.cpp:1448).
	int  m_sceneViewportX      = 0;
	int  m_sceneViewportY      = 0;
	int  m_sceneViewportW      = 0;
	int  m_sceneViewportH      = 0;
	bool m_sceneViewportActive = false;
	float m_sceneFovY          = D3DXToRadian(45.0f);

	// m_sceneFovY: the vertical FoV (radians, POST 120-degree clamp) that SetSceneViewport
	// feeds to the projection; read by ReferenceGizmoHandleLength for screen-uniform gizmo
	// sizing while m_sceneViewportActive. (Supersedes the old per-pixel-FoV reference-height
	// fields, which were removed; the projection uses the anchored 768px height, not BackBufferHeight.)

    COLORREF    m_background;
	bool		m_showGround;
	float		m_groundZ;
	int			m_groundTextureIndex;   //: 0..kGroundTextureCount-1
	// Per-slot user-supplied texture file path. Empty string means
	// "use bundled default" (for slots 0..kGroundTextureBundledCount-1
	// except slot kGroundSolidColorSlot which has no file source) or
	// "slot is empty" (for higher slots). Persisted by main.cpp via
	// HKCU\Software\AloParticleEditor\GroundTextureSlot{0..11}.
	std::wstring m_groundSlotCustomPaths[kGroundTextureCount];
	COLORREF     m_groundSolidColor;   // slot kGroundSolidColorSlot
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
    //: scene-global shadow tint. Stored only — no shader handle
    // currently consumes it. Exposed in the Lighting dialog for parity
    // with the Petroglyph map editor's panel and forward-compatibility
    // with shader changes.
    D3DXVECTOR4 m_shadow;
    Light       m_lights[3];
    D3DXMATRIX  m_sphLightFill[3];
    D3DXMATRIX  m_sphLightAll[3];

	// / Phase 3 Stage 1: Skydome UV sphere geometry.
	// Originally D3DPOOL_MANAGED so it survived device Reset, but
	// D3D9Ex disallows the managed pool — promoted to D3DPOOL_DEFAULT.
	// Engine::Reset now releases the VB/IB before m_pDevice->Reset and
	// recreates them via CreateSkydomeMeshBuffers() after the device
	// successfully resets (mirrors the existing OnLostDevice / OnResetDevice
	// flow used for shaders + bloom + compositor RT). The vertex
	// declaration m_pSkydomeDecl is not pool-bound and stays valid
	// across Reset.
	struct SkydomeVertex
	{
	    D3DXVECTOR3 Position;
	    D3DXVECTOR3 Normal;
	    D3DXVECTOR2 TexCoord; // (U, V) for equirectangular sampling
	};

	static const int kSkydomeLongSegments    = 32;
	static const int kSkydomeLatSegments     = 16;

	IDirect3DVertexBuffer9*      m_pSkydomeVB;
	IDirect3DIndexBuffer9*       m_pSkydomeIB;
	IDirect3DVertexDeclaration9* m_pSkydomeDecl;
	DWORD                        m_skydomeIndexCount;

	//: skydome effect and texture state
	ID3DXEffect*             m_pSkydomeEffect;
	D3DXHANDLE               m_hSkydomeWVP;
	D3DXHANDLE               m_hSkydomeTex;
	IDirect3DTexture9*       m_pSkydomeTexture;
	int                      m_skydomeIndex;
	std::wstring             m_skydomeCustomSlotPaths[kSkydomeSlotCount - kSkydomeFirstCustomSlot];

	// Game-faithful dome meshes (additive to the simple-background state
	// above; m_skydomeIndex stays the frozen legacy/primary scalar). When a real
	// dome is selected by Name these hold the decoded .alo + per-sub-mesh shaders;
	// otherwise empty and the simple-background sphere renders instead.
	SkydomeContext           m_skydomeContext = SkydomeContext::Space;
	std::string              m_skydomePrimaryName;     // "" = none
	std::string              m_skydomeSecondaryName;   // "" = none
	SkydomeMesh              m_skydomePrimaryMesh;
	SkydomeMesh              m_skydomeSecondaryMesh;
	// [seam fix] "Smooth skydome seams" preference (default on). Gates the UV
	// re-map in SkydomeMesh::Load; toggled live via SetSkydomeSeamFix (bridge).
	bool                     m_skydomeSeamFix = true;
	// Per-slot load outcome (None = no Name chosen, Ok = .alo loaded,
	// LoadFailed = Name chosen but the .alo wouldn't load). Set in
	// RebuildSkydomeMeshes alongside the mesh state it gates the render on.
	SkydomeSlotStatus        m_skydomePrimaryStatus   = SkydomeSlotStatus::None;
	SkydomeSlotStatus        m_skydomeSecondaryStatus = SkydomeSlotStatus::None;
	// [#NN] Cached per-axis skydome lists (LoadAllSkydomeLists), rebuilt only when the
	// mod/submod context changes. Without this, #224's GameObjectFiles locator re-parsed
	// GameObjectFiles.xml + re-sniffed every referenced file ~4x per mod switch (the two
	// RebuildSkydomeMeshes axes + the two picker-query axes), on the UI thread -> mod-switch
	// lag. Built lazily + context-checked by EnsureSkydomeLists(); both EnumerateSkydomeNames
	// and RebuildSkydomeMeshes read it.
	std::array<std::vector<SkydomeRef>, kNumSkydomeAxes> m_skydomeLists;
	bool                      m_skydomeListsValid = false;
	std::vector<std::wstring> m_skydomeListsCtxRoots;     // content roots the cache reflects

	// Imported reference object (a game-object .alo placed in the preview
	// for scale). Rigid multi-part: each sub-mesh placed by its skeleton bone.
	// is the render path only; the picker/transform/persistence are.
	ReferenceObjectMesh      m_referenceObjectMesh;
	// Hardpoint attach models mounted on the selected object (turrets / weapons
	// the unit references via its <HardPoints> list). Rebuilt in RebuildReferenceObjectMesh;
	// drawn after the unit in RenderReferenceObject; looped at every device-reset site.
	std::vector<std::unique_ptr<ReferenceAttachment>> m_referenceAttachments;

	// selection + placement state driving m_referenceObjectMesh.
	std::string              m_referenceObjectName;            // "" = none selected
	bool                     m_referenceObjectVisible = true;
	bool                     m_modelShadowsEnabled = true;     // [shadow] "Model shadows" pref (default on)
	bool                     m_softShadowsEnabled  = true;     // [soft-shadows] "Soft shadows" pref (default on)
	D3DXVECTOR3              m_referencePosition = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	D3DXVECTOR3              m_referenceRotation = D3DXVECTOR3(0.0f, 0.0f, 0.0f);  // degrees [yaw,pitch,roll]
	// Per-object <Scale_Factor> render multiplier for the selected reference
	// object (1.0 = native; trooper 1.5, AT-AT 1.8). Applied as a leftmost uniform
	// scale in ReferenceObjectWorldFrom so render/pick/selection-box agree. Reset to
	// 1.0 at the TOP of RebuildReferenceObjectMesh (all exit paths) and set from the
	// catalog only on a successful resolve, so a mod/submod switch never leaves it stale.
	float                    m_referenceScaleFactor = 1.0f;
	// Render-only "display" transform eased toward the committed transform above
	// each frame so gizmo/object motion is smooth (incl. under snap). Committed values
	// stay exact (undo / spinners / saved file). See Engine::EaseReferenceDisplay.
	D3DXVECTOR3              m_displayPosition = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	D3DXVECTOR3              m_displayRotation = D3DXVECTOR3(0.0f, 0.0f, 0.0f);
	long long               m_displayLastQpc = 0;        // QPC of last ease (0 = uninit -> snap); QPC not GetTickCount so the ease stays smooth on high-refresh displays
	ReferenceObjectStatus    m_referenceObjectStatus = ReferenceObjectStatus::None;
	bool                     m_referenceObjectSelected = false;  // gizmo visible/grabbable only when selected; assign ONLY via RefLockResolveSelected so the lock is honoured (see RefLock.h)
	bool                     m_referenceLocked = false;  // freeze: non-selectable/grabbable; transform frozen at the bridge (UI path), not the engine setter (undo restore must write)
	ManipHandle              m_hoverManip;                       // handle under the cursor (highlight), kind=NONE = none
	ManipHandle              m_activeManip;            // the handle currently being DRAGGED (NONE = idle); drives guide/sweep/dim
	float                    m_activeGrabAngle   = 0.0f;  // rotate: ring angle (rad) at grab
	float                    m_activeAppliedAngle = 0.0f; // rotate: grab + applied accumulated angle (rad)
	GameObjectCatalog        m_referenceCatalog;               // lazily built; invalidated on mod switch
	bool                     m_referenceCatalogBuilt = false;
	// Off-UI-thread catalog build. BuildGameObjectCatalog parses every object
	// XML (O(content)) -- on a big mod that froze the whole window when run on the
	// WebView2 UI thread (BridgeDispatcher::Dispatch). It now runs on a worker thread
	// with an ISOLATED FileManager (its own MEG handles -> no seek-race against the UI
	// thread's FileManager, files.cpp:89); the finished catalog is swapped in on the UI
	// thread in Update(). A generation counter (bumped on each invalidation) discards a
	// build whose mod/submod context changed mid-flight.
	bool                     m_catalogWanted = false;           // someone needs the catalog (keep it warm across mod switches)
	std::atomic<bool>        m_catalogBuilding{ false };         // a worker build is in flight
	std::mutex               m_catalogMutex;                     // guards m_pendingCatalog + m_pendingCatalogGen
	std::unique_ptr<GameObjectCatalog> m_pendingCatalog;        // worker -> UI handoff slot
	uint64_t                 m_catalogGeneration = 0;            // current valid generation (bumped on invalidation)
	uint64_t                 m_pendingCatalogGen = 0;            // generation the pending build was started at
	std::thread              m_catalogThread;                    // the worker (joined on harvest / in ~Engine)
	bool                     m_catalogJustReady = false;         // set on swap; ConsumeCatalogReadyFlag() -> host emits state-changed
	bool                     m_referenceMeshDeferred = false;    // a ref-object rebuild waiting on the catalog
	// The mod/submod context the current/in-flight catalog reflects (captured
	// at build launch). ReloadTextures invalidates the catalog ONLY when the active
	// roots differ from this -- so a texture-only reload (F5 / file open), which keeps
	// the mod context, neither rebuilds an identical catalog nor drops the ref object.
	std::vector<std::wstring> m_catalogContextRoots;      // content roots the catalog reflects

	// unit-grid state (RenderUnitGrid is).
	bool                     m_gridVisible = false;
	float                    m_gridSpacing = 20.0f;
	bool                     m_snapEnabled = false;            // gizmo grid/angle snap

	//: bump-mapped ground lighting. Effect + tangent-space vertex decl +
	// normal-map state, mirroring the skydome effect lifecycle. Faithful port
	// of TerrainMeshBump.fx (reference/foc-shaders/) minus cloud/FOW.
	struct GroundVertex
	{
	    D3DXVECTOR3 Position;
	    D3DXVECTOR3 Normal;
	    D3DXVECTOR2 TexCoord;
	    D3DXVECTOR3 Tangent;
	    D3DXVECTOR3 Binormal;
	};
	ID3DXEffect*                 m_pGroundEffect;
	IDirect3DVertexDeclaration9* m_pGroundDecl;
	IDirect3DTexture9*           m_pGroundNormalTexture;     // companion _bc map, or flat fallback
	IDirect3DTexture9*           m_pGroundFlatNormalTexture; // 1px (128,128,255) neutral normal
	D3DXHANDLE m_hGroundWVP, m_hGroundWorld, m_hGroundSphFill,
	           m_hGroundLightObjVec, m_hGroundLightDiffuse, m_hGroundLightSpecular,
	           m_hGroundEyeObjPos, m_hGroundBaseTex, m_hGroundNormalTex;

	// Resources
	IDirect3DTexture9*	m_pGroundTexture;
	IDirect3DTexture9*	m_pSceneTexture;
    IDirect3DSurface9*  m_pDepthStencilSurface;
	IDirect3DTexture9*	m_pDistortTexture;
	// Offscreen MSAA surfaces. When m_msaaActive the scene draws into these;
	// StretchRect resolves m_pMsaaColor into m_pSceneTexture (non-MS) before the
	// bloom/distort/compose post-process chain reads it. D3DPOOL_DEFAULT — released
	// and recreated on every device reset / resize alongside the other scene RTs.
	IDirect3DSurface9*  m_pMsaaColor  = NULL;  // multisampled scene RT
	IDirect3DSurface9*  m_pMsaaDepth  = NULL;  // matching multisampled depth-stencil
	bool                m_msaaActive  = false; // false => non-MSAA path (behavior-identical to pre-MSAA)
	// [runtime-MSAA] Preferred level set by SetMsaaLevel (0=off, 2/4/8 samples).
	// m_msaaDirty triggers ApplyMsaaLevelNow() on the render thread; m_currentMsaaLevel
	// reflects what was actually allocated (may be < preferred due to driver caps).
	int                 m_msaaPreferredLevel = 4;  // user pref: 0=off, else target samples (2/4/8)
	int                 m_currentMsaaLevel   = 0;  // actual level in use (0 = off)
	bool                m_msaaDirty          = false; // set by SetMsaaLevel; applied on the render thread
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

	// [soft-shadows] Screen-space shadow mask + the FoC blur-composite effect.
	// Mirrors the bloom RT plumbing exactly: m_pShadowMask is a full-backbuffer
	// A8R8G8B8 D3DPOOL_DEFAULT render-target texture, created/released alongside
	// the bloom ping/pong targets and on every device reset/resize. When MSAA is
	// active the stencil-driven mask is rendered into m_pShadowMaskMsaa (a
	// matching-MSAA render-target surface, the same trick as m_pMsaaColor) then
	// StretchRect-resolved into m_pShadowMask so the blur pass can sample it.
	// m_pShadowBlurEffect = getShader("Engine\\StencilDarkenFinalBlur.fx"); NULL
	// (or any cached handle missing) => soft shadows unavailable => hard fallback.
	IDirect3DTexture9*  m_pShadowMask     = nullptr;
	IDirect3DSurface9*  m_pShadowMaskMsaa = nullptr;
	Effect*             m_pShadowBlurEffect = nullptr;
	D3DXHANDLE          m_hShadowBlurAmt   = nullptr;  // float blurAmt
	D3DXHANDLE          m_hShadowBlurWvp   = nullptr;  // m_worldViewProj (WORLDVIEWPROJECTION)
	D3DXHANDLE          m_hShadowBlurTech  = nullptr;  // first technique that validates
	bool                m_shadowBlurReady  = false;    // effect loaded + handles bound

	ITextureManager&				m_textureManager;
	IShaderManager&					m_shaderManager;
	// follow-up: needed to resolve curated skydome textures from the
	// base game / active mod via the MEG-archive + loose-file chain.
	IFileManager&					m_fileManager;
	// Phase 3 Stage 1: promoted from IDirect3D9/IDirect3DDevice9 to
	// the *Ex types so the engine's render target can be opened as a
	// shared-handle resource by a D3D11 device (Stage 2). IDirect3DDevice9Ex
	// inherits from IDirect3DDevice9, so existing call sites that use the
	// base interface (TextureManager, ShaderManager, Effect helpers) keep
	// working through implicit covariance. D3DPOOL_MANAGED is no longer
	// available on this device — the four pre-existing managed-pool sites
	// (engine.cpp 1044/1511/1522/1608) have been migrated to
	// D3DPOOL_DEFAULT and added to the OnLostDevice/OnResetDevice flow.
	IDirect3D9Ex*					m_pDirect3D;
	D3DPRESENT_PARAMETERS			m_presentationParameters;
	IDirect3DDevice9Ex*				m_pDevice;
	IDirect3DVertexDeclaration9*	m_pDeclaration;

	// Phase 3 Stage 4a — D3D9Ex event query for cross-device
	// GPU sync. Lazy-created on first IssueEndFrameQuery; released in
	// Engine::Reset before m_pDevice->Reset; lazy-recreated on next
	// Issue. See IssueEndFrameQuery / WaitEndFrameQuery declarations
	// in the public section for usage.
	IDirect3DQuery9*				m_pEndFrameQuery = NULL;

	// non-owning. When non-null, Render targets its off-screen
	// shared-RT and skips the engine-side Present() (the host's DComp
	// path presents). Lifetime managed by HostWindowImpl; detached via
	// SetAlphaCompositor(nullptr) on WM_DESTROY before the compositor
	// is destroyed.
	host::AlphaCompositor*			m_pAlphaCompositor = nullptr;

	static D3DVERTEXELEMENT9 ParticleElements[];
    
	// Shader
	D3DXHANDLE	 p_worldViewProjection;
};

#endif
