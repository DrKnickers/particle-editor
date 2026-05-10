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
    D3DXVECTOR3 jitterVelocity = D3DXVECTOR3(0, 0, 0);   // per-axis ±, units/sec
};

class SpawnerDriver
{
public:
    // Hard caps. See tasks/todo.md for rationale.
    static const int   MAX_ACTIVE_INSTANCES   = 50;
    static const int   MAX_SPAWNS_PER_FRAME   = 5;
    static const int   MAX_BURST_SIZE         = 10;
    static constexpr float MAX_SPACING_SEC    = 10.0f;
    static constexpr float MAX_INTERVAL_SEC   = 60.0f;
    static constexpr float MAX_LIFETIME_SEC   = 600.0f;
    static constexpr float JITTER_MAX         = 10000.0f;

    SpawnerDriver();

    // Replace the active config. Resets the burst-state machine so a
    // freshly-applied config doesn't strand half-finished bursts with
    // stale parameters.
    void SetConfig(const SpawnerConfig& cfg);
    const SpawnerConfig& GetConfig() const { return m_cfg; }

    // Drive emission. Called once per frame from main.cpp's Render
    // before engine->Update().
    void Tick(float dtSeconds, const ParticleSystem* sys, Engine* engine);

    // Manual fire. In Manual mode: kicks off one burst.
    // In Auto mode (or when a burst is already in flight): no-op.
    void Trigger(const ParticleSystem* sys, Engine* engine);

    // For the dialog's read-only "Bursts/sec" label in Auto mode.
    float DerivedBurstsPerSec() const { return ComputeBurstsPerSec(m_cfg); }

    // Static variant for callers that have a config but no driver
    // instance (e.g. dialog code mid-edit). Same math as the instance
    // method; one source of truth.
    static float ComputeBurstsPerSec(const SpawnerConfig& cfg);

    bool IsActive() const { return m_cfg.enabled || m_cfg.mode == SpawnerConfig::Mode::Manual; }

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

    void StartBurst();
};

// Validate / clamp a config in place against the driver's hard caps.
void ClampSpawnerConfig(SpawnerConfig& cfg);

#endif
