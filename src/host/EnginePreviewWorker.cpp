// Runs bounded pipe requests and loads the returned still on the worker thread.
// Result delivery uses the host's posted-message drain; no UI work happens here.
#include "EnginePreviewWorker.h"
#include "GdiplusEncode.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace host
{
namespace
{
using namespace engine_preview;
constexpr DWORD         kPollIntervalMs        = 20;
constexpr DWORD         kReadChunkBytes        = 4096;
constexpr size_t        kBytesPerMegabyte      = 1024 * 1024;
constexpr double        kMillisecondsPerSecond = 1000.0;
constexpr unsigned char kPngSignature[]        = {137, 80, 78, 71, 13, 10, 26, 10};

std::string JobLogName(const EnginePreviewWorker::Job& job)
{
    if (job.operation == EnginePreviewWorker::Operation::Status) return "status";
    if (job.operation == EnginePreviewWorker::Operation::Still) return "still";
    return job.command;
}

struct CloseEvent
{
    void operator()(void* event) const { CloseHandle(event); }
};

std::vector<std::wstring> EnumeratePipes()
{
    WIN32_FIND_DATAW entry  = {};
    HANDLE           search = FindFirstFileW(L"\\\\.\\pipe\\*", &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES) return {};
        throw Failure("internal", "Cannot enumerate engine preview pipes (error " + std::to_string(error) + ").");
    }
    std::vector<std::wstring> names;
    do
    {
        names.emplace_back(entry.cFileName);
    } while (FindNextFileW(search, &entry));
    const DWORD error = GetLastError();
    FindClose(search);
    if (error != ERROR_NO_MORE_FILES) throw Failure("internal", "Cannot enumerate engine preview pipes.");
    return names;
}

std::string ReadPng(const nlohmann::json& reply)
{
    const bool hasFields = reply.contains("path") && reply["path"].is_string() && reply.contains("width") &&
                           reply["width"].is_number_integer() && reply.contains("height") &&
                           reply["height"].is_number_integer() && reply.contains("seconds") &&
                           reply["seconds"].is_number();
    if (!hasFields) throw Failure("protocol", "The still reply is missing its image fields.");
    const double seconds = reply["seconds"].get<double>();
    if (reply["width"] <= 0 || reply["height"] <= 0 || !std::isfinite(seconds) || seconds < 0)
        throw Failure("protocol", "The still reply has invalid dimensions or timing.");
    const std::filesystem::path path = std::filesystem::u8path(reply["path"].get<std::string>());
    if (!path.is_absolute() || FoldPath(path.extension().string()) != ".png")
        throw Failure("protocol", "The still reply must name an absolute PNG path.");
    const std::uintmax_t bytes = std::filesystem::file_size(path);
    if (bytes < sizeof(kPngSignature) || bytes > kMaxImageBytes)
        throw Failure("internal", "The preview PNG is empty or exceeds the " +
                                      std::to_string(kMaxImageBytes / kBytesPerMegabyte) + " MB limit.");
    std::vector<std::uint8_t> png(static_cast<size_t>(bytes));
    std::ifstream             image(path, std::ios::binary);
    if (!image.read(reinterpret_cast<char*>(png.data()), static_cast<std::streamsize>(png.size())))
        throw Failure("internal", "Cannot read the engine preview PNG.");
    if (!std::equal(std::begin(kPngSignature), std::end(kPngSignature), png.begin()))
        throw Failure("protocol", "The engine preview file is not a PNG.");
    const std::string uri = "data:image/png;base64," + Base64Encode(png.data(), png.size());
    if (uri.size() > kMaxImageBytes)
        throw Failure("internal", "The preview image exceeds the " +
                                      std::to_string(kMaxImageBytes / kBytesPerMegabyte) + " MB bridge limit.");
    return uri;
}
} // namespace

EnginePreviewWorker::EnginePreviewWorker(HWND hwnd, UINT doneMessage) : m_hwnd(hwnd), m_doneMessage(doneMessage)
{
    m_worker = std::thread([this]() -> void { WorkerLoop(); });
}

EnginePreviewWorker::~EnginePreviewWorker()
{
    Finish();
}

bool EnginePreviewWorker::Enqueue(Job job)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping || m_busy) return false;
    m_cancelled = false;
    m_busy      = true;
    m_jobs.push_back(std::move(job));
    m_work.notify_one();
    return true;
}

void EnginePreviewWorker::ClosePipe()
{
    std::lock_guard<std::mutex> lock(m_pipeMutex);
    if (m_pipe == INVALID_HANDLE_VALUE) return;
    CancelIoEx(m_pipe, nullptr);
    CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
}

