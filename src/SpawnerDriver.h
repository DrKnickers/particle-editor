#ifndef SPAWNER_DRIVER_H
#define SPAWNER_DRIVER_H

#include "engine.h"

// Programmable particle spawner for the preview viewport. Replaces the
// "hold Shift, click once" single-instance flow with a configurable
// driver that emits ParticleSystemInstance objects either on demand
// (Manual) or on a recurring schedule (Auto).
//
// Each spawned instance starts at `position`, moves at constant
// `velocity`, and is capped by `maxLifetimeSec`. Per-instance motion
// is driven by ParticleSystemInstance::Update; this class just stamps
// the initial state and hands the instance off to the engine.
//
// All state is session/registry-only — never written into the .alo.

struct SpawnerConfig
{
    enum class Mode : int
    {
        Manual = 0,    // fires only when Trigger() is called
        Auto   = 1,    // fires on a recurring schedule when enabled
    };

    Mode        mode           = Mode::Auto;
    bool        enabled        = false;            // Auto only; pauses the schedule

    // Burst structure (both modes). One burst fires `burstSize` instances
    // spaced `spacingSec` seconds apart; in Auto mode bursts repeat with
    // `intervalSec` between the END of one and the START of the next.
    int         burstSize      = 1;                // 1..MAX_BURST_SIZE
    float       spacingSec     = 0.0f;             // 0..MAX_SPACING_SEC
    float       intervalSec    = 10.0f;            // Auto only; 0..MAX_INTERVAL_SEC

    D3DXVECTOR3 position       = D3DXVECTOR3(0, 0, 0);   // world-space spawn point
    D3DXVECTOR3 velocity       = D3DXVECTOR3(0, 0, 0);   // initial velocity, units/s

    // Hard cap on each spawned instance's lifetime. 0 means no cap —
    // instance lives until its particles die naturally per the .alo.
    float       maxLifetimeSec = 5.0f;

    D3DXVECTOR3 jitterPosition = D3DXVECTOR3(0, 0, 0);   // per-axis ±, world units

    // Path-shaping. Each spawned instance follows a shaped path
    // over its lifetime rather than a straight line:
    //
    //   - acceleration: deterministic constant accel (gravity-like) that
    //     bends the path into an arc. units/sec².
    //   - squiggleAmplitude / squiggleFrequency: a smooth per-axis
    //     sinusoidal lateral wander layered on top of the arc. Each
    //     instance gets its own random phase per axis at spawn, so
    //     siblings in a burst diverge organically. Amplitude is peak
    //     lateral displacement (world units); frequency is oscillations
    //     per second (Hz), shared across axes.
    //
    // All zero ⇒ a plain straight line (constant velocity), the
    // earlier behaviour minus the old velocity jitter.
    D3DXVECTOR3 acceleration      = D3DXVECTOR3(0, 0, 0);   // arc, units/sec²
    D3DXVECTOR3 squiggleAmplitude = D3DXVECTOR3(0, 0, 0);   // per-axis ±, world units
    float       squiggleFrequency = 1.0f;                  // Hz; 0..SQUIGGLE_FREQ_MAX
};

class SpawnerDriver
{
public:
    // Hard caps. See tasks/todo.md for rationale.
    static const int   MAX_ACTIVE_INSTANCES   = 50;
    static const int   MAX_SPAWNS_PER_FRAME   = 5;
    static const int   MAX_BURST_SIZE         = 10;
    // Cap on queued pre-begun manual triggers. Only bridge automation can
    // queue more than 1 (all queuing happens inside a single frame window);
    // 16 bursts already exceed MAX_ACTIVE_INSTANCES at any burstSize, so
    // nothing legitimate needs more — this just bounds hostile spam.
    static const int   MAX_PENDING_BURSTS     = 16;
    static constexpr float MAX_SPACING_SEC    = 10.0f;
    static constexpr float MAX_INTERVAL_SEC   = 60.0f;
    static constexpr float MAX_LIFETIME_SEC   = 600.0f;
    static constexpr float JITTER_MAX         = 10000.0f;
    static constexpr float SQUIGGLE_FREQ_MAX  = 20.0f;

    SpawnerDriver();

    // Replace the active config. Resets the burst-state machine so a
    // freshly-applied config doesn't strand half-finished bursts with
    // stale parameters.
    void SetConfig(const SpawnerConfig& cfg);
    const SpawnerConfig& GetConfig() const { return m_cfg; }

    // Drive emission. Called once per frame from main.cpp's Render
    // before engine->Update().
    void Tick(float dtSeconds, const ParticleSystem* sys, Engine* engine);

    // Manual fire. In Manual mode: kicks off one burst. If a burst is
    // armed but Tick hasn't begun emitting it yet, the trigger is QUEUED
    // (fires after the current burst) — never silently dropped. Once a
    // burst has begun emitting, re-triggers are debounced. In Auto mode:
    // no-op.
    void Trigger(const ParticleSystem* sys, Engine* engine);

    // Abort not-yet-begun manual work: clears the queued-trigger backlog
    // and, if the armed burst hasn't begun emitting, disarms it. A burst
    // that has already begun keeps firing (stop's long-standing in-flight
    // semantics). Called from the spawner/stop bridge handler and from
    // the host's refusal mirror in EmitStatsTick — the latter covers
    // engine-recorded refusals that happen OUTSIDE Tick (the edit-time
    // SetEstimatedLoad clear), which the Tick refusal branch can't see.
    void CancelPending();

    // Static variant for callers that have a config but no driver
    // instance (e.g. dialog code mid-edit). Same math as the instance
    // method; one source of truth.
    static float ComputeBurstsPerSec(const SpawnerConfig& cfg);

private:
    enum class Phase
    {
        Waiting,        // not currently firing instances
        BurstFiring,    // mid-burst
    };

    SpawnerConfig m_cfg;
    Phase         m_phase;

    int           m_burstRemaining;       // instances left in current burst
    float         m_timeUntilNextInstance;
    float         m_timeUntilNextBurst;   // Auto: countdown to next burst start

    // Manual triggers that arrived while a burst was armed but had not yet
    // BEGUN emitting (no Tick ran since StartBurst). Trigger() only arms;
    // emission happens on the next render-loop Tick — so back-to-back
    // bridge `spawner/trigger` requests can land inside one frame window,
    // and dropping them there silently coalesces distinct commands (the
    // preview-overload spec flake). Queued bursts fire one per Tick after
    // the current burst completes. Genuine mid-burst re-triggers (burst
    // has begun) are still debounced. A gate refusal clears the queue
    // (its bursts would refuse-and-clear again — banner churn). Bounded
    // in practice by triggers-per-frame; only bridge automation can
    // exceed 1, and the estimate gate/instance cap bound the effect.
    int           m_pendingBursts;
    bool          m_burstBegun;           // current burst has entered Tick emission

    void StartBurst();
};

// Validate / clamp a config in place against the driver's hard caps.
void ClampSpawnerConfig(SpawnerConfig& cfg);

#endif
