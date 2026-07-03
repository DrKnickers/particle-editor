#pragma once
// AsyncFrameEncoder — Branch B of the record speedup (tasks/todo.md §3).
//
// Moves the record loop's per-frame PNG compress+write off the record thread:
// measured at 56-62 ms/frame = 54-55% of all tick time (the [record-timing]
// Phase-0 numbers), it was the single largest serial cost. The GRAB stays on
// the record thread (WindowCapture.h GrabWindowPixels) so pixel content is
// identical to the old inline path by construction; only zlib/PNG and the
// disk write run here, on ONE background worker.
//
// Review-driven requirements (tasks/todo.md §3 Branch B + risks 3/4):
//  - CLSID pre-warm: the ctor resolves the "image/png" encoder CLSID on the
//    CALLING (UI) thread — GdiplusEncode.h's cache is not safe for a
//    concurrent first call from two threads.
//  - Byte-capped queue (default 128 MB), computed from actual frame sizes
//    (f04 grabs ~18 MB/frame at 3200x1460); Enqueue BLOCKS while full, so the
//    worst case degrades to the old serial behavior — never unbounded memory.
//  - Fail-loud, fail-early: a worker encode failure latches {failed, path};
//    the next Enqueue returns false (=> ClipRunner exit 4 on that frame) and
//    Finish() returns false so the .tmp -> out publish is skipped.
//  - RAII join: Finish() (idempotent) drains + joins; the dtor calls it too,
//    but callers MUST Finish() explicitly before Gdiplus::GdiplusShutdown —
//    after a successful Finish() the dtor does no GDI+ work, so a late
//    destruction (e.g. via a hook closure that outlives the pump) is safe.

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GdiplusEncode.h"
#include "WindowCapture.h"

namespace host {

class AsyncFrameEncoder
{
public:
    struct Frame
    {
        std::vector<unsigned char> bgra;
        int w = 0, h = 0;
        std::wstring path;
    };

    using LogFn = std::function<void(const std::string&)>;

    explicit AsyncFrameEncoder(size_t maxQueuedBytes = 128ull * 1024 * 1024,
                               LogFn log = nullptr)
        : m_maxBytes(maxQueuedBytes), m_log(std::move(log))
    {
        // Pre-warm the shared CLSID cache on this (UI) thread; the worker's
        // lookups then always hit the cache read path. JPEG too: any first-call
        // for a different MIME on the UI thread mid-record (e.g. a stray modal
        // screenshot via AlphaCompositor) would push_back into the vector the
        // worker iterates — warming both closes that axis outright.
        CLSID warm = {};
        (void)host::GdiplusEncoderClsid(L"image/png", warm);
        (void)host::GdiplusEncoderClsid(L"image/jpeg", warm);
        m_worker = std::thread([this] { WorkerLoop(); });
    }

    AsyncFrameEncoder(const AsyncFrameEncoder&) = delete;
    AsyncFrameEncoder& operator=(const AsyncFrameEncoder&) = delete;

    ~AsyncFrameEncoder() { Finish(); }

    // Queue one frame for encoding. Blocks while the queue holds maxQueuedBytes
    // (bounded stall — at most one encode's duration per retry). Returns false
    // if a previous frame's encode already failed (the caller should abort the
    // run with exit 4; the failing path is in FailedPath()).
    bool Enqueue(Frame f)
    {
        std::unique_lock<std::mutex> lk(m_mu);
        if (m_failed) return false;
        const size_t bytes = f.bgra.size();
        m_space.wait(lk, [&] {
            return m_failed || m_stopping || m_bytes + bytes <= m_maxBytes || m_queue.empty();
        });
        // Enqueue after Finish() began would silently drop the frame — report
        // it as a failure rather than lie about having accepted it.
        if (m_failed || m_stopping) return false;
        m_bytes += bytes;
        m_queue.push_back(std::move(f));
        m_work.notify_one();
        return true;
    }

    // Drain the queue, stop and join the worker. Idempotent. Returns true if
    // every queued frame encoded cleanly (callers gate the .tmp -> out rename
    // on this). Safe to call from the UI thread only.
    bool Finish()
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_finished) return !m_failed;
            m_stopping = true;
            m_work.notify_one();
            m_space.notify_all();
        }
        if (m_worker.joinable()) m_worker.join();
        std::lock_guard<std::mutex> lk(m_mu);
        m_finished = true;
        return !m_failed;
    }

    std::wstring FailedPath() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_failedPath;
    }

private:
    void WorkerLoop()
    {
        for (;;)
        {
            Frame f;
            bool alreadyFailed = false;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_work.wait(lk, [&] { return m_stopping || !m_queue.empty(); });
                if (m_queue.empty()) return;   // stopping + drained
                f = std::move(m_queue.front());
                m_queue.pop_front();
                m_bytes -= f.bgra.size();
                alreadyFailed = m_failed;
                m_space.notify_all();
            }
            if (alreadyFailed) continue;   // failed runs discard, not encode
            if (!host::EncodeBgraToPng(f.bgra.data(), f.w, f.h, f.path))
            {
                std::lock_guard<std::mutex> lk(m_mu);
                if (!m_failed)
                {
                    m_failed = true;
                    m_failedPath = f.path;
                    if (m_log) m_log("async-encode failed: frame write did not complete");
                }
                m_space.notify_all();
                // Keep draining (cheaply discarding) so Enqueue never deadlocks.
            }
        }
    }

    mutable std::mutex      m_mu;
    std::condition_variable m_work, m_space;
    std::deque<Frame>       m_queue;
    size_t                  m_bytes = 0;
    const size_t            m_maxBytes;
    bool                    m_stopping = false;
    bool                    m_failed = false;
    bool                    m_finished = false;
    std::wstring            m_failedPath;
    LogFn                   m_log;
    std::thread             m_worker;
};

}  // namespace host
