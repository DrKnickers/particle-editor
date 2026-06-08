// FramePublisher — Phase 1 production transport.
//
// Owns the per-frame "encode → base64 → emit" pipeline for the
// canvas-in-DOM viewport (architecture C). Constructed when the host
// runs in architecture-C mode (default under, opt-out via
// ALO_HOSTING_MODE=legacy); the legacy WS_EX_LAYERED popup path
// bypasses this class entirely (HostWindow holds the pointer as a
// nullable unique_ptr).
//
// Lifecycle: constructed alongside the AlphaCompositor (which it does
// not own), destroyed before the compositor in WM_DESTROY. Calls
// AlphaCompositor::EncodeFrameJpeg() to read the pre-stamp DIB cache
// — that cache is updated by AlphaCompositor::Composite() each frame,
// so OnFrameComposited() should be invoked AFTER engine->Render()
// returns (Composite runs inside engine->Render).
//
// All operations are UI-thread only. No synchronization required on
// the JPEG buffer or frame counter; both are written + read on the
// same thread.
//
// See Phase 0 spike + (tasks/lessons.md) for the
// design rationale behind inline-base64 instead of
// WebResourceRequested.

#ifndef HOST_FRAME_PUBLISHER_H
#define HOST_FRAME_PUBLISHER_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace host {

class AlphaCompositor;

class FramePublisher
{
public:
    // Emit callback: receives a JSON string ready to hand to
    // ICoreWebView2::PostWebMessageAsJson. HostWindow supplies a
    // lambda that does the UTF-16 conversion + post.
    using EmitFn = std::function<void(const std::string& json)>;

    // Optional logger callback: receives a fully-formatted log line
    // (no newline). HostWindow supplies a lambda that fans through
    // its existing Log() facility. Null logger = silent.
    using LogFn = std::function<void(const std::string& line)>;

    // The compositor must outlive the publisher. `quality` is JPEG
    // quality 1..100 (clamped inside the encoder). Both callbacks are
    // copied — keep them cheap or hold the captured state by pointer.
    FramePublisher(AlphaCompositor* compositor, EmitFn emit, int quality);

    ~FramePublisher() = default;
    FramePublisher(const FramePublisher&)            = delete;
    FramePublisher& operator=(const FramePublisher&) = delete;

    void SetLogger(LogFn log) { m_log = std::move(log); }

    // Encode the latest composited frame as JPEG, base64-encode, and
    // emit a viewport/frame-ready event. Throttled diagnostic log line
    // fires once per second when a logger is attached. Safe no-op when
    // the compositor has no frame yet (returns false). Returns true if
    // a frame was actually emitted.
    bool OnFrameComposited();

    uint64_t FrameId() const { return m_frameId; }

private:
    AlphaCompositor* m_compositor;
    EmitFn   m_emit;
    LogFn    m_log;
    int      m_quality;
    std::vector<uint8_t> m_jpegBuf;
    int      m_lastW       = 0;
    int      m_lastH       = 0;
    uint64_t m_frameId     = 0;
    DWORD    m_lastLogTick = 0;
};

} // namespace host

#endif // HOST_FRAME_PUBLISHER_H
