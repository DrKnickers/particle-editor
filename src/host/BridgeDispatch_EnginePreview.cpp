// Interactive engine-preview bridge handlers and result delivery. Document copies
// are saved here on the UI thread; the worker exclusively owns pipe and PNG I/O.
#include "BridgeDispatcher.h"
#include "BridgeDispatchShared.h"
#include "BridgeRequestContext.h"
#include "EnginePreviewCopy.h"
#include "HostMessages.h"
#include "ModulePath.h"
#include "StringConv.h"
#include "SettingsRegistry.h"
#include "EngineHostSetupPolicy.h"
#include "gamedata/managers.h"
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
    if (m_engineHostSetup)
    {
        // Closing during setup still persists its result, or finishes a failed save's rollback.
        do
        {
            m_engineHostSetup->Finish();
            DrainEngineHostSetupResults();
        } while (m_engineHostSetup->IsBusy());
    }
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

bool BridgeDispatcher::CanUseEnginePreview() const
{
    // Keep explicit start controls reachable before the configured host has been launched.
    return m_enginePreviewAllowed && (IsEnginePreviewReady() || !m_engineHostExePath.empty());
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

bool BridgeDispatcher::BuildStillOrLaunchJob(BridgeRequestContext& ctx, EnginePreviewWorker::Job& job)
{
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
    if (!IsEnginePreviewReady())
    {
        job.operation = EnginePreviewWorker::Operation::Launch;
        job.exePath   = m_engineHostExePath;
        // Resume the still with its original bridge parameters after host configuration arrives.
        job.args = params;
        return true;
    }
    job.exePath   = m_engineHostExePath;
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

namespace
{
bool SaveEngineHostExePath(const std::wstring& path, std::string& error)
{
    HKEY settings = OpenSettingsKeyForWrite();
    if (!settings)
    {
        error = "Cannot open the engine host settings for writing.";
        return false;
    }
    LONG saved = ERROR_SUCCESS;
    if (path.empty())
        saved = RegDeleteValueW(settings, kRegEngineHostExePath);
    else
        saved = RegSetValueExW(settings, kRegEngineHostExePath, 0, REG_SZ, reinterpret_cast<const BYTE*>(path.c_str()),
                               static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(settings);
    if (saved == ERROR_SUCCESS || (path.empty() && saved == ERROR_FILE_NOT_FOUND)) return true;
    error = "Cannot save the engine host executable path.";
    return false;
}
} // namespace

bool BridgeDispatcher::TryDispatchEngineHostSettings(BridgeRequestContext& ctx)
{
    const bool isGet = ctx.kind == "engineHost/settings";
    if (!isGet && ctx.kind != "engineHost/settings/set") return false;
    if (isGet)
    {
        ctx.SendOk({{"exePath", m_engineHostExePath.empty() ? json(nullptr) : json(WideToUtf8(m_engineHostExePath))}});
        return true;
    }
    if (m_engineHostSetup && m_engineHostSetup->IsBusy())
    {
        ctx.SendErr("Engine host setup is already running.");
        return true;
    }
    const json::const_iterator value = ctx.params.find("exePath");
    if (value == ctx.params.end() || (!value->is_null() && !value->is_string()))
    {
        ctx.SendErr("exePath must be a string or null.");
        return true;
    }
    const std::wstring path = value->is_null() ? L"" : Utf8ToWide(value->get<std::string>());
    if (!value->is_null())
    {
        if (!EngineHostProcess::IsValidExePath(path))
        {
            ctx.SendErr("exePath must be an existing absolute .exe path.");
            return true;
        }
    }
    std::string error;
    if (PersistsUserState() && !SaveEngineHostExePath(path, error))
    {
        ctx.SendErr(error);
        return true;
    }
    m_engineHostExePath = path;
    EmitEngineStateChanged();
    ctx.SendOk({{"exePath", path.empty() ? json(nullptr) : json(WideToUtf8(path))}});
    return true;
}

bool BridgeDispatcher::TryDispatchEngineHostSetup(BridgeRequestContext& ctx)
{
    const bool isStatus     = ctx.kind == "engineHost/setup/status";
    const bool isStart      = ctx.kind == "engineHost/setup/start";
    const bool isRemove     = ctx.kind == "engineHost/setup/remove";
    const bool isRecognized = isStatus || isStart || isRemove;
    if (!isRecognized) return false;
    if (!m_enginePreviewAllowed)
    {
        ctx.SendErr("the engine preview is not available in this run");
        return true;
    }
    if (isStatus)
    {
        try
        {
            const std::wstring target = EngineHostSetup::ManagedPath();
            const bool         isManaged =
                std::filesystem::exists(std::filesystem::path(target) / engine_host::kSetupJournalName);
            ctx.SendOk({{"managedPath", isManaged ? json(WideToUtf8(target)) : json(nullptr)},
                        {"isRunning", EngineHostSetup::IsRunning(target)}});
        }
        catch (const std::exception& failure)
        {
            ctx.SendErr(failure.what());
        }
        return true;
    }
    const bool isPreviewActive = m_enginePreviewPending || m_engineLiveState != EngineLiveState::Off;
    if (isPreviewActive)
    {
        ctx.SendErr("Stop the live engine view and wait for preview requests before changing setup.");
        return true;
    }
    try
    {
        m_engineHostProcess.StopStarted();
    }
    catch (const std::exception& failure)
    {
        ctx.SendErr(failure.what());
        return true;
    }
    EngineHostSetup::Job job;
    job.remove = isRemove;
    if (isStart)
    {
        const json::const_iterator editor = ctx.params.find("editorExePath");
        if (editor == ctx.params.end() || !editor->is_string())
        {
            ctx.SendErr("editorExePath must be a string.");
            return true;
        }
        const std::filesystem::path moduleFolder = host::ModuleDirectory();
        if (moduleFolder.empty())
        {
            ctx.SendErr("Cannot resolve the Particle Editor folder.");
            return true;
        }
        if (!m_fileManager)
        {
            ctx.SendErr("Game paths are unavailable for engine preview setup.");
            return true;
        }
        job.editorExePath = Utf8ToWide(editor->get<std::string>());
        job.packageManifestPath =
            (moduleFolder / engine_host::kBundledPackageFolder / engine_host::kPackageManifestName).wstring();
        job.basepaths    = m_fileManager->GetBasepaths();
        job.contentRoots = m_fileManager->GetContentRoots();
    }
    if (!m_engineHostSetup) m_engineHostSetup = std::make_unique<EngineHostSetup>(m_hostHwnd);
    std::string error;
    if (!m_engineHostSetup->Start(std::move(job), error))
    {
        ctx.SendErr(error);
        return true;
    }
    ctx.SendOk({{"ok", true}});
    return true;
}

bool BridgeDispatcher::ApplyEngineHostSetupResult(json& payload)
{
    if (!m_engineHostSetupSaveError.empty())
    {
        const std::string rollbackError = payload.value("error", std::string());
        payload = {{"ok", false},
                   {"action", "setup"},
                   {"error", m_engineHostSetupSaveError +
                                 (rollbackError.empty() ? " Setup was rolled back." : " Rollback: " + rollbackError)}};
        m_engineHostSetupSaveError.clear();
        return true;
    }
    if (!payload.value("ok", false)) return true;
    const bool         isSetup = payload.at("action") == "setup";
    const std::wstring target  = EngineHostSetup::ManagedPath();
    const bool         clearsPath =
        !isSetup && engine_host::IsSetupPathWithin(WideToUtf8(m_engineHostExePath), WideToUtf8(target));
    if (!isSetup && !clearsPath) return true;
    const std::wstring path = isSetup ? Utf8ToWide(payload.at("exePath").get<std::string>()) : L"";
    std::string        error;
    if (PersistsUserState() && !SaveEngineHostExePath(path, error))
    {
        if (isSetup)
        {
            // Registry failure must not leave a successful-looking installation behind.
            EngineHostSetup::Job rollback;
            rollback.remove = true;
            std::string rollbackError;
            if (m_engineHostSetup->Start(std::move(rollback), rollbackError))
            {
                m_engineHostSetupSaveError = error;
                return false;
            }
            error += " Rollback: " + rollbackError;
        }
        payload["ok"]    = false;
        payload["error"] = error;
        payload.erase("exePath");
        return true;
    }
    m_engineHostExePath   = path;
    m_enginePreviewStatus = {{"state", "absent"}};
    m_enginePreviewPipe.clear();
    EmitEngineStateChanged();
    return true;
}

void BridgeDispatcher::DrainEngineHostSetupResults()
{
    if (!m_engineHostSetup) return;
    for (EngineHostSetup::Event& event : m_engineHostSetup->TakeEvents())
    {
        if (event.finished)
        {
            // A throw here would escape the window procedure; report it as the setup's failure instead.
            try
            {
                if (!ApplyEngineHostSetupResult(event.payload)) continue;
            }
            catch (const std::exception& failure)
            {
                event.payload = {
                    {"ok", false}, {"action", event.payload.value("action", "setup")}, {"error", failure.what()}};
            }
        }
        // The setup worker emits these facts; the later Preferences UI consumes them.
        if (m_emit)
            m_emit(json{{"type", "evt"},
                        {"kind", event.finished ? "engineHost/setup-finished" : "engineHost/setup-progressed"},
                        {"payload", event.payload}});
    }
}

bool BridgeDispatcher::TryDispatchEnginePreview(BridgeRequestContext& ctx)
{
    const std::string& kind = ctx.kind;
    if (TryDispatchEngineHostSetup(ctx)) return true;
    if (TryDispatchEngineHostSettings(ctx)) return true;
    const bool recognized =
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
    if (m_engineHostSetup && m_engineHostSetup->IsBusy())
    {
        ctx.SendErr("Engine host setup is already running.");
        return true;
    }
    if (m_enginePreviewPending)
    {
        ctx.SendErr("An engine preview request is already running. Cancel it before retrying.");
        return true;
    }
    if (!m_enginePreviewWorker)
        m_enginePreviewWorker =
            std::make_unique<EnginePreviewWorker>(m_hostHwnd, WM_APP_ENGINE_PREVIEW_READY, &m_engineHostProcess);
    EnginePreviewWorker::Job job;
    job.token = ++m_enginePreviewToken;
    if (kind == "enginePreview/status")
    {
        job.operation = EnginePreviewWorker::Operation::Status;
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
    if (kind == "enginePreview/still" && !BuildStillOrLaunchJob(ctx, job)) return true;
    if (!m_enginePreviewWorker->Enqueue(std::move(job)))
    {
        ctx.SendErr("The engine preview worker is busy or stopping.");
        return true;
    }
    m_enginePreviewPending = true;
    ctx.SendOk({{"status", "pending"}, {"requestId", m_enginePreviewToken}});
    return true;
}

bool BridgeDispatcher::ResumeStillAfterHostLaunch(EnginePreviewWorker::Result& result)
{
    m_enginePreviewStatus = result.payload;
    m_enginePreviewPipe   = result.pipeName;
    EmitEngineStateChanged();
    EnginePreviewWorker::Job still;
    still.token = result.token;
    const std::string    id;
    const std::string    kind = "enginePreview/still";
    BridgeRequestContext context{*this, id, kind, result.args, json::object()};
    if (IsEnginePreviewReady() && BuildStillOrLaunchJob(context, still))
    {
        if (m_enginePreviewWorker->Enqueue(std::move(still)))
        {
            m_enginePreviewPending = true;
            return true;
        }
        context.SendErr("The engine preview worker is busy or stopping.");
    }
    const std::string error = context.res.value(
        "error", result.payload.value("error", std::string("The engine host is not ready for preview.")));
    result.payload   = {{"ok", false}, {"error", error}};
    result.operation = EnginePreviewWorker::Operation::Still;
    return false;
}

void BridgeDispatcher::DrainEnginePreviewResults()
{
    if (!m_enginePreviewWorker) return;
    for (EnginePreviewWorker::Result& result : m_enginePreviewWorker->TakeFinished())
    {
        if (result.token != m_enginePreviewToken) continue;
        m_enginePreviewPending = false;
        if (result.operation == EnginePreviewWorker::Operation::Launch && ResumeStillAfterHostLaunch(result)) continue;
        const bool status = result.operation == EnginePreviewWorker::Operation::Status;
        if (!status && m_engineLiveState == EngineLiveState::Live) m_engineLiveSync.NoteStillFinished();
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
