#include "SpawnerDriver.h"
#include "ParticleSystemInstance.h"
#include "ParticleRandom.h"
#include "SpawnerRandom.h"
#include "ProjectileShot.h"
#include <algorithm>
#include <cassert>

namespace
{

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
    return D3DXVECTOR3(Clamp(v.x, -bound, bound), Clamp(v.y, -bound, bound), Clamp(v.z, -bound, bound));
}

} // namespace

void ClampSpawnerConfig(SpawnerConfig& cfg)
{
    cfg.burstSize         = ClampInt(cfg.burstSize, 1, SpawnerDriver::MAX_BURST_SIZE);
    cfg.spacingSec        = Clamp(cfg.spacingSec, 0.0f, SpawnerDriver::MAX_SPACING_SEC);
    cfg.intervalSec       = Clamp(cfg.intervalSec, 0.0f, SpawnerDriver::MAX_INTERVAL_SEC);
    cfg.maxLifetimeSec    = Clamp(cfg.maxLifetimeSec, 0.0f, SpawnerDriver::MAX_LIFETIME_SEC);
    cfg.position          = ClampVec(cfg.position, SpawnerDriver::JITTER_MAX);
    cfg.velocity          = ClampVec(cfg.velocity, SpawnerDriver::JITTER_MAX);
    cfg.targetPosition    = ClampVec(cfg.targetPosition, SpawnerDriver::JITTER_MAX);
    cfg.jitterPosition    = ClampVec(cfg.jitterPosition, SpawnerDriver::JITTER_MAX);
    cfg.acceleration      = ClampVec(cfg.acceleration, SpawnerDriver::JITTER_MAX);
    cfg.squiggleAmplitude = ClampVec(cfg.squiggleAmplitude, SpawnerDriver::JITTER_MAX);
    cfg.squiggleFrequency = Clamp(cfg.squiggleFrequency, 0.0f, SpawnerDriver::SQUIGGLE_FREQ_MAX);

    if ((int)cfg.mode < 0 || (int)cfg.mode > 1)
    {
        cfg.mode = SpawnerConfig::Mode::Auto;
    }
}

ProjectileFlightParams ReferenceFlightParams(const Engine::ReferenceProjectileInfo& proj, float groundZ)
{
    ProjectileFlightParams p;
    p.speedPerFrame          = proj.speedPerFrame;
    p.range                  = proj.range;
    p.turnRateDegPerFrame    = proj.turnRateDegPerFrame;
    p.bombFallAccel          = proj.bombFallAccel;
    p.grenadeGravity         = proj.grenadeGravity;
    p.grenadeLobMod          = proj.grenadeLobMod;
    p.grenadeCanLobSlower    = proj.grenadeCanLobSlower;
    p.maxLifetimeSec         = proj.maxLifetimeSec;
    p.groundZ                = groundZ;
    p.clusterBomb            = IsClusterBombCategory(proj.category);
    p.rocketStraightDistance = proj.rocketStraightDistance;
    p.rocketCurveDistance    = proj.rocketCurveDistance;
    p.rocketCurveOffset      = proj.rocketCurveOffset;
    p.rocketExplodesAtTarget = proj.rocketExplodesAtTarget;
    p.rocketTargetOnTerrain  = proj.rocketTargetOnTerrain;
    p.inSpace                = proj.inSpace;
    // The preview's random stream stands in for the game's synchronized stream,
    // so each shot bends differently without matching any one game shot.
    p.randomInRange = [](float lo, float hi)
    {
        return ParticleRandom::Range(lo, hi);
    };
    return p;
}

bool IsBombingRunInEffect(const SpawnerConfig& cfg, const Engine& engine)
{
    Engine::ReferenceProjectileInfo proj;
    if (!cfg.bombingRun || !cfg.matchProjectile || !engine.GetReferenceProjectile(proj)) return false;
    return proj.speedPerFrame > 0.0f && ProjectileFlightKindFor(proj.category) == ProjectileFlightKind::Bomb;
}