void EnginePreviewWorker::Cancel()
{
    m_cancelled = true;
    ClosePipe();
}

void EnginePreviewWorker::Finish()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    Cancel();
    m_work.notify_one();
    if (m_worker.joinable()) m_worker.join();
}

std::vector<EnginePreviewWorker::Result> EnginePreviewWorker::TakeFinished()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Result>         results;
    results.swap(m_finished);
    return results;
}

DWORD EnginePreviewWorker::Transfer(bool writing, char* buffer, DWORD bytes, ULONGLONG deadline)
{
    if (GetTickCount64() >= deadline)
        throw engine_preview::Failure("timeout", engine_preview::ErrorMessage("timeout", ""));
    const std::unique_ptr<void, CloseEvent> event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw engine_preview::Failure("internal", "Cannot create a pipe completion event.");
    OVERLAPPED operation = {};
    operation.hEvent     = event.get();
    DWORD transferred    = 0;
    {
        std::lock_guard<std::mutex> lock(m_pipeMutex);
        if (m_cancelled) throw engine_preview::Failure("cancelled", engine_preview::ErrorMessage("cancelled", ""));
        const BOOL completed = writing ? WriteFile(m_pipe, buffer, bytes, &transferred, &operation)
                                       : ReadFile(m_pipe, buffer, bytes, &transferred, &operation);
        if (completed && transferred != 0) return transferred;
        if (completed) throw engine_preview::Failure("disconnected", engine_preview::ErrorMessage("disconnected", ""));
        if (GetLastError() != ERROR_IO_PENDING)
            throw engine_preview::Failure("disconnected", engine_preview::ErrorMessage("disconnected", ""));
    }
    bool timedOut = false;
    for (;;)
    {
        const DWORD waited = WaitForSingleObject(event.get(), kPollIntervalMs);
        if (waited == WAIT_OBJECT_0) break;
        timedOut = GetTickCount64() >= deadline;
        if (m_cancelled || timedOut || waited == WAIT_FAILED)
        {
            ClosePipe();
            // Closing cancels the kernel operation; its buffers must outlive completion.
            WaitForSingleObject(event.get(), INFINITE);
            if (timedOut) throw engine_preview::Failure("timeout", engine_preview::ErrorMessage("timeout", ""));
            throw engine_preview::Failure("cancelled", engine_preview::ErrorMessage("cancelled", ""));
        }
    }
    std::lock_guard<std::mutex> lock(m_pipeMutex);
    if (m_cancelled) throw engine_preview::Failure("cancelled", engine_preview::ErrorMessage("cancelled", ""));
    if (!GetOverlappedResult(m_pipe, &operation, &transferred, FALSE) || transferred == 0)
        throw engine_preview::Failure("disconnected", engine_preview::ErrorMessage("disconnected", ""));
    return transferred;
}

nlohmann::json EnginePreviewWorker::Exchange(const std::string&    command,
                                             const nlohmann::json& args,
                                             double                budgetSeconds)
{
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline =
        started +
        static_cast<ULONGLONG>((budgetSeconds + engine_preview::kTransportGraceSeconds) * kMillisecondsPerSecond);
    try
    {
        const std::int64_t id      = m_nextId;
        std::string        line    = engine_preview::FrameRequest(m_nextId, command, args);
        size_t             written = 0;
        while (written < line.size())
            written += Transfer(true, line.data() + written, static_cast<DWORD>(line.size() - written), deadline);
        std::string reply;
        char        chunk[kReadChunkBytes];
        for (;;)
        {
            const DWORD received = Transfer(false, chunk, sizeof(chunk), deadline);
            reply.append(chunk, received);
            const size_t newline = reply.find('\n');
            if (newline != std::string::npos)
            {
                if (newline + 1 != reply.size())
                    throw engine_preview::Failure("protocol", "Unexpected data after the reply.");
                reply.pop_back();
                break;
            }
            if (reply.size() > engine_preview::kMaxLineBytes)
                throw engine_preview::Failure("protocol", "Engine preview reply is too large.");
        }
        nlohmann::json    result = engine_preview::ParseReply(reply, id);
        const std::string log =
            "[engine-preview] " + command + " code=ok elapsed_ms=" + std::to_string(GetTickCount64() - started) + "\n";
        OutputDebugStringA(log.c_str());
        return result;
    }
    catch (const engine_preview::Failure& error)
    {
        const std::string log = "[engine-preview] " + command + " code=" + error.code +
                                " elapsed_ms=" + std::to_string(GetTickCount64() - started) + "\n";
        OutputDebugStringA(log.c_str());
        throw;
    }
}

