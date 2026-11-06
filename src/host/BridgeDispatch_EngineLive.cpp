// Live engine view bridge (#949 3b.3): starts and stops the engine host's frame stream and
// hands each new frame to the engine. The worker owns the pipe; EngineLiveReader owns the
// shared memory; the engine only draws what it is given.
#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
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
// No new frame for this long means the host stopped presenting (minimized, hung or busy).
constexpr double kLiveStaleSeconds = 2.0;
constexpr int    kLiveMinEdgePx    = 64;
// Used only when no scene viewport has been laid out yet.
constexpr int kLiveFallbackWidthPx  = 1280;
constexpr int kLiveFallbackHeightPx = 720;
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
    // Larger viewports stream at the host's maximum and are letterboxed up by the engine.
    widthPx  = std::clamp(widthPx, kLiveMinEdgePx, static_cast<int>(engine_live::kMaxWidthPx));
    heightPx = std::clamp(heightPx, kLiveMinEdgePx, static_cast<int>(engine_live::kMaxHeightPx));
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
    job.args = {{"width", widthPx}, {"height", heightPx}, {"max_fps", kLiveMaxFps}, {"timeout_s", kLiveStartSeconds}};
    if (!m_engineLiveWorker->Enqueue(std::move(job)))
    {
        ctx.SendErr("The live engine view worker is busy or stopping.");
        return false;
    }
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
    if (kind != "engineLive/start" && kind != "engineLive/stop") return false;
    if (!m_enginePreviewAllowed)
    {
        ctx.SendErr("the engine preview is not available in this run");
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
        const bool isCurrentStart = result.token == m_engineLiveToken && m_engineLiveState == EngineLiveState::Starting;
        if (!isCurrentStart) continue;
        if (!result.payload.value("ok", false))
        {
            // A start that failed late (a timeout) may already have started the stream.
            StopEngineLive(result.payload.value("error", std::string("The live engine view did not start.")));
            continue;
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
    if (m_engineLiveStopPending) SendEngineLiveStop();
}

void BridgeDispatcher::PumpEngineLiveFrame()
{
    if (m_engineLiveState != EngineLiveState::Live || !m_engineLiveReader || !m_engine) return;
    if (m_engineLiveReader->HasHostExited())
    {
        StopEngineLive("The engine host closed; showing the editor's own view.");
        return;
    }
    engine_live::FrameInfo info;
    if (m_engineLiveReader->TakeNewestFrame(m_engineLivePixels, info))
    {
        const UINT pitchBytes = info.width * engine_live::kBytesPerPixel;
        // Our own scene stays on screen until the first frame has actually been uploaded.
        if (m_engine->UploadLiveFrame(m_engineLivePixels.data(), info.width, info.height, pitchBytes))
            m_engine->SetLiveViewActive(true);
        return;
    }
    if (m_engineLiveReader->SecondsSinceLastFrame() > kLiveStaleSeconds)
        StopEngineLive("The engine host stopped sending frames; showing the editor's own view.");
}
} // namespace host