bool IsProjectileTargetOutOfReach(const SpawnerConfig& cfg, const Engine* engine)
{
    Engine::ReferenceProjectileInfo proj;
    if (!cfg.matchProjectile || engine == NULL || !engine->GetReferenceProjectile(proj)) return false;
    if (ProjectileFlightKindFor(proj.category) != ProjectileFlightKind::Grenade) return false;
    const ProjectileFlightParams p         = ReferenceFlightParams(proj, engine->GetGroundZ());
    const float                  launch[3] = {cfg.position.x, cfg.position.y, cfg.position.z};
    const float                  target[3] = {cfg.targetPosition.x, cfg.targetPosition.y, cfg.targetPosition.z};
    float                        angleRad = 0.0f, speedPerFrame = 0.0f;
    return p.speedPerFrame > 0.0f && !GameGrenadeLob(p, launch, target, angleRad, speedPerFrame);
}

namespace
{

// How one spawned instance moves: the authored motion, or the projectile's flight.
struct ShotMotion
{
    bool                      fired            = true;  // false: the game wouldn't fire this shot
    bool                      isProjectileShot = false; // the projectile's flight replaced the authored motion
    D3DXVECTOR3               velocity;
    float                     maxLifetimeSec = 0.0f;
    D3DXVECTOR3               accel;
    D3DXVECTOR3               squiggle;
    std::vector<FlightSample> curvedPath; // empty for authored motion and straight shots
};

ShotMotion ComputeShotMotion(const SpawnerConfig& cfg, const D3DXVECTOR3& launchPos, const Engine& engine)
{
    ShotMotion motion;
    motion.velocity       = cfg.velocity;
    motion.maxLifetimeSec = cfg.maxLifetimeSec;
    motion.accel          = cfg.acceleration;
    motion.squiggle       = cfg.squiggleAmplitude;

    Engine::ReferenceProjectileInfo proj;
    if (!cfg.matchProjectile || !engine.GetReferenceProjectile(proj)) return motion;

    const float                aim[3] = {cfg.velocity.x, cfg.velocity.y, cfg.velocity.z};
    const ProjectileFlightKind kind   = ProjectileFlightKindFor(proj.category);
    if (kind == ProjectileFlightKind::Direct)
    {
        ProjectileShot flight;
        if (!ComputeProjectileShot(proj.speedPerFrame, proj.range, aim, flight)) return motion;
        motion.velocity       = D3DXVECTOR3(flight.velocity[0], flight.velocity[1], flight.velocity[2]);
        motion.maxLifetimeSec = flight.flightSec;
    }
    else
    {
        const float                  launch[3] = {launchPos.x, launchPos.y, launchPos.z};
        const float                  target[3] = {cfg.targetPosition.x, cfg.targetPosition.y, cfg.targetPosition.z};
        const ProjectileFlightParams params    = ReferenceFlightParams(proj, engine.GetGroundZ());
        if (!ComputeProjectileFlight(kind, params, launch, aim, target, motion.curvedPath))
        {
            // A grenade out of lob range: the game doesn't fire, so the preview doesn't either.
            motion.fired = false;
            return motion;
        }
        // The first step's velocity, so particles emitted before the path
        // takes over inherit the shot's motion, not the aim's.
        float startPos[3], startVel[3], startFacing[3];
        SampleFlightPath(motion.curvedPath, 0.0f, startPos, startVel, startFacing);
        motion.velocity       = D3DXVECTOR3(startVel[0], startVel[1], startVel[2]);
        motion.maxLifetimeSec = FlightPathSeconds(motion.curvedPath);
    }
    motion.isProjectileShot = true;
    motion.accel            = D3DXVECTOR3(0, 0, 0);
    motion.squiggle         = D3DXVECTOR3(0, 0, 0);
    return motion;
}

// Returns false when the preview's hard guard refused the spawn.
bool SpawnShotInstance(
    const ParticleSystem& sys, Engine& engine, SpawnerAnchor& anchor, float squiggleFrequency, ShotMotion motion)
{
    anchor.SetVelocity(motion.velocity); // no velocity jitter; path is shaped over lifetime instead
    ParticleSystemInstance* inst = engine.SpawnParticleSystem(sys, &anchor);
    if (inst == NULL) return false;

    inst->MarkSpawnerOwned();
    inst->SetMaxLifetime(motion.maxLifetimeSec);
    inst->SetPathShape(motion.accel, motion.squiggle, squiggleFrequency, SpawnerRandom::SquigglePhase());
    if (motion.isProjectileShot)
    {
        D3DXVECTOR3 dir;
        D3DXVec3Normalize(&dir, &motion.velocity);
        if (!motion.curvedPath.empty())
            dir = D3DXVECTOR3(motion.curvedPath[0].facing[0], motion.curvedPath[0].facing[1],
                              motion.curvedPath[0].facing[2]);
        inst->MarkProjectileShot(dir);
        if (!motion.curvedPath.empty()) inst->SetFlightPath(std::move(motion.curvedPath));
    }
    inst->Detach(); // freeze the stamped values; per-frame motion takes over from Update
    return true;
}

} // namespace

