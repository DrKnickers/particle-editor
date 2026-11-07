// Owns background setup/refresh/removal and queued notifications; the dispatcher persists settings
// and emits bridge events on the UI thread. No document or WebView access occurs here.
#pragma once

#include <windows.h>
#include "host/third_party/nlohmann/json.hpp"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace host
{
class EngineHostSetup
{
public:
    enum class Mode
    {
        Setup,
        Refresh,
        Remove
    };
    struct Job
    {
        Mode                      mode = Mode::Setup;
        std::wstring              editorExePath;
        std::wstring              packageManifestPath;
        std::vector<std::wstring> basepaths;
        std::vector<std::wstring> contentRoots;
    };
    struct Event
    {
        bool           finished = false;
        nlohmann::json payload;
    };
    struct RefreshStatus
    {
        bool        isHostOutOfDate = false;
        std::string bundledHostVersion;
    };

    explicit EngineHostSetup(HWND hwnd);
    ~EngineHostSetup()
    {
        RequestCancel();
        Finish();
    }
    bool Start(Job job, std::string& error);
    void Finish()
    {
        if (m_worker.joinable()) m_worker.join();
    }
    bool                IsBusy() const;
    std::optional<Mode> ActiveMode() const;
    // Only Setup copies long enough to be worth cancelling; Refresh and Remove take about a second.
    // Inline so the header-only destructor links in tests that do not compile EngineHostSetup.cpp.
    void RequestCancel()
    {
        if (m_busy && m_mode == Mode::Setup) m_cancelRequested = true;
    }
    std::vector<Event>   TakeEvents();
    static std::wstring  ManagedPath();
    static bool          IsRunning(const std::wstring& target);
    static bool          IsSetupComplete(const std::wstring& target);
    static RefreshStatus CheckRefreshStatus(const std::wstring& target, const std::wstring& packageManifestPath);

private:
    void Run(const Job& job);
    void ReportProgress(const std::string& stage, size_t done, size_t total);
    void Publish(Event event);

    HWND              m_hwnd;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_cancelRequested{false};
    // Start, status and cancel are dispatched on the UI thread; the worker reads its captured Job.
    Mode               m_mode = Mode::Setup;
    std::thread        m_worker;
    std::mutex         m_mutex;
    std::vector<Event> m_events;
    ULONGLONG          m_lastProgressMs = 0;
    std::string        m_lastStage;
};
} // namespace host