void EnginePreviewWorker::RunStillJob(const Job& job, Result& result)
{
    using namespace engine_preview;
    const nlohmann::json still = Exchange("preview-still", job.args, job.budgetSeconds);
    const std::string    uri   = ReadPng(still);
    if (m_cancelled) throw Failure("cancelled", ErrorMessage("cancelled", ""));
    result.payload = {{"ok", true},
                      {"dataUri", uri},
                      {"seconds", still["seconds"]},
                      {"width", still["width"]},
                      {"height", still["height"]}};
}

void EnginePreviewWorker::RunCommandJob(const Job& job, Result& result)
{
    const nlohmann::json reply = Exchange(job.command, job.args, job.budgetSeconds);
    if (m_cancelled) throw engine_preview::Failure("cancelled", engine_preview::ErrorMessage("cancelled", ""));
    result.payload = {{"ok", true}, {"result", reply}};
}

void EnginePreviewWorker::RunStatusJob(Result& result)
{
    using namespace engine_preview;
    Exchange("ping", nlohmann::json::object(), kCommandBudgetSeconds);
    const nlohmann::json version = Exchange("version", nlohmann::json::object(), kCommandBudgetSeconds);
    if (!version.contains("mapped") || !version["mapped"].is_boolean() || !version.contains("host_version") ||
        !version["host_version"].is_string())
        throw Failure("protocol", "The host version reply is invalid.");
    result.payload["hostVersion"] = version["host_version"];
    result.payload["state"]       = "unsupported-build";
    if (version["mapped"].get<bool>())
    {
        result.payload["state"]      = "not-configured";
        const nlohmann::json preview = Exchange("preview-info", nlohmann::json::object(), kCommandBudgetSeconds);
        if (!preview.contains("effect_path") || !preview["effect_path"].is_string() ||
            !preview.contains("internal_name") || !preview["internal_name"].is_string())
            throw Failure("protocol", "The host preview configuration reply is invalid.");
        result.payload["effectPath"]   = preview["effect_path"];
        result.payload["internalName"] = preview["internal_name"];
        result.payload["state"]        = "ready";
    }
}

EnginePreviewWorker::Result EnginePreviewWorker::RunJob(const Job& job)
{
    using namespace engine_preview;
    const ULONGLONG started = GetTickCount64();
    Result          result;
    result.operation = job.operation;
    result.token     = job.token;
    result.payload   = {{"state", "absent"}};
    try
    {
        if (m_cancelled) throw Failure("cancelled", ErrorMessage("cancelled", ""));
        result.pipeName = job.pipeName.empty() ? SelectPipe(EnumeratePipes(), job.pid) : job.pipeName;
        {
            std::lock_guard<std::mutex> lock(m_pipeMutex);
            if (m_cancelled) throw Failure("cancelled", ErrorMessage("cancelled", ""));
            m_pipe = CreateFileW(result.pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OVERLAPPED, nullptr);
            if (m_pipe == INVALID_HANDLE_VALUE)
            {
                const DWORD       error = GetLastError();
                const std::string code  = error == ERROR_FILE_NOT_FOUND ? "absent" : "internal";
                throw Failure(code, ErrorMessage(code, "Cannot connect to the engine preview pipe (error " +
                                                           std::to_string(error) + ")."));
            }
        }
        if (job.operation == Operation::Still)
            RunStillJob(job, result);
        else if (job.operation == Operation::Command)
            RunCommandJob(job, result);
        else
            RunStatusJob(result);
    }
    catch (const Failure& error)
    {
        if (job.operation != Operation::Status) result.payload = {{"ok", false}};
        result.payload["error"] = error.what();
        result.payload["code"]  = error.code;
    }
    catch (const std::exception& error)
    {
        result.payload = {{"ok", false},
                          {"state", "absent"},
                          {"error", std::string("Cannot load engine preview: ") + error.what()},
                          {"code", "internal"}};
    }
    ClosePipe();
    const std::string code = result.payload.value("code", std::string("ok"));
    const std::string log  = std::string("[engine-preview] ") + JobLogName(job) + " code=" + code +
                             " elapsed_ms=" + std::to_string(GetTickCount64() - started) + "\n";
    OutputDebugStringA(log.c_str());
    return result;
}

void EnginePreviewWorker::WorkerLoop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_work.wait(lock, [this]() -> bool { return m_stopping || !m_jobs.empty(); });
            if (m_stopping) return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }
        Result result = RunJob(job);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_finished.push_back(std::move(result));
            m_busy = false;
        }
        if (m_hwnd && !PostMessageW(m_hwnd, m_doneMessage, 0, 0))
            OutputDebugStringA("[engine-preview] result notification failed\n");
    }
}
} // namespace host
