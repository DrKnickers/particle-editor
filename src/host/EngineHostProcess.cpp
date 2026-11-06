// Validates the read-only host ini and starts the map editor in a lifetime job.
// The worker supplies bounded pipe pings; this file never touches the UI or renderer.
#include "EngineHostProcess.h"
#include "EngineHostLaunchPolicy.h"
#include "EnginePreviewPipe.h"
#include <algorithm>
#include <filesystem>
#include <vector>

namespace host
{
namespace
{
constexpr DWORD  kInitialIniChars       = 256;
constexpr size_t kIniBufferGrowthFactor = 2;
constexpr double kMillisecondsPerSecond = 1000.0;
constexpr double kPingBudgetSeconds     = 0.25;

std::wstring ReadIniValue(const std::wstring& path, const wchar_t* section, const wchar_t* key)
{
    std::vector<wchar_t> value(kInitialIniChars);
    for (;;)
    {
        const DWORD count =
            GetPrivateProfileStringW(section, key, L"", value.data(), static_cast<DWORD>(value.size()), path.c_str());
        if (count < value.size() - 1) return std::wstring(value.data(), count);
        value.resize(value.size() * kIniBufferGrowthFactor);
    }
}

void FailLaunch(const std::string& action)
{
    const DWORD error = GetLastError();
    throw engine_preview::Failure("launch", action + " (Windows error " + std::to_string(error) + ").");
}
} // namespace

EngineHostProcess::~EngineHostProcess()
{
    if (m_process) CloseHandle(m_process);
    if (m_job) CloseHandle(m_job);
}

bool EngineHostProcess::IsValidExePath(const std::wstring& exePath)
{
    if (exePath.find(L'\0') != std::wstring::npos) return false;
    const std::filesystem::path exe(exePath);
    std::error_code             error;
    return exe.is_absolute() && engine_preview::FoldPath(exe.extension().string()) == ".exe" &&
           std::filesystem::is_regular_file(exe, error);
}

void EngineHostProcess::Launch(const std::wstring& exePath)
{
    if (exePath.empty()) throw engine_preview::Failure("launch", engine_host::kPathNotSetMessage);
    const std::filesystem::path exe(exePath);
    if (!IsValidExePath(exePath))
        throw engine_preview::Failure("launch", "The configured map editor must be an existing absolute .exe path.");
    const std::wstring           iniPath = (exe.parent_path() / L"alamo_preview.ini").wstring();
    const engine_host::IniValues ini{
        ReadIniValue(iniPath, L"Window", L"Hidden"), ReadIniValue(iniPath, L"Preview", L"EffectPath"),
        ReadIniValue(iniPath, L"Preview", L"MapA"), ReadIniValue(iniPath, L"Preview", L"MapB"),
        ReadIniValue(iniPath, L"Preview", L"SceneDir")};
    const std::string refusal = engine_host::FindMissingLaunchPrerequisites(true, ini);
    if (!refusal.empty()) throw engine_preview::Failure("launch", refusal);
    if (!m_job)
    {
        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) FailLaunch("Cannot create the map editor lifetime job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags     = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        {
            const DWORD error = GetLastError();
            CloseHandle(m_job);
            m_job = nullptr;
            SetLastError(error);
            FailLaunch("Cannot configure the map editor lifetime job");
        }
    }
    STARTUPINFOW startup        = {};
    startup.cb                  = sizeof(startup);
    PROCESS_INFORMATION child   = {};
    std::wstring        command = L"\"" + exePath + L"\"";
    const std::wstring  folder  = exe.parent_path().wstring();
    // Suspend until assignment so even a fast startup cannot escape editor ownership.
    if (!CreateProcessW(exePath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        folder.c_str(), &startup, &child))
        FailLaunch("Cannot start the map editor");
    const bool assigned = AssignProcessToJobObject(m_job, child.hProcess) != FALSE;
    if (!assigned || ResumeThread(child.hThread) == static_cast<DWORD>(-1))
    {
        const DWORD error = GetLastError();
        if (!TerminateProcess(child.hProcess, error))
            OutputDebugStringA("[engine-host] ERROR terminating the suspended map editor\n");
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
        SetLastError(error);
        FailLaunch("Cannot attach or resume the map editor lifetime job");
    }
    CloseHandle(child.hThread);
    if (m_process) CloseHandle(m_process);
    m_process = child.hProcess;
    m_pid     = child.dwProcessId;
}

void EngineHostProcess::StopStarted()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_process) return;
    // Ending it loses nothing: its maps, textures and effect are host-managed copies.
    if (WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT)
    {
        if (!TerminateProcess(m_process, 0)) FailLaunch("Cannot stop the map editor");
        if (WaitForSingleObject(m_process, kStopTimeoutMs) != WAIT_OBJECT_0) FailLaunch("The map editor did not stop");
    }
    CloseHandle(m_process);
    m_process = nullptr;
    m_pid     = 0;
}

DWORD EngineHostProcess::EnsureStarted(const std::wstring&      exePath,
                                       const Ping&              ping,
                                       const std::atomic<bool>& cancelled)
{
    std::unique_lock<std::mutex> lock(m_mutex, std::defer_lock);
    while (!lock.try_lock())
    {
        if (cancelled) throw engine_preview::Failure("cancelled", "Map editor startup cancelled.");
        Sleep(kPollIntervalMs);
    }
    if (cancelled) throw engine_preview::Failure("cancelled", "Map editor startup cancelled.");
    const DWORD initialState = m_process ? WaitForSingleObject(m_process, 0) : WAIT_OBJECT_0;
    if (initialState == WAIT_FAILED) FailLaunch("Cannot query the map editor process");
    if (initialState == WAIT_OBJECT_0) Launch(exePath);
    const ULONGLONG deadline = GetTickCount64() + kLaunchTimeoutMs;
    for (;;)
    {
        if (cancelled) throw engine_preview::Failure("cancelled", "Map editor startup cancelled.");
        const DWORD processState = WaitForSingleObject(m_process, 0);
        if (processState == WAIT_FAILED) FailLaunch("Cannot wait for the map editor process");
        if (processState == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(m_process, &exitCode)) FailLaunch("Cannot query the map editor exit code");
            throw engine_preview::Failure("launch", "The map editor exited before answering (exit code " +
                                                        std::to_string(exitCode) + ").");
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            throw engine_preview::Failure(
                "timeout", "The map editor did not answer a pipe ping within " +
                               std::to_string(kLaunchTimeoutMs / static_cast<DWORD>(kMillisecondsPerSecond)) +
                               " seconds. Check its host installation.");
        const double remainingSeconds = static_cast<double>(deadline - now) / kMillisecondsPerSecond;
        if (ping(m_pid, (std::min)(kPingBudgetSeconds, remainingSeconds))) return m_pid;
        const ULONGLONG afterPing = GetTickCount64();
        if (afterPing < deadline)
            Sleep(static_cast<DWORD>((std::min)(static_cast<ULONGLONG>(kPollIntervalMs), deadline - afterPing)));
    }
}
} // namespace host
