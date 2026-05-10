#include "SpawnerDriver.h"
#include "ParticleSystemInstance.h"
#include <algorithm>
#include <cstdlib>

namespace {

// Anchor-Object3D with public position/velocity setters. Used as the
// transient parent of each spawned instance so the spawn inherits a
// stamped position + velocity. The instance is detached immediately
// after spawn — its self-motion (driven inside ParticleSystemInstance::
// Update) takes over from there.
class SpawnerAnchor : public Object3D
{
public:
    SpawnerAnchor() : Object3D(NULL) {}
    void SetPosition(const D3DXVECTOR3& p) { m_position = p; }
    void SetVelocity(const D3DXVECTOR3& v) { m_velocity = v; }
};

inline float JitterAxis(float r)
{
    if (r <= 0.0f) return 0.0f;
    float u = (float)std::rand() / (float)RAND_MAX;
    return (u * 2.0f - 1.0f) * r;
}

inline D3DXVECTOR3 Jitter(const D3DXVECTOR3& r)
{
    return D3DXVECTOR3(JitterAxis(r.x), JitterAxis(r.y), JitterAxis(r.z));
}

inline float Clamp(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline int ClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

inline D3DXVECTOR3 ClampVec(const D3DXVECTOR3& v, float bound)
{
    return D3DXVECTOR3(Clamp(v.x, -bound, bound),
                       Clamp(v.y, -bound, bound),
                       Clamp(v.z, -bound, bound));
}

} // namespace

void ClampSpawnerConfig(SpawnerConfig& cfg)
{
    cfg.burstSize      = ClampInt(cfg.burstSize, 1, SpawnerDriver::MAX_BURST_SIZE);
    cfg.spacingSec     = Clamp(cfg.spacingSec,     0.0f, SpawnerDriver::MAX_SPACING_SEC);
    cfg.intervalSec    = Clamp(cfg.intervalSec,    0.0f, SpawnerDriver::MAX_INTERVAL_SEC);
    cfg.maxLifetimeSec = Clamp(cfg.maxLifetimeSec, 0.0f, SpawnerDriver::MAX_LIFETIME_SEC);
    cfg.position       = ClampVec(cfg.position, SpawnerDriver::JITTER_MAX);
    cfg.velocity       = ClampVec(cfg.velocity, SpawnerDriver::JITTER_MAX);
    cfg.jitterPosition = ClampVec(cfg.jitterPosition, SpawnerDriver::JITTER_MAX);
    cfg.jitterVelocity = ClampVec(cfg.jitterVelocity, SpawnerDriver::JITTER_MAX);

    if ((int)cfg.mode < 0 || (int)cfg.mode > 1)
    {
        cfg.mode = SpawnerConfig::Mode::Auto;
    }
}

SpawnerDriver::SpawnerDriver()
    : m_phase(Phase::Waiting),
      m_burstRemaining(0),
      m_timeUntilNextInstance(0.0f),
      m_timeUntilNextBurst(0.0f)
{
}

void SpawnerDriver::SetConfig(const SpawnerConfig& cfg)
{
    SpawnerConfig oldCfg = m_cfg;
    m_cfg = cfg;
    ClampSpawnerConfig(m_cfg);

    // Reset the burst-state machine ONLY on transitions that change the
    // schedule's identity — mode flips and enable toggles. Parameter
    // tweaks (burst size, spacing, interval, position, velocity, jitter,
    // lifetime) leave the state alone, so:
    //
    //   - typing into a spinner mid-burst doesn't abort the burst.
    //   - typing into a spinner doesn't reset the interval timer (which
    //     would cause spurious immediate bursts on every keystroke).
    //
    // An in-flight burst keeps firing with whatever burstSize was
    // captured at StartBurst (so changing burstSize mid-burst affects
    // the *next* burst, not the current one). Spacing is read fresh on
    // each emission, so changing it mid-burst takes effect immediately.
    bool modeChanged    = (oldCfg.mode    != m_cfg.mode);
    bool enabledChanged = (oldCfg.enabled != m_cfg.enabled);

    if (modeChanged)
    {
        m_phase                 = Phase::Waiting;
        m_burstRemaining        = 0;
        m_timeUntilNextInstance = 0.0f;
        m_timeUntilNextBurst    = m_cfg.intervalSec;   // wait one interval before first burst
    }
    else if (enabledChanged && m_cfg.enabled && m_phase == Phase::Waiting)
    {
        // Just toggled Enabled true in Auto mode while idle: schedule
        // the first burst one interval out so the user has time to
        // verify their config without an immediate surprise burst.
        m_timeUntilNextBurst = m_cfg.intervalSec;
    }
}

float SpawnerDriver::ComputeBurstsPerSec(const SpawnerConfig& cfg)
{
    if (cfg.mode != SpawnerConfig::Mode::Auto) return 0.0f;
    int n = (cfg.burstSize > 1) ? cfg.burstSize - 1 : 0;
    float cycle = n * cfg.spacingSec + cfg.intervalSec;
    if (cycle < 1.0e-4f) cycle = 1.0e-4f;
    return 1.0f / cycle;
}

void SpawnerDriver::StartBurst()
{
    m_phase                 = Phase::BurstFiring;
    m_burstRemaining        = m_cfg.burstSize;
    m_timeUntilNextInstance = 0.0f;     // first instance fires this frame
}

void SpawnerDriver::Trigger(const ParticleSystem* sys, Engine* engine)
{
    if (m_cfg.mode != SpawnerConfig::Mode::Manual) return;
    if (m_phase == Phase::BurstFiring) return;     // ignore re-trigger mid-burst
    if (sys == NULL || engine == NULL) return;
    StartBurst();
}

void SpawnerDriver::Tick(float dtSeconds, const ParticleSystem* sys, Engine* engine)
{
    if (sys == NULL || engine == NULL) return;
    if (dtSeconds <= 0.0f || dtSeconds > 1.0f) dtSeconds = 1.0f / 60.0f;

    // Auto mode: countdown to next burst when between bursts.
    if (m_phase == Phase::Waiting
        && m_cfg.mode == SpawnerConfig::Mode::Auto
        && m_cfg.enabled)
    {
        m_timeUntilNextBurst -= dtSeconds;
        if (m_timeUntilNextBurst <= 0.0f)
        {
            StartBurst();
            m_timeUntilNextBurst = 0.0f;
        }
    }

    // Burst firing: emit instances on schedule until the burst is done
    // or we hit a per-frame / live-instance cap.
    if (m_phase == Phase::BurstFiring)
    {
        m_timeUntilNextInstance -= dtSeconds;

        int spawnsThisFrame = 0;
        SpawnerAnchor anchor;
        while (m_burstRemaining > 0 && m_timeUntilNextInstance <= 0.0f)
        {
            if (spawnsThisFrame >= MAX_SPAWNS_PER_FRAME)
            {
                // Stutter / huge dt — drop surplus rather than queuing.
                m_timeUntilNextInstance = m_cfg.spacingSec;
                break;
            }
            if (engine->ActiveSpawnerInstanceCount() >= MAX_ACTIVE_INSTANCES)
            {
                // Cap hit — drop this and all remaining instances in
                // the burst. Resume schedule on next interval.
                m_burstRemaining = 0;
                break;
            }

            D3DXVECTOR3 pos = m_cfg.position + Jitter(m_cfg.jitterPosition);
            D3DXVECTOR3 vel = m_cfg.velocity + Jitter(m_cfg.jitterVelocity);
            anchor.SetPosition(pos);
            anchor.SetVelocity(vel);

            ParticleSystemInstance* inst = engine->SpawnParticleSystem(*sys, &anchor);
            if (inst != NULL)
            {
                inst->MarkSpawnerOwned();
                inst->SetMaxLifetime(m_cfg.maxLifetimeSec);
                inst->Detach();   // freeze the stamped values; per-frame motion takes over from Update
            }

            m_burstRemaining--;
            m_timeUntilNextInstance += m_cfg.spacingSec;
            spawnsThisFrame++;
        }

        if (m_burstRemaining == 0)
        {
            m_phase = Phase::Waiting;
            m_timeUntilNextBurst = m_cfg.intervalSec;
        }
    }
}
