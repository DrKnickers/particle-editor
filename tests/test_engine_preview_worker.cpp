// Exercises the real worker against an in-process pipe server, never the game.
// The fixture owns its pipe and temporary PNG and joins all threads on teardown.
#include "host/EnginePreviewWorker.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

using host::EnginePreviewWorker;
using nlohmann::json;
namespace
{
constexpr DWORD         kResultTimeoutMs = 8000;
constexpr DWORD         kPollMs          = 10;
constexpr ULONGLONG     kCancelLimitMs   = 1000;
constexpr double        kReplySeconds    = 2.6;
constexpr unsigned char kPng[]           = {137, 80, 78, 71, 13, 10, 26, 10};

void Require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

class FakeServer
{
public:
    using Reply = std::function<json(const json&)>;
    explicit FakeServer(Reply reply, bool hang = false) : m_reply(std::move(reply)), m_hang(hang)
    {
        m_name    = L"\\\\.\\pipe\\alamo-preview-" + std::to_wstring(GetCurrentProcessId());
        m_stop    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_ioReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_pipe =
            CreateNamedPipeW(m_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_WAIT, 1,
                             host::engine_preview::kMaxLineBytes, host::engine_preview::kMaxLineBytes, 0, nullptr);
        Require(m_stop != nullptr && m_ioReady != nullptr && m_pipe != INVALID_HANDLE_VALUE, "create fake pipe");
        m_thread = std::thread([this]() -> void { Serve(); });
    }
    ~FakeServer()
    {
        SetEvent(m_stop);
        if (m_thread.joinable()) m_thread.join();
        CloseHandle(m_pipe);
        CloseHandle(m_stop);
        CloseHandle(m_ioReady);
    }
    const std::wstring& Name() const { return m_name; }

private:
    bool AwaitCompletion(OVERLAPPED& operation)
    {
        const HANDLE events[]  = {m_stop, m_ioReady};
        const DWORD  completed = WaitForMultipleObjects(static_cast<DWORD>(std::size(events)), events, FALSE, INFINITE);
        if (completed == WAIT_OBJECT_0 + 1) return true;
        CancelIoEx(m_pipe, &operation);
        // The fixture also keeps pending I/O buffers alive until cancellation completes.
        WaitForSingleObject(m_ioReady, INFINITE);
        return false;
    }
    bool Transfer(bool writing, char* buffer, DWORD bytes, DWORD& transferred)
    {
        ResetEvent(m_ioReady);
        OVERLAPPED operation = {};
        operation.hEvent     = m_ioReady;
        const BOOL completed = writing ? WriteFile(m_pipe, buffer, bytes, &transferred, &operation)
                                       : ReadFile(m_pipe, buffer, bytes, &transferred, &operation);
        if (completed) return transferred != 0;
        if (GetLastError() != ERROR_IO_PENDING || !AwaitCompletion(operation)) return false;
        return GetOverlappedResult(m_pipe, &operation, &transferred, FALSE) && transferred != 0;
    }
    void Serve()
    {
        OVERLAPPED connection = {};
        connection.hEvent     = m_ioReady;
        if (!ConnectNamedPipe(m_pipe, &connection))
        {
            const DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING && !AwaitCompletion(connection)) return;
            if (error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED) return;
        }
        if (m_hang)
        {
            WaitForSingleObject(m_stop, INFINITE);
            return;
        }
        std::string request;
        char        letter      = 0;
        DWORD       transferred = 0;
        while (Transfer(false, &letter, 1, transferred))
        {
            if (letter != '\n')
            {
                request += letter;
                continue;
            }
            const json reply = m_reply(json::parse(request));
            if (reply.is_null())
            {
                DisconnectNamedPipe(m_pipe);
                return;
            }
            std::string line = reply.dump() + '\n';
            if (!Transfer(true, line.data(), static_cast<DWORD>(line.size()), transferred)) return;
            request.clear();
        }
    }
    Reply        m_reply;
    bool         m_hang;
    std::wstring m_name;
    HANDLE       m_pipe    = INVALID_HANDLE_VALUE;
    HANDLE       m_stop    = nullptr;
    HANDLE       m_ioReady = nullptr;
    std::thread  m_thread;
};

