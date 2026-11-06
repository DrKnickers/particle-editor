// Live engine view bridge (#949 3b.3): starts and stops the engine host's frame stream, hands
// each new frame to the engine, and forwards the camera, edits and resizes to the host. The
// worker owns the pipe; EngineLiveReader owns the shared memory; EngineLiveSync picks what to
// send next; the engine only draws what it is given.
#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "EngineLiveFraming.h"
#include "HostMessages.h"
#include "StringConv.h"
#include <algorithm>

namespace host
{
using nlohmann::json;

namespace
{
// The host paces its editor to this rate while streaming; our viewport never shows more.
constexpr int    kLiveMaxFps       = 60;
constexpr double kLiveStartSeconds = engine_preview::kStillBudgetSeconds;
// The host's limit for live-refresh, which reloads the effect and may reopen the map.
constexpr double kLiveRefreshSeconds = 60.0;
// No new frame for this long means the host stopped presenting (minimized, hung or busy).
constexpr double kLiveStaleSeconds = 2.0;
// Used only when no scene viewport has been laid out yet.
constexpr int kLiveFallbackWidthPx  = 1280;
constexpr int kLiveFallbackHeightPx = 720;

bool IsHostGone(const json& payload)
{
    const std::string code = payload.value("code", "");
    return code == "absent" || code == "disconnected";
}
} // namespace

const char* BridgeDispatcher::EngineLiveStateName() const
{
    switch (m_engineLiveState)
    {
        case EngineLiveState::Starting: return "starting";
        case EngineLiveState::Live:     return "live";
        default:                        return "off";
    }
}

void BridgeDispatcher::EmitEngineLiveStateChanged(const std::string& message)
{
    if (!m_emit) return;
    json payload = {{"state", EngineLiveStateName()}};
    if (!message.empty()) payload["message"] = message;
    m_emit(json{{"type", "evt"}, {"kind", "engineLive/state-changed"}, {"payload", payload}});
}

bool BridgeDispatcher::StartEngineLive(BridgeRequestContext& ctx)
{
    if (!IsEnginePreviewReady())
    {
        ctx.SendErr("Check engine preview status before starting the live engine view.");
        return false;
    }
    if (!ctx.RequireEngine("live engine view")) return false;
    if (!m_pParticleSystem || !*m_pParticleSystem)
    {
        ctx.SendErr("Open an effect before starting the live engine view.");
        return false;
    }
    int x = 0, y = 0, widthPx = kLiveFallbackWidthPx, heightPx = kLiveFallbackHeightPx;
    m_engine->GetSceneViewport(x, y, widthPx, heightPx);
    const engine_live::ParkSize park = engine_live::ParkSizeForViewport(widthPx, heightPx);
    try
    {
        std::string error;
        if (!WriteEnginePreviewDocumentCopy(error))
        {
            ctx.SendErr(error);
            return false;
        }
    }
    catch (const std::exception& error)
    {
        ctx.SendErr(error.what());
        return false;
    }
    if (!m_engineLiveWorker)
        m_engineLiveWorker = std::make_unique<EnginePreviewWorker>(m_hostHwnd, WM_APP_ENGINE_PREVIEW_READY);
    EnginePreviewWorker::Job job;
    job.operation     = EnginePreviewWorker::Operation::Command;
    job.command       = "live-start";
    job.token         = ++m_engineLiveToken;
    job.pipeName      = m_enginePreviewPipe;
    job.budgetSeconds = kLiveStartSeconds;
    job.args          = {
        {"width", park.widthPx}, {"height", park.heightPx}, {"max_fps", kLiveMaxFps}, {"timeout_s", kLiveStartSeconds}};
    if (!m_engineLiveWorker->Enqueue(std::move(job)))
    {
        ctx.SendErr("The live engine view worker is busy or stopping.");
        return false;
    }
    m_engineLiveSync = engine_live::EngineLiveSync{};
    m_engineLiveSync.NoteParked(park.widthPx, park.heightPx);
    m_engineLiveState = EngineLiveState::Starting;
    EmitEngineLiveStateChanged("");
    return true;
}

// The worker runs one job at a time and refuses others while busy (a live-start still in
// flight), so a refused stop is kept pending and sent when the worker's current job finishes.
void BridgeDispatcher::SendEngineLiveStop()
{
    m_engineLiveStopPending = false;
    if (!m_engineLiveWorker) return;
    EnginePreviewWorker::Job job;
    job.operation     = EnginePreviewWorker::Operation::Command;
    job.command       = "live-stop";
    job.token         = m_engineLiveToken;
    job.pipeName      = m_enginePreviewPipe;
    job.budgetSeconds = engine_preview::kCommandBudgetSeconds;
    if (!m_engineLiveWorker->Enqueue(std::move(job))) m_engineLiveStopPending = true;
}

// Returns to our own renderer at once; the host is told to stop streaming in the background.
void BridgeDispatcher::StopEngineLive(const std::string& reason)
{
    const bool wasOff = m_engineLiveState == EngineLiveState::Off;
    m_engineLiveReader.reset();
    if (m_engine) m_engine->SetLiveViewActive(false);
    m_engineLiveState = EngineLiveState::Off;
    ++m_engineLiveToken; // a live-start still in flight is now stale
    if (wasOff) return;
    SendEngineLiveStop();
    EmitEngineLiveStateChanged(reason);
}

bool BridgeDispatcher::TryDispatchEngineLive(BridgeRequestContext& ctx)
{
    const std::string& kind = ctx.kind;
    if (kind != "engineLive/start" && kind != "engineLive/stop" && kind != "engineLive/stats") return false;
    if (!m_enginePreviewAllowed)
    {
        ctx.SendErr("the engine preview is not available in this run");
        return true;
    }
    if (kind == "engineLive/stats")
    {
        const std::optional<double> latencyMs = m_engineLiveSync.CameraLatencyMs();
        ctx.SendOk({{"state", EngineLiveStateName()},
                    {"fps", m_engineLiveState == EngineLiveState::Live ? m_engineLiveSync.FramesPerSecond() : 0.0},
                    {"cameraLatencyMs", latencyMs ? json(*latencyMs) : json(nullptr)}});
        return true;
    }
    if (kind == "engineLive/stop")
    {
        StopEngineLive("");
        ctx.SendOk({{"state", "off"}});
        return true;
    }
    if (m_engineLiveState != EngineLiveState::Off)
    {
        ctx.SendOk({{"state", EngineLiveStateName()}});
        return true;
    }
    if (StartEngineLive(ctx)) ctx.SendOk({{"state", "starting"}});
    return true;
}

void BridgeDispatcher::DrainEngineLiveResults()
{
    if (!m_engineLiveWorker) return;
    for (EnginePreviewWorker::Result& result : m_engineLiveWorker->TakeFinished())
    {
        if (result.token != m_engineLiveToken) continue;
        if (m_engineLiveState == EngineLiveState::Starting)
            FinishEngineLiveStart(result);
        else if (m_engineLiveState == EngineLiveState::Live)
            FinishEngineLiveCommand(result);
    }
    if (m_engineLiveStopPending) SendEngineLiveStop();
}

void BridgeDispatcher::FinishEngineLiveStart(const EnginePreviewWorker::Result& result)
{
    if (!result.payload.value("ok", false))
    {
        // A start that failed late (a timeout) may already have started the stream.
        StopEngineLive(result.payload.value("error", std::string("The live engine view did not start.")));
        return;
    }
    try
    {
        const json& reply  = result.payload.at("result");
        m_engineLiveReader = std::make_unique<EngineLiveReader>(Utf8ToWide(reply.at("section").get<std::string>()),
                                                                Utf8ToWide(reply.at("event").get<std::string>()),
                                                                engine_preview::ParsePipePid(result.pipeName));
        m_engineLiveState  = EngineLiveState::Live;
        EmitEngineLiveStateChanged("");
    }
    catch (const std::exception& error)
    {
        StopEngineLive(std::string("The live engine view did not start: ") + error.what());
    }
}

void BridgeDispatcher::FinishEngineLiveCommand(const EnginePreviewWorker::Result& result)
{
    const bool isAnchorRequest =
        m_engineLiveSync.IsAnchorRequest(engine_live::LiveCommand{result.command, result.args});
    const bool isOk = result.payload.value("ok", false);
    if (isOk && isAnchorRequest)
    {
        try
        {
            const json&                             reply = result.payload.at("result");
            std::optional<engine_live::FieldLimits> fieldLimits;
            const bool hasFieldLimits = reply.contains("fov_min") && reply["fov_min"].is_number() &&
                                        reply.contains("fov_max") && reply["fov_max"].is_number();
            if (hasFieldLimits)
            {
                fieldLimits = engine_live::FieldLimits{reply["fov_min"].get<double>(), reply["fov_max"].get<double>()};
            }
            m_engineLiveSync.NoteAnchor(reply.at("target").get<std::array<double, 3>>(), fieldLimits);
            return;
        }
        catch (const json::exception& error)
        {
            OutputDebugStringA((std::string("[live-view] ERROR opening camera reply: ") + error.what() + "\n").c_str());
        }
    }
    if (isAnchorRequest) m_engineLiveSync.NoteAnchorFailed();
    if (!isOk) ReportEngineLiveCommandFailure(result);
}

// A refresh, camera or park that fails leaves the stream running on the last state the host
// accepted, so the view stays live and the failure is shown; a vanished host ends it.
void BridgeDispatcher::ReportEngineLiveCommandFailure(const EnginePreviewWorker::Result& result)
{
    if (IsHostGone(result.payload))
    {
        StopEngineLive("The engine host closed; showing the editor's own view.");
        return;
    }
    const std::string error = result.payload.value("error", std::string("unknown error"));
    OutputDebugStringA(("[live-view] ERROR live command failed: " + error + "\n").c_str());
    EmitEngineLiveStateChanged("The engine view missed an update: " + error);
}

// Every way the editor's view changes (orbit, pan, zoom, reset, resize) ends up in the engine's
// camera and scene viewport, so reading both each frame needs no hook in each input path.
void BridgeDispatcher::NoteEngineLiveView(std::uint64_t nowMs)
{
    int x = 0, y = 0, widthPx = kLiveFallbackWidthPx, heightPx = kLiveFallbackHeightPx;
    m_engine->GetSceneViewport(x, y, widthPx, heightPx);
    // A minimized window lays the viewport out at zero size; the host keeps its last view.
    if (widthPx <= 0 || heightPx <= 0) return;
    const engine_live::ParkSize park = engine_live::ParkSizeForViewport(widthPx, heightPx);
    m_engineLiveSync.NoteViewportSize(park.widthPx, park.heightPx, nowMs);

    const engine_live::FieldLimits limits = m_engineLiveSync.HostFieldLimits();
    const engine_live::HostLens    lens =
        engine_live::MatchHostLens(m_engine->GetSceneFovYRadians(), widthPx, heightPx, limits);
    const Engine::Camera& camera = m_engine->GetCamera();
    try
    {
        const engine_preview::Orbit orbit = engine_preview::LiveOrbitFromLookAt(
            {camera.Position.x, camera.Position.y, camera.Position.z},
            {camera.Target.x, camera.Target.y, camera.Target.z}, lens.distanceScale);
        m_engineLiveSync.NoteCamera(orbit, lens.fieldDegrees);
    }
    catch (const engine_preview::Failure&)
    {
        // A camera the host cannot take (panned past its limits, or looking too nearly level
        // for its ground-bound target) keeps the last one sent;
        // the user sees the live view stop following, and panning back resumes it.
    }
}

// One command at a time: the host applies them in order, and a still in flight owns the
// host's clock and camera until it finishes.
void BridgeDispatcher::SendNextEngineLiveCommand(std::uint64_t nowMs)
{
    if (m_enginePreviewPending || !m_engineLiveWorker || m_engineLiveWorker->IsBusy()) return;
    std::optional<engine_live::LiveCommand> next = m_engineLiveSync.TakeNextCommand(nowMs);
    if (!next) return;
    EnginePreviewWorker::Job job;
    job.operation     = EnginePreviewWorker::Operation::Command;
    job.command       = next->command;
    job.args          = std::move(next->args);
    job.token         = m_engineLiveToken;
    job.pipeName      = m_enginePreviewPipe;
    job.budgetSeconds = engine_preview::kCommandBudgetSeconds;
    if (job.command == engine_live::kRefreshCommand)
    {
        std::string error;
        bool        isWritten = false;
        try
        {
            isWritten = WriteEnginePreviewDocumentCopy(error);
        }
        catch (const std::exception& exception)
        {
            error = exception.what();
        }
        if (!isWritten)
        {
            EmitEngineLiveStateChanged("The edit could not be sent to the engine view: " + error);
            return;
        }
        job.args          = {{"timeout_s", kLiveRefreshSeconds}};
        job.budgetSeconds = kLiveRefreshSeconds;
    }
    // IsBusy was false a moment ago, so only a worker that is shutting down refuses here.
    if (!m_engineLiveWorker->Enqueue(std::move(job)))
        OutputDebugStringA("[live-view] ERROR live command refused by the worker\n");
}

void BridgeDispatcher::PumpEngineLiveFrame()
{
    if (m_engineLiveState != EngineLiveState::Live || !m_engineLiveReader || !m_engine) return;
    if (m_engineLiveReader->HasHostExited())
    {
        StopEngineLive("The engine host closed; showing the editor's own view.");
        return;
    }
    const std::uint64_t    nowMs = GetTickCount64();
    engine_live::FrameInfo info;
    if (m_engineLiveReader->TakeNewestFrame(m_engineLivePixels, info))
    {
        const UINT pitchBytes = info.width * engine_live::kBytesPerPixel;
        // Our own scene stays on screen until the first frame has actually been uploaded.
        if (m_engine->UploadLiveFrame(m_engineLivePixels.data(), info.width, info.height, pitchBytes))
            m_engine->SetLiveViewActive(true);
        m_engineLiveSync.NoteFrameCameraTag(info.cameraTag, nowMs);
        m_engineLiveSync.NoteFrameShown(nowMs);
    }
    else if (m_engineLiveReader->SecondsSinceLastFrame() > kLiveStaleSeconds)
    {
        StopEngineLive("The engine host stopped sending frames; showing the editor's own view.");
        return;
    }
    NoteEngineLiveView(nowMs);
    SendNextEngineLiveCommand(nowMs);
}
} // namespace host
