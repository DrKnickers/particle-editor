// Bridge request handlers for the timeline/* kinds, and the interactive
// timeline session they drive. BridgeDispatcher::
// DispatchInternal routes requests here via TryDispatchTimeline; the host's
// paced idle slot calls ServiceTimeline once per frame.
//
// While a session is open a ParticleReplayController is the only thing that
// advances the simulation. The host renders with simulate=false, and the
// kinds that would spawn, clear or move the clock behind the controller are
// refused (RefuseWhileTimeline). The frame on screen is always the replay of
// the session's inputs (TimelineInputs) to its frame: when an input changes,
// the next slot replays again from a reset. A camera move is an input only
// for weather documents; elsewhere a parked frame re-bakes its billboards.

#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "simulation/ParticleRandom.h"
#include "simulation/ReplayPlayClock.h"

#include <algorithm>
#include <cstring>
#include <random>

using nlohmann::json;

namespace host
{

namespace
{

// The timeline replays the in-memory document, saved or not; the scenario's
// path is only a label (the hooks spawn the live ParticleSystem).
const wchar_t kTimelineDocumentLabel[] = L"<in-memory document>";

// The review window: PreviewScenario's default, 10 s at 60 frames a second.
const int kTimelineMaxFrames = PreviewScenario{}.maxFrames;

// The playhead state event rides at most this often while playing.
const int kTimelinePlayEmitsPerSecond = 15;

std::int64_t TimelineTicks()
{
    LARGE_INTEGER ticks;
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
}

std::int64_t TimelineFrequency()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

// Weather emitters place and wrap their particles around the camera while
// simulating (EmitterInstance), so for them the camera is a replay input.
bool HasWeatherEmitter(const ParticleSystem* document)
{
    if (!document) return false;
    for (const auto* emitter : document->getEmitters())
        if (emitter && emitter->isWeatherParticle) return true;
    return false;
}

// Holds a flag true for a scope, however the scope exits.
struct ScopedFlag
{
    bool& flag;
    explicit ScopedFlag(bool& f) : flag(f) { flag = true; }
    ~ScopedFlag() { flag = false; }
};

void CopyCamera(const Engine::Camera& camera, float (&out)[9])
{
    const D3DXVECTOR3* vectors[] = {&camera.Position, &camera.Target, &camera.Up};
    for (int v = 0; v < 3; ++v)
    {
        out[v * 3 + 0] = vectors[v]->x;
        out[v * 3 + 1] = vectors[v]->y;
        out[v * 3 + 2] = vectors[v]->z;
    }
}

} // namespace

bool BridgeDispatcher::TimelineInputs::operator==(const TimelineInputs& other) const
{
    // Bitwise camera comparison: any change, however small, is a new input.
    return document == other.document && documentChanges == other.documentChanges &&
           documentEdits == other.documentEdits && seed == other.seed && guardEnabled == other.guardEnabled &&
           guardLimit == other.guardLimit && estimate == other.estimate &&
           std::memcmp(camera, other.camera, sizeof(camera)) == 0;
}

void BridgeDispatcher::SetTimelineSeed(std::uint32_t seed)
{
    m_timelineSeed   = seed;
    m_timelineSeeded = true;
}

PreviewScenario BridgeDispatcher::TimelineScenario() const
{
    PreviewScenario scenario;
    scenario.aloPath = kTimelineDocumentLabel;
    scenario.seed    = m_timelineSeed;
    // The guard and estimate the user last pushed, else what the engine runs
    // with now. The engine clamps the limit to the range the scenario allows.
    scenario.overloadGuardEnabled = m_overloadGuardCached
                                        ? m_overloadGuardEnabled
                                        : (m_engine ? m_engine->OverloadGuardEnabledForObservation() : true);
    const int limit = m_overloadGuardCached
                          ? m_overloadGuardMaxParticles
                          : (m_engine ? m_engine->OverloadGuardLimitForObservation() : scenario.overloadGuardLimit);
    scenario.overloadGuardLimit = std::clamp(limit, 1000, 1000000);
    scenario.estimatedParticlesPerInstance =
        m_estimatedLoadCached ? m_estimatedLoadPerInstance : (m_engine ? m_engine->EstimatedLoadForObservation() : 0.0);
    return scenario;
}

BridgeDispatcher::TimelineInputs BridgeDispatcher::CurrentTimelineInputs() const
{
    TimelineInputs inputs;
    inputs.document                = (m_pParticleSystem && *m_pParticleSystem) ? m_pParticleSystem->get() : nullptr;
    inputs.seed                    = m_timelineSeed;
    inputs.documentEdits           = m_documentEdits;
    const PreviewScenario scenario = TimelineScenario();
    inputs.guardEnabled            = scenario.overloadGuardEnabled;
    inputs.guardLimit              = scenario.overloadGuardLimit;
    inputs.estimate                = scenario.estimatedParticlesPerInstance;
    if (m_engine)
    {
        inputs.documentChanges = m_engine->ParticleSystemChangeApplyCountForTesting();
        // Only weather emitters simulate around the camera. Elsewhere a move
        // changes no simulation state, only the billboards Update bakes into
        // the vertices, which ServiceTimeline re-bakes without a replay.
        if (HasWeatherEmitter(inputs.document)) CopyCamera(m_engine->GetCamera(), inputs.camera);
    }
    return inputs;
}

bool BridgeDispatcher::TimelineCameraIsInput() const
{
    return HasWeatherEmitter((m_pParticleSystem && *m_pParticleSystem) ? m_pParticleSystem->get() : nullptr);
}

bool BridgeDispatcher::ReplayTimelineTo(int frame)
{
    TimelineSession& t = m_timeline;
    // A replay can pump messages (the proof's admitDevice does); an end that
    // arrives meanwhile waits for it (EndTimeline).
    const ScopedFlag     replaying(t.replaying);
    const TimelineInputs inputs        = CurrentTimelineInputs();
    const bool           inputsChanged = !(inputs == t.replayed);
    if (inputsChanged) ++t.revision;
    t.replayed = inputs;
    t.frame    = frame;

    if (!m_engine || !inputs.document || inputs.document->getEmitters().empty())
    {
        // Nothing to spawn (SpawnParticleSystem misbehaves on an emitter-less
        // system): show the empty scene.
        t.controller.reset();
        if (m_engine)
        {
            m_engine->Clear();
            m_engine->InvalidatePausedIdleSkip();
        }
        FreezePreviewClockAt(0.0f); // the free-run clock stays stopped
        t.empty   = true;
        t.stale   = false;
        t.refused = false;
        return true;
    }
    t.empty = false;

    bool ok;
    if (!inputsChanged && !t.stale && t.controller && frame > t.controller->Frame())
    {
        // Nothing changed: continue forward instead of replaying from frame 0.
        // The proof's E2 shows seeking and playing sequentially agree.
        ok = true;
        while (ok && t.controller->Frame() < frame) ok = t.controller->Advance(t.controller->Frame() + 1 == frame);
    }
    else
    {
        if (inputsChanged || t.stale || !t.controller)
        {
            ParticleReplayController::Hooks hooks = m_timelineHooks();
            // Report a guard refusal instead of leaving an unexplained empty frame.
            const auto clear     = hooks.clearInstances;
            hooks.clearInstances = [this, clear]()
            {
                clear();
                m_timeline.refused = false;
            };
            const auto tick   = hooks.tickSpawner;
            hooks.tickSpawner = [this, tick]()
            {
                const bool ticked = tick();
                if (m_engine && m_engine->TakeSpawnRefusal(nullptr)) m_timeline.refused = true;
                return ticked;
            };
            // The editor keeps a degraded composition (an occluded window, a
            // late frame query): the simulation is still exact, so only a
            // blocked device fails the replay. The proof's checks, wrapped
            // inside, still fail a strict render there.
            const auto render = hooks.render;
            hooks.render      = [this, render]()
            {
                return render() || (m_engine && !m_engine->DeviceCallsBlocked());
            };
            t.controller = std::make_unique<ParticleReplayController>(TimelineScenario(), std::move(hooks));
        }
        ok = t.controller->SeekTo(frame, false);
    }
    // A failed replay has no accepted frame. Keep the requested one, stop
    // playing, and retry from a reset in a later slot.
    t.stale = !ok;
    if (!ok) t.playing = false;
    if (m_engine) CopyCamera(m_engine->GetCamera(), t.bakedCamera);
    return ok;
}

nlohmann::json BridgeDispatcher::TimelineStateJson() const
{
    const TimelineSession& t      = m_timeline;
    const char*            status = !t.active                       ? "closed"
                                    : t.stale                       ? "stale"
                                    : t.empty                       ? "empty"
                                    : t.playing                     ? "playing"
                                    : t.frame >= kTimelineMaxFrames ? "ended"
                                                                    : "paused";
    return json{
        {"active", t.active},
        {"playing", t.playing},
        {"loop", t.loop},
        {"frame", t.frame},
        {"timeSeconds", static_cast<double>(t.frame) / kReplayFramesPerSecond},
        {"maxFrames", kTimelineMaxFrames},
        {"seed", m_timelineSeed},
        {"status", status},
        {"refused", t.refused},
        {"revision", t.revision},
    };
}

void BridgeDispatcher::EmitTimelineState()
{
    m_timeline.lastEmitTicks = TimelineTicks();
    if (!m_emit || !m_timelineTellsEditor) return;
    m_emit(json{{"type", "evt"}, {"kind", "timeline/state"}, {"payload", TimelineStateJson()}});
}

bool BridgeDispatcher::RefuseWhileTimeline(BridgeRequestContext& ctx)
{
    if (!m_timeline.active) return false;
    ctx.SendErr(ctx.kind + " is unavailable while the timeline is open");
    return true;
}

bool BridgeDispatcher::ServiceTimeline()
{
    TimelineSession& t = m_timeline;
    if (!t.active || t.replaying) return false;
    const std::int64_t now       = TimelineTicks();
    const std::int64_t frequency = TimelineFrequency();

    // An input changed (or the last replay failed): replay to the frame. A
    // failed replay retries at most four times a second and reports only
    // when its status changes.
    if (t.stale || !(CurrentTimelineInputs() == t.replayed))
    {
        const bool wasStale = t.stale;
        if (wasStale && now - t.retryTicks < frequency / 4) return false;
        t.retryTicks  = now;
        const bool ok = ReplayTimelineTo(t.frame);
        t.anchor      = ReplayPlayAnchor{t.frame, now};
        if (!(wasStale && t.stale)) EmitTimelineState();
        if (t.endPending)
        {
            EndTimeline();
            return false;
        }
        return ok && !t.empty; // the replay presented its destination
    }
    if (t.empty || !t.controller) return false;

    if (!t.playing)
    {
        // Parked with only the camera moved (a weather document replays
        // instead): re-bake the billboards with one Update at the frozen
        // time, as a paused free-run preview does. It spawns, kills and
        // draws nothing; the proof's E12 checks that against E1.
        float camera[9] = {};
        if (m_engine) CopyCamera(m_engine->GetCamera(), camera);
        if (m_engine && !m_engine->DeviceCallsBlocked() && std::memcmp(camera, t.bakedCamera, sizeof(camera)) != 0)
        {
            // Update returns early on a device it cannot admit: keep the
            // old camera then, so the next slot tries again.
            const std::uint64_t before = m_engine->CompletedFullUpdateCountForObservation();
            m_engine->InvalidatePausedIdleSkip();
            m_engine->Update();
            if (m_engine->CompletedFullUpdateCountForObservation() == before + 1)
                std::memcpy(t.bakedCamera, camera, sizeof(camera));
        }
        return false;
    }

    const ReplayPlayStep step      = NextReplayPlayStep(t.anchor, t.frame, now, frequency, kTimelineMaxFrames, t.loop);
    bool                 presented = false;
    if (step.wrap)
    {
        presented = ReplayTimelineTo(0);
    }
    else
    {
        // Update only; the host's render presents the frame once.
        bool ok = true;
        {
            const ScopedFlag replaying(t.replaying);
            while (ok && t.controller->Frame() < step.target) ok = t.controller->Advance(false);
        }
        if (m_engine) CopyCamera(m_engine->GetCamera(), t.bakedCamera);
        if (ok)
            t.frame = step.target;
        else
        {
            t.stale   = true;
            t.playing = false;
        }
    }
    if (step.reanchor) t.anchor = ReplayPlayAnchor{t.frame, now};
    if (step.ended) t.playing = false;
    if (!t.playing || step.wrap || now - t.lastEmitTicks >= frequency / kTimelinePlayEmitsPerSecond)
        EmitTimelineState();
    if (t.endPending)
    {
        EndTimeline();
        return false;
    }
    return presented;
}

void BridgeDispatcher::EndTimeline()
{
    TimelineSession& t = m_timeline;
    if (!t.active) return;
    if (t.replaying)
    {
        // A message pumped from inside a replay: end once it returns.
        t.endPending = true;
        return;
    }
    t.endPending = false;
    t.controller.reset();
    t.active  = false;
    t.playing = false;
    // The replay's instance goes; free-run resumes with the user's spawner,
    // guard, estimate and clock. Placed instances from before the session
    // are not restored (the UI says so when the timeline opens).
    if (m_engine)
    {
        m_engine->Clear();
        m_engine->InvalidatePausedIdleSkip();
        m_engine->TakeSpawnRefusal(nullptr);
        if (m_overloadGuardCached) m_engine->SetOverloadGuard(m_overloadGuardEnabled, m_overloadGuardMaxParticles);
        if (m_estimatedLoadCached) m_engine->SetEstimatedLoad(m_estimatedLoadPerInstance);
    }
    if (m_spawnerDriver)
    {
        m_spawnerDriver->Reset();
        m_spawnerDriver->SetConfig(m_spawnerConfig);
    }
    FreezePreviewClockAt(t.savedTime);
    SetPreviewPaused(t.savedPaused);
    EmitEngineStateChanged();
    EmitTimelineState();
}

bool BridgeDispatcher::TryDispatchTimeline(BridgeRequestContext& ctx)
{
    const json&        params = ctx.params;
    const std::string& kind   = ctx.kind;
    if (kind.rfind("timeline/", 0) != 0) return false;
    TimelineSession& t = m_timeline;
    if (t.replaying)
    {
        ctx.SendErr("the timeline is busy replaying");
        return true;
    }
    // An end pumped from inside this request's replay runs after the reply.
    struct EndAfterReply
    {
        BridgeDispatcher& dispatcher;
        ~EndAfterReply()
        {
            if (dispatcher.m_timeline.endPending) dispatcher.EndTimeline();
        }
    } endAfterReply{*this};

    // -------- timeline/begin -----------------------------------------
    // Open the session (idempotent): pause free-run, replay frame 0 of the
    // current document under the session's variation.
    if (kind == "timeline/begin")
    {
        if (!m_timelineAllowed || !m_timelineHooks)
        {
            ctx.SendErr("the timeline is not available in this run");
            return true;
        }
        if (!ctx.RequireEngine(kind.c_str())) return true;
        if (!t.active)
        {
            // The first session replays the launch variation.
            if (!m_timelineSeeded) SetTimelineSeed(ParticleRandom::SeedValue());
            t.savedPaused = IsPreviewPaused();
            t.savedTime   = GetTimeF();
            t.active      = true;
            t.playing     = false;
            t.stale       = false;
            t.replayed    = TimelineInputs{};
            t.controller.reset();
            ReplayTimelineTo(0);
            t.revision = 0;
            t.anchor   = ReplayPlayAnchor{0, TimelineTicks()};
            EmitEngineStateChanged(); // the free-run clock is now frozen
        }
        ctx.SendOk(TimelineStateJson());
        return true;
    }

    // -------- timeline/end -------------------------------------------
    if (kind == "timeline/end")
    {
        EndTimeline();
        ctx.SendOk(TimelineStateJson());
        return true;
    }

    if (!t.active)
    {
        ctx.SendErr("the timeline is not open");
        return true;
    }

    // -------- timeline/seek ------------------------------------------
    // Previous/next send frame -/+ 1; reset and jump-to-start send 0;
    // jump-to-end sends maxFrames. Clamped to the review window.
    if (kind == "timeline/seek")
    {
        const auto it = params.find("frame");
        if (it == params.end() || !it->is_number_integer())
        {
            ctx.SendErr("frame must be an integer");
            return true;
        }
        ReplayTimelineTo(ClampReplayFrame(it->get<long long>(), kTimelineMaxFrames));
        t.anchor = ReplayPlayAnchor{t.frame, TimelineTicks()};
        ctx.SendOk(TimelineStateJson());
        return true;
    }

    // -------- timeline/set -------------------------------------------
    // Play/pause and loop; an absent field is left unchanged.
    if (kind == "timeline/set")
    {
        for (const char* field : {"playing", "loop"})
        {
            const auto it = params.find(field);
            if (it != params.end() && !it->is_boolean())
            {
                ctx.SendErr(std::string(field) + " must be a boolean");
                return true;
            }
        }
        if (params.contains("loop")) t.loop = params["loop"].get<bool>();
        if (params.contains("playing"))
        {
            const bool playing = params["playing"].get<bool>();
            if (playing && !t.playing)
            {
                // Play at the end of the window starts over.
                if (t.frame >= kTimelineMaxFrames && !t.loop) ReplayTimelineTo(0);
                t.anchor = ReplayPlayAnchor{t.frame, TimelineTicks()};
            }
            t.playing = playing && !t.stale;
        }
        ctx.SendOk(TimelineStateJson());
        return true;
    }

    // -------- timeline/new-variation ---------------------------------
    // A new seed, replayed at the current frame. Drawn from the OS, never
    // from the particle stream.
    if (kind == "timeline/new-variation")
    {
        std::random_device entropy;
        std::uint32_t      seed = entropy();
        if (seed == m_timelineSeed) ++seed;
        SetTimelineSeed(seed);
        ReplayTimelineTo(t.frame);
        t.anchor = ReplayPlayAnchor{t.frame, TimelineTicks()};
        ctx.SendOk(TimelineStateJson());
        return true;
    }

    ctx.SendErr("not implemented yet");
    return true;
}

} // namespace host
