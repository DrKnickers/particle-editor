// Owns the mapping of the engine host's live frame section and one thread that keeps the
// newest complete frame. Does not touch D3D or the document; the dispatcher creates it after
// `live-start` and hands frames to the engine on the render thread.
#ifndef HOST_ENGINE_LIVE_READER_H
#define HOST_ENGINE_LIVE_READER_H

#include <windows.h>
#include "EngineLiveFrame.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace host
{
class EngineLiveReader
{
public:
    // Opens the named section and auto-reset ready event the host returned from `live-start`,
    // and watches hostPid so a host exit is seen at once. Throws engine_preview::Failure.
    EngineLiveReader(const std::wstring& sectionName, const std::wstring& eventName, std::uint32_t hostPid);
    ~EngineLiveReader();
    EngineLiveReader(const EngineLiveReader&)            = delete;
    EngineLiveReader& operator=(const EngineLiveReader&) = delete;

    // Swaps the newest frame not yet taken into pixels. False when no newer frame arrived.
    bool   TakeNewestFrame(std::vector<std::uint8_t>& pixels, engine_live::FrameInfo& info);
    double SecondsSinceLastFrame() const;
    bool   HasHostExited() const { return m_hostExited.load(); }

private:
    void              ReadLoop();
    void              ReleaseHandles();
    [[noreturn]] void FailOpen(const char* what);
    void              CopyNewestFrame();

    HANDLE                    m_section     = nullptr;
    const std::uint8_t*       m_view        = nullptr;
    HANDLE                    m_ready       = nullptr;
    HANDLE                    m_hostProcess = nullptr;
    HANDLE                    m_stop        = nullptr;
    std::vector<std::uint8_t> m_reading; // touched only by the read thread
    std::int64_t              m_readSequence = 0;
    mutable std::mutex        m_mutex;
    std::vector<std::uint8_t> m_newest;
    engine_live::FrameInfo    m_newestInfo;
    bool                      m_hasUntakenFrame = false;
    ULONGLONG                 m_lastFrameTick   = 0;
    std::atomic<bool>         m_hostExited{false};
    std::thread               m_thread;
};
} // namespace host
#endif
