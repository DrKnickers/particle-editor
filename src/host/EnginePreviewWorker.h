// Owns one background pipe client and its result queue. The dispatcher owns this
// worker; it never receives a live document or calls the engine or WebView.
#ifndef HOST_ENGINE_PREVIEW_WORKER_H
#define HOST_ENGINE_PREVIEW_WORKER_H

#include <windows.h>
#include "EnginePreviewPipe.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace host
{
class EnginePreviewWorker
{
public:
    enum class Operation
    {
        Status,
        Still
    };
    struct Job
    {
        Operation      operation = Operation::Status;
        std::uint64_t  token     = 0;
        std::uint32_t  pid       = 0;
        std::wstring   pipeName;
        nlohmann::json args          = nlohmann::json::object();
        double         budgetSeconds = engine_preview::kStillBudgetSeconds;
    };
    struct Result
    {
        Operation      operation = Operation::Status;
        std::uint64_t  token     = 0;
        std::wstring   pipeName;
        nlohmann::json payload = nlohmann::json::object();
    };

    EnginePreviewWorker(HWND hwnd, UINT doneMessage);
    ~EnginePreviewWorker();
    EnginePreviewWorker(const EnginePreviewWorker&)            = delete;
    EnginePreviewWorker& operator=(const EnginePreviewWorker&) = delete;
    bool                 Enqueue(Job job);
    void                 Cancel();
    void                 Finish();
    std::vector<Result>  TakeFinished();

private:
    void                    WorkerLoop();
    Result                  RunJob(const Job& job);
    void                    RunStatusJob(Result& result);
    void                    RunStillJob(const Job& job, Result& result);
    nlohmann::json          Exchange(const std::string& command, const nlohmann::json& args, double budgetSeconds);
    DWORD                   Transfer(bool writing, char* buffer, DWORD bytes, ULONGLONG deadline);
    void                    ClosePipe();
    HWND                    m_hwnd;
    UINT                    m_doneMessage;
    std::mutex              m_mutex;
    std::condition_variable m_work;
    std::deque<Job>         m_jobs;
    std::vector<Result>     m_finished;
    bool                    m_stopping = false;
    bool                    m_busy     = false;
    std::atomic<bool>       m_cancelled{false};
    // Serializes handle creation, issuing I/O and cancellation to prevent handle reuse races.
    std::mutex   m_pipeMutex;
    HANDLE       m_pipe   = INVALID_HANDLE_VALUE;
    std::int64_t m_nextId = 1;
    std::thread  m_worker;
};
} // namespace host
#endif
