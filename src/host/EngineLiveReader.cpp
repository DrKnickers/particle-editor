// Live engine view reader: maps the host's frame section, waits on its ready event and keeps
// the newest complete frame for the render thread. Layout rules live in EngineLiveFrame.h.
#include "EngineLiveReader.h"
#include "EnginePreviewPipe.h"

namespace host
{
namespace
{
// The read thread also wakes this often without an event, so a missed signal or a host that
// stopped presenting never leaves it blocked.
constexpr DWORD  kReadPollMs            = 100;
constexpr double kMillisecondsPerSecond = 1000.0;

[[noreturn]] void ThrowOpenFailure(const char* what, DWORD error)
{
    throw engine_preview::Failure("internal", std::string("Cannot open the live engine view ") + what + " (error " +
                                                  std::to_string(error) + ").");
}
} // namespace

EngineLiveReader::EngineLiveReader(const std::wstring& sectionName,
                                   const std::wstring& eventName,
                                   std::uint32_t       hostPid)
{
    m_section = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName.c_str());
    if (!m_section) ThrowOpenFailure("frame section", GetLastError());
    m_view = static_cast<const std::uint8_t*>(MapViewOfFile(m_section, FILE_MAP_READ, 0, 0, 0));
    MEMORY_BASIC_INFORMATION region = {};
    const bool               isMapped =
        m_view && VirtualQuery(m_view, &region, sizeof region) && engine_live::IsValidHeader(m_view, region.RegionSize);
    if (!isMapped)
    {
        ReleaseHandles();
        throw engine_preview::Failure("protocol", "The engine host's live frame section has an unknown layout.");
    }
    m_ready = OpenEventW(SYNCHRONIZE, FALSE, eventName.c_str());
    if (!m_ready) FailOpen("ready event");
    m_hostProcess = OpenProcess(SYNCHRONIZE, FALSE, hostPid);
    if (!m_hostProcess) FailOpen("host process handle");
    m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop) FailOpen("stop event");
    m_lastFrameTick = GetTickCount64();
    m_thread        = std::thread([this]() { ReadLoop(); });
}

void EngineLiveReader::FailOpen(const char* what)
{
    const DWORD error = GetLastError(); // read before ReleaseHandles' calls overwrite it
    ReleaseHandles();
    ThrowOpenFailure(what, error);
}

EngineLiveReader::~EngineLiveReader()
{
    if (m_thread.joinable())
    {
        SetEvent(m_stop);
        m_thread.join();
    }
    ReleaseHandles();
}

void EngineLiveReader::ReleaseHandles()
{
    if (m_view) UnmapViewOfFile(m_view);
    for (HANDLE handle : {m_section, m_ready, m_hostProcess, m_stop})
        if (handle) CloseHandle(handle);
    m_view    = nullptr;
    m_section = m_ready = m_hostProcess = m_stop = nullptr;
}

bool EngineLiveReader::TakeNewestFrame(std::vector<std::uint8_t>& pixels, engine_live::FrameInfo& info)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasUntakenFrame) return false;
    pixels.swap(m_newest);
    info              = m_newestInfo;
    m_hasUntakenFrame = false;
    return true;
}

double EngineLiveReader::SecondsSinceLastFrame() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (GetTickCount64() - m_lastFrameTick) / kMillisecondsPerSecond;
}

void EngineLiveReader::CopyNewestFrame()
{
    engine_live::FrameInfo info;
    // Torn and invalid reads are dropped: the next ready event brings a whole frame, and a
    // host that only ever writes invalid frames is caught by the stale-frame timeout.
    const engine_live::ReadOutcome outcome =
        engine_live::ReadNewestFrame(m_view, engine_live::kSectionBytes, m_readSequence, m_reading, info);
    if (outcome != engine_live::ReadOutcome::Copied) return;
    m_readSequence = info.sequence;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_newest.swap(m_reading);
    m_newestInfo      = info;
    m_hasUntakenFrame = true;
    m_lastFrameTick   = GetTickCount64();
}

void EngineLiveReader::ReadLoop()
{
    const HANDLE waits[] = {m_stop, m_hostProcess, m_ready};
    for (;;)
    {
        const DWORD woke = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, kReadPollMs);
        if (woke == WAIT_OBJECT_0) return;
        if (woke == WAIT_FAILED)
        {
            // Stopping here, not spinning: the dispatcher's stale-frame timeout then falls back.
            OutputDebugStringA(
                ("[live-view] ERROR frame wait failed, error " + std::to_string(GetLastError()) + "\n").c_str());
            return;
        }
        if (woke == WAIT_OBJECT_0 + 1)
        {
            m_hostExited = true;
            return;
        }
        CopyNewestFrame();
    }
}
} // namespace host
