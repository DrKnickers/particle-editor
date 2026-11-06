// Interactive engine-preview bridge handlers and result delivery. Document copies
// are saved here on the UI thread; the worker exclusively owns pipe and PNG I/O.
#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "EnginePreviewCopy.h"
#include "HostMessages.h"
#include "ModulePath.h"
#include "StringConv.h"
#include <filesystem>

namespace host
{
using nlohmann::json;

void BridgeDispatcher::SetEnginePreviewAllowed(bool allowed)
{
    m_enginePreviewAllowed = allowed;
}

void BridgeDispatcher::ShutdownEnginePreviewWorker()
{
    if (m_enginePreviewWorker) m_enginePreviewWorker->Finish();
    // Finish cancels the pipe, so this stop rarely reaches the host. That is acceptable: an
    // abandoned host only keeps pacing its editor, and the next live-start takes it over.
    StopEngineLive("");
    if (m_engineLiveWorker) m_engineLiveWorker->Finish();
}

bool BridgeDispatcher::IsEnginePreviewReady() const
{
    return m_enginePreviewStatus.value("state", "absent") == "ready";
}

bool BridgeDispatcher::WriteEnginePreviewDocumentCopy(std::string& error)
{
    constexpr UINT kWindowsPathChars          = MAX_PATH + 1;
    wchar_t        windows[kWindowsPathChars] = {};
    const UINT     chars                      = GetWindowsDirectoryW(windows, kWindowsPathChars);
    if (chars == 0 || chars >= kWindowsPathChars)
        throw engine_preview::Failure("internal", "Cannot resolve the Windows directory.");
    return WriteEnginePreviewCopy(**m_pParticleSystem,
                                  Utf8ToWide(m_enginePreviewStatus.at("effectPath").get<std::string>()),
                                  m_enginePreviewStatus.at("internalName").get<std::string>(), ModuleDirectory(),
                                  windows, m_currentFilePath, error);
}

bool BridgeDispatcher::BuildStillJob(BridgeRequestContext& ctx, EnginePreviewWorker::Job& job)
{
    if (!IsEnginePreviewReady())
    {
        ctx.SendErr("Check engine preview status before requesting a still.");
        return false;
    }
    const json& params      = ctx.params;
    const bool  validTime   = params.contains("timeSeconds") && params["timeSeconds"].is_number();
    const bool  validCamera = params.contains("matchCamera") && params["matchCamera"].is_boolean();
    if (!validTime || !validCamera)
    {
        ctx.SendErr("timeSeconds and matchCamera are required");
        return false;
    }
    const double timeSeconds = params["timeSeconds"].get<double>();
    if (!std::isfinite(timeSeconds) || timeSeconds < 0 || timeSeconds > engine_preview::kMaxTimeSeconds)
    {
        ctx.SendErr("Preview time must be between 0 and " +
                    std::to_string(static_cast<unsigned>(engine_preview::kMaxTimeSeconds)) + " seconds.");
        return false;
    }
    if (!ctx.RequireEngine("engine preview")) return false;
    if (!m_pParticleSystem || !*m_pParticleSystem)
    {
        ctx.SendErr("Open an effect before requesting an engine preview.");
        return false;
    }
    job.operation = EnginePreviewWorker::Operation::Still;
    job.pipeName  = m_enginePreviewPipe;
    job.args      = {{"time_s", timeSeconds},
                     {"random_state", engine_preview::kPreviewRandomState},
                     {"scale", engine_preview::kPreviewCaptureScale},
                     {"timeout_s", engine_preview::kStillBudgetSeconds}};
    try
    {
        if (params["matchCamera"].get<bool>())
        {
            const Engine::Camera&       camera = m_engine->GetCamera();
            const engine_preview::Orbit orbit =
                engine_preview::OrbitFromLookAt({camera.Position.x, camera.Position.y, camera.Position.z},
                                                {camera.Target.x, camera.Target.y, camera.Target.z});
            job.args["camera"] = {
                {"pitch", orbit.pitch}, {"yaw", orbit.yaw}, {"distance", orbit.distance}, {"target", orbit.target}};
        }
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
    return true;
}

bool BridgeDispatcher::TryDispatchEnginePreview(BridgeRequestContext& ctx)
{
    const std::string& kind = ctx.kind;
    const bool         recognized =
        kind == "enginePreview/status" || kind == "enginePreview/still" || kind == "enginePreview/cancel";
    if (!recognized) return false;
    const ULONGLONG started = GetTickCount64();
    // Includes validation refusals and cancellation, which never reach the pipe worker.
    struct RequestLog
    {
        BridgeRequestContext& context;
        ULONGLONG             started;
        ~RequestLog()
        {
            const std::string code = context.res.value("ok", false) ? "accepted" : "refused";
            const std::string log  = "[engine-preview] " + context.kind + " code=" + code +
                                     " elapsed_ms=" + std::to_string(GetTickCount64() - started) + "\n";
            OutputDebugStringA(log.c_str());
        }
    } log{ctx, started};
    if (!m_enginePreviewAllowed)
    {
        ctx.SendErr("the engine preview is not available in this run");
        return true;
    }
    if (kind == "enginePreview/cancel")
    {
        if (m_enginePreviewWorker) m_enginePreviewWorker->Cancel();
        ctx.SendOk(json::object());
        return true;
    }
    if (m_enginePreviewPending)
    {
        ctx.SendErr("An engine preview request is already running. Cancel it before retrying.");
        return true;
    }
    if (!m_enginePreviewWorker)
        m_enginePreviewWorker = std::make_unique<EnginePreviewWorker>(m_hostHwnd, WM_APP_ENGINE_PREVIEW_READY);
    EnginePreviewWorker::Job job;
    job.token = ++m_enginePreviewToken;
    if (kind == "enginePreview/status")
    {
        if (ctx.params.contains("pid"))
        {
            const json& pid = ctx.params["pid"];
            if (!pid.is_number_unsigned() && !pid.is_number_integer())
            {
                ctx.SendErr("pid must be a positive integer");
                return true;
            }
            if (pid <= 0 || pid > (std::numeric_limits<std::uint32_t>::max)())
            {
                ctx.SendErr("pid is outside the process id range");
                return true;
            }
            job.pid = pid.get<std::uint32_t>();
        }
    }
    if (kind == "enginePreview/still" && !BuildStillJob(ctx, job)) return true;
    if (!m_enginePreviewWorker->Enqueue(std::move(job)))
    {
        ctx.SendErr("The engine preview worker is busy or stopping.");
        return true;
    }
    m_enginePreviewPending = true;
    ctx.SendOk({{"status", "pending"}, {"requestId", m_enginePreviewToken}});
    return true;
}

void BridgeDispatcher::DrainEnginePreviewResults()
{
    if (!m_enginePreviewWorker) return;
    for (EnginePreviewWorker::Result& result : m_enginePreviewWorker->TakeFinished())
    {
        if (result.token != m_enginePreviewToken) continue;
        m_enginePreviewPending = false;
        const bool status      = result.operation == EnginePreviewWorker::Operation::Status;
        if (status)
        {
            m_enginePreviewStatus = result.payload;
            m_enginePreviewPipe   = result.pipeName;
            EmitEngineStateChanged();
        }
        else if (result.payload.value("code", "") == "absent" || result.payload.value("code", "") == "disconnected")
        {
            m_enginePreviewStatus = {{"state", "absent"}};
            EmitEngineStateChanged();
        }
        result.payload.erase("effectPath");
        result.payload["requestId"] = result.token;
        if (m_emit)
            m_emit(json{{"type", "evt"},
                        {"kind", status ? "enginePreview/status-checked" : "enginePreview/still-rendered"},
                        {"payload", result.payload}});
    }
}
} // namespace host