SpawnerDriver::SpawnerDriver()
    : m_phase(Phase::Waiting), m_burstRemaining(0), m_timeUntilNextInstance(0.0f), m_timeUntilNextBurst(0.0f),
      m_pendingBursts(0), m_burstBegun(false)
{
}

void SpawnerDriver::Reset()
{
    m_runActive             = false;
    m_phase                 = Phase::Waiting;
    m_burstRemaining        = 0;
    m_timeUntilNextInstance = 0.0f;
    m_timeUntilNextBurst    = 0.0f;
    m_pendingBursts         = 0;
    m_burstBegun            = false;
}

bool SpawnerDriver::IsIdle() const
{
    return m_phase == Phase::Waiting && m_burstRemaining == 0 && m_pendingBursts == 0 &&
           (m_cfg.mode == SpawnerConfig::Mode::Manual || !m_cfg.enabled);
}

void SpawnerDriver::SetConfig(const SpawnerConfig& cfg)
{
    SpawnerConfig oldCfg = m_cfg;
    m_cfg                = cfg;
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
    bool modeChanged    = (oldCfg.mode != m_cfg.mode);
    bool enabledChanged = (oldCfg.enabled != m_cfg.enabled);

    if (modeChanged)
    {
        m_phase                 = Phase::Waiting;
        m_burstRemaining        = 0;
        m_timeUntilNextInstance = 0.0f;
        m_timeUntilNextBurst    = m_cfg.intervalSec; // wait one interval before first burst
        m_pendingBursts         = 0;                 // queued manual triggers die with the mode
        m_burstBegun            = false;
        m_runActive             = false;
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
    int   n     = (cfg.burstSize > 1) ? cfg.burstSize - 1 : 0;
    float cycle = n * cfg.spacingSec + cfg.intervalSec;
    if (cycle < 1.0e-4f) cycle = 1.0e-4f;
    return 1.0f / cycle;
}

void SpawnerDriver::StartBurst()
{
    m_phase                 = Phase::BurstFiring;
    m_burstRemaining        = m_cfg.burstSize;
    m_timeUntilNextInstance = 0.0f;  // first instance fires this frame
    m_burstBegun            = false; // Tick hasn't processed this burst yet
}

void SpawnerDriver::Trigger(const ParticleSystem* sys, Engine* engine)
{
    if (m_cfg.mode != SpawnerConfig::Mode::Manual) return;
    if (sys == NULL || engine == NULL) return;
    if (m_phase == Phase::BurstFiring)
    {
        // An armed burst that Tick hasn't begun emitting yet is NOT
        // "mid-burst": Trigger only arms, so two bridge triggers inside
        // one frame window would otherwise coalesce (distinct commands
        // silently dropped — the preview-overload spec flake). Queue it.
        // Once the burst has begun, re-triggers stay debounced (button
        // hammering mid-burst doesn't stack bursts).
        if (!m_burstBegun && m_pendingBursts < MAX_PENDING_BURSTS) ++m_pendingBursts;
        return;
    }
    StartBurst();
}

void SpawnerDriver::CancelPending()
{
    m_pendingBursts = 0;
    if (m_phase == Phase::BurstFiring && !m_burstBegun)
    {
        // Armed but Tick never processed it: safe to disarm outright.
        m_phase              = Phase::Waiting;
        m_burstRemaining     = 0;
        m_timeUntilNextBurst = m_cfg.intervalSec;
    }
}

void SpawnerDriver::Tick(float dtSeconds, const ParticleSystem* sys, Engine* engine)
{
    if (sys == NULL || engine == NULL) return;
    if (!m_runActive && m_runMarkersShown) HideBombingRunMarkers(*engine);
    // A frozen preview clock reports zero elapsed time on every repaint.
    // Treat that as a true pause; inventing a frame here consumes auto-spawn
    // intervals and begins armed bursts while the scene is stopped.
    if (dtSeconds == 0.0f) return;
    if (dtSeconds < 0.0f || dtSeconds > 1.0f) dtSeconds = 1.0f / 60.0f;

    // Auto mode: countdown to next burst when between bursts.
    if (m_phase == Phase::Waiting && m_cfg.mode == SpawnerConfig::Mode::Auto && m_cfg.enabled)
    {
        m_timeUntilNextBurst -= dtSeconds;
        if (m_timeUntilNextBurst <= 0.0f)
        {
            StartBurst();
            m_timeUntilNextBurst = 0.0f;
        }
    }

    // Burst firing: emit instances on schedule until the burst is done
    // or we hit a per-frame / live-instance cap. In bombing-run mode the
    // burst flies one run instead.
    if (m_phase == Phase::BurstFiring)
    {
        if (!m_burstBegun && IsBombingRunInEffect(m_cfg, *engine)) BeginBombingRun(*engine);
        m_burstBegun = true; // re-triggers from here on are mid-burst (debounced)
        if (m_runActive)
            TickBombingRun(dtSeconds, *sys, *engine);
        else
            EmitBurstInstances(dtSeconds, *sys, *engine);

        if (m_burstRemaining == 0 && !m_runActive)
        {
            m_phase              = Phase::Waiting;
            m_timeUntilNextBurst = m_cfg.intervalSec;
            // A manual trigger queued while this burst was armed-but-unbegun
            // fires next: re-arm now, emit on the next Tick. (Zero after a
            // gate refusal — the branch above cleared the queue.)
            if (m_pendingBursts > 0)
            {
                --m_pendingBursts;
                StartBurst();
            }
        }
    }
}

// Emits the current burst's due instances, up to the per-frame and live-instance caps.
void SpawnerDriver::EmitBurstInstances(float dtSeconds, const ParticleSystem& sys, Engine& engine)
{
    m_timeUntilNextInstance -= dtSeconds;

    int           spawnsThisFrame = 0;
    SpawnerAnchor anchor;
    while (m_burstRemaining > 0 && m_timeUntilNextInstance <= 0.0f)
    {
        if (spawnsThisFrame >= MAX_SPAWNS_PER_FRAME)
        {
            // Stutter / huge dt — drop surplus rather than queuing.
            m_timeUntilNextInstance = m_cfg.spacingSec;
            break;
        }
        if (engine.ActiveSpawnerInstanceCount() >= MAX_ACTIVE_INSTANCES)
        {
            // Cap hit — drop this and all remaining instances in
            // the burst. Resume schedule on next interval.
            m_burstRemaining = 0;
            break;
        }

        D3DXVECTOR3 pos = m_cfg.position + SpawnerRandom::Jitter(m_cfg.jitterPosition);
        anchor.SetPosition(pos);

        // A shot the game wouldn't fire spawns nothing (the pane says why);
        // the burst still moves on to its next instance.
        ShotMotion motion = ComputeShotMotion(m_cfg, pos, engine);
        if (motion.fired && !SpawnShotInstance(sys, engine, anchor, m_cfg.squiggleFrequency, std::move(motion)))
        {
            DisableAfterRefusal();
            break;
        }

        m_burstRemaining--;
        m_timeUntilNextInstance += m_cfg.spacingSec;
        spawnsThisFrame++;
    }
}

// [hard-guard] SpawnParticleSystem returns NULL ONLY on a gate refusal (the
// estimate × placed-count would blow the cap, so it cleared the preview).
// Disable the driver so an auto/interval spawner doesn't refuse-and-clear
// every cycle (banner churn) — the user re-arms deliberately after
// lightening the effect or raising the cap.
void SpawnerDriver::DisableAfterRefusal()
{
    m_cfg.enabled    = false;
    m_burstRemaining = 0;
    m_runActive      = false;
    m_phase          = Phase::Waiting;
    m_pendingBursts  = 0; // queued bursts would refuse-and-clear again
}

// Plans the run this burst flies, from the matched bomb's bomber.
void SpawnerDriver::BeginBombingRun(const Engine& engine)
{
    Engine::ReferenceProjectileInfo proj;
    if (!engine.GetReferenceProjectile(proj))
    {
        assert(!"BeginBombingRun runs only when IsBombingRunInEffect");
        return;
    }
    const float target[3] = {m_cfg.targetPosition.x, m_cfg.targetPosition.y, m_cfg.targetPosition.z};
    const float aim[3]    = {m_cfg.velocity.x, m_cfg.velocity.y, m_cfg.velocity.z};
    m_run                 = PlanBombingRun(proj.bomber, target, aim, engine.GetGroundZ());
    m_runActive           = true;
    m_runClockSec         = 0.0f;
    m_runNextDrop         = 0;
    m_burstRemaining      = 0; // burst size doesn't apply to a run
}

// Spawns the run's due drops, shows where the bombers are, and ends the run once they
// have flown past.
void SpawnerDriver::TickBombingRun(float dtSeconds, const ParticleSystem& sys, Engine& engine)
{
    m_runClockSec += dtSeconds;
    int           spawnsThisFrame = 0;
    SpawnerAnchor anchor;
    while (m_runNextDrop < m_run.drops.size() && m_run.drops[m_runNextDrop].timeSec <= m_runClockSec)
    {
        // Past the per-frame cap a drop waits for the next frame; it is timestamped, so
        // nothing is lost after a stutter, only released late.
        if (spawnsThisFrame >= MAX_SPAWNS_PER_FRAME) break;
        const BombingRunDrop& drop = m_run.drops[m_runNextDrop++];
        // Over the live-instance cap a drop is skipped, like a burst's surplus.
        if (engine.ActiveSpawnerInstanceCount() >= MAX_ACTIVE_INSTANCES) continue;

        // Each bomb leaves along the run heading and falls on its own bomber's marker.
        SpawnerConfig dropCfg  = m_cfg;
        dropCfg.velocity       = D3DXVECTOR3(m_run.heading[0], m_run.heading[1], m_run.heading[2]);
        const float* marker    = m_run.markers[drop.bomberIndex];
        dropCfg.targetPosition = D3DXVECTOR3(marker[0], marker[1], marker[2]);
        const D3DXVECTOR3 pos(drop.pos[0], drop.pos[1], drop.pos[2]);
        anchor.SetPosition(pos);
        ShotMotion motion = ComputeShotMotion(dropCfg, pos, engine);
        if (motion.fired && !SpawnShotInstance(sys, engine, anchor, m_cfg.squiggleFrequency, std::move(motion)))
        {
            DisableAfterRefusal();
            HideBombingRunMarkers(engine);
            return;
        }
        spawnsThisFrame++;
    }

    if (m_runClockSec >= m_run.durationSec && m_runNextDrop >= m_run.drops.size())
    {
        m_runActive = false;
        HideBombingRunMarkers(engine);
        return;
    }
    ShowBombingRunMarkers(engine);
}

// Hands the engine each bomber's position now and its target marker, for the viewport.
void SpawnerDriver::ShowBombingRunMarkers(Engine& engine)
{
    Engine::BombingRunMarkers markers;
    for (int bomber = 0; bomber < kBombingRunBomberCount; ++bomber)
    {
        float position[3];
        BomberPositionAt(m_run, bomber, m_runClockSec, position);
        markers.bomberPositions[bomber] = D3DXVECTOR3(position[0], position[1], position[2]);
        const float* marker             = m_run.markers[bomber];
        markers.targetMarkers[bomber]   = D3DXVECTOR3(marker[0], marker[1], marker[2]);
    }
    engine.SetBombingRunMarkers(markers);
    m_runMarkersShown = true;
}

void SpawnerDriver::HideBombingRunMarkers(Engine& engine)
{
    engine.ClearBombingRunMarkers();
    m_runMarkersShown = false;
}