EnginePreviewWorker::Result WaitResult(EnginePreviewWorker& worker)
{
    const ULONGLONG deadline = GetTickCount64() + kResultTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        std::vector<EnginePreviewWorker::Result> results = worker.TakeFinished();
        if (!results.empty()) return results.front();
        Sleep(kPollMs);
    }
    throw std::runtime_error("worker failed to finish within test budget");
}

EnginePreviewWorker::Job StillJob(const FakeServer& server)
{
    EnginePreviewWorker::Job job;
    job.operation     = EnginePreviewWorker::Operation::Still;
    job.pipeName      = server.Name();
    job.args          = {{"time_s", 0}, {"random_state", 1}};
    job.budgetSeconds = 0;
    return job;
}

void TestSuccess(const std::filesystem::path& png)
{
    FakeServer server(
        [&](const json& request) -> json
        {
            Require(request["cmd"] == "preview-still" && request["args"]["random_state"] == 1,
                    "still command and seed");
            return {{"id", request["id"]},
                    {"ok", true},
                    {"result", {{"path", png.u8string()}, {"width", 1}, {"height", 1}, {"seconds", kReplySeconds}}}};
        });
    EnginePreviewWorker worker(nullptr, 0);
    Require(worker.Enqueue(StillJob(server)), "enqueue success job");
    const EnginePreviewWorker::Result result = WaitResult(worker);
    Require(result.payload["ok"] == true && result.payload["seconds"] == kReplySeconds, result.payload.dump().c_str());
    Require(result.payload["dataUri"].get<std::string>().rfind("data:image/png;base64,", 0) == 0, "PNG data URI");
}

void TestStatus()
{
    FakeServer server(
        [](const json& request) -> json
        {
            json result = json::object();
            if (request["cmd"] == "version") result = {{"mapped", true}, {"host_version", "1.0.0"}};
            if (request["cmd"] == "preview-info") result = {{"effect_path", "configured"}, {"internal_name", "effect"}};
            return {{"id", request["id"]}, {"ok", true}, {"result", result}};
        });
    EnginePreviewWorker      worker(nullptr, 0);
    EnginePreviewWorker::Job job;
    job.pipeName = server.Name();
    Require(worker.Enqueue(job), "enqueue status");
    Require(WaitResult(worker).payload["state"] == "ready", "ping/version/config status flow");
}

void TestFailure(const std::string& code)
{
    FakeServer server(
        [&](const json& request) -> json
        {
            if (code == "disconnected") return nullptr;
            return {{"id", request["id"]}, {"ok", false}, {"error", {{"code", code}, {"message", "fake host"}}}};
        });
    EnginePreviewWorker worker(nullptr, 0);
    Require(worker.Enqueue(StillJob(server)), "enqueue failure job");
    const EnginePreviewWorker::Result result = WaitResult(worker);
    Require(result.payload["ok"] == false && result.payload["code"] == code, "host failure preserved");
}

void TestHung(bool cancel, bool shutdown)
{
    FakeServer          server([](const json&) -> json { return nullptr; }, true);
    EnginePreviewWorker worker(nullptr, 0);
    Require(worker.Enqueue(StillJob(server)), "enqueue hung job");
    Sleep(kPollMs);
    const ULONGLONG started = GetTickCount64();
    if (shutdown)
    {
        worker.Finish();
        Require(GetTickCount64() - started < kCancelLimitMs, "shutdown joins promptly");
        return;
    }
    if (cancel) worker.Cancel();
    const EnginePreviewWorker::Result result = WaitResult(worker);
    Require(result.payload["code"] == (cancel ? "cancelled" : "timeout"), "cancel or transport timeout");
    if (cancel) Require(GetTickCount64() - started < kCancelLimitMs, "cancel interrupts pipe read");
}
} // namespace

int main()
{
    const std::filesystem::path png = std::filesystem::current_path() / "tests" /
                                      ("engine-preview-" + std::to_string(GetCurrentProcessId()) + ".png");
    try
    {
        {
            std::ofstream image(png, std::ios::binary);
            image.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
        }
        TestSuccess(png);
        TestStatus();
        TestFailure("timeout");
        TestFailure("disconnected");
        TestHung(false, false);
        TestHung(true, false);
        TestHung(false, true);
        std::filesystem::remove(png);
        std::puts("engine preview worker: ALL PASS");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::filesystem::remove(png);
        std::printf("FAIL: %s\n", error.what());
        return 1;
    }
}
