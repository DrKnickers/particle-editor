// Dispatcher-owned job keeps only editor-started map editors alive until shutdown.
// Workers call EnsureStarted; pipe transport remains in EnginePreviewWorker.
#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace host
{
class EngineHostProcess
{
public:
    // Receives the host pid and ping budget in seconds; true means the host is ready.
    using Ping                              = std::function<bool(DWORD, double)>;
    static constexpr DWORD kLaunchTimeoutMs = 30000;
    static constexpr DWORD kPollIntervalMs  = 100;
    static constexpr DWORD kStopTimeoutMs   = 10000;
    static bool            IsValidExePath(const std::wstring& exePath);
    ~EngineHostProcess();
    DWORD EnsureStarted(const std::wstring& exePath, const Ping& ping, const std::atomic<bool>& cancelled);
    // Ends the map editor this editor started, if any, so setup can change or remove its folder.
    void StopStarted();

private:
    void       Launch(const std::wstring& exePath);
    std::mutex m_mutex;
    HANDLE     m_job     = nullptr;
    HANDLE     m_process = nullptr;
    DWORD      m_pid     = 0;
};
} // namespace host
