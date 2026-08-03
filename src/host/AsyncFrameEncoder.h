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
//  - [R3b] The worker encodes via WIC (WicEncode.h), NOT GDI+ — the old
//    GDI+ CLSID pre-warm is gone with its consumer (the worker no longer
//    touches GdiplusEncode.h's cache, so that concurrency axis is closed
//    by construction). The worker initializes MTA COM for WIC itself.
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

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "WicEncode.h"     // [R3b] WIC PNG core for the worker (GDI+ stays for one-shots)

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
        // [R3b] No GDI+ CLSID pre-warm anymore: the worker encodes via WIC
        // and never reads GdiplusEncode.h's cache (see the header comment).
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
        // [R3] Time the cap-block: when the queue is full this wait inherits
        // the worker's PNG-encode variance INTO frame time, invisibly — the
        // record-timing summary reads these so back-pressure is a first-class
        // number, not a mystery bump in the png/capture buckets.
        if (m_bytes + bytes > m_maxBytes && !m_queue.empty())
        {
            const auto w0 = std::chrono::steady_clock::now();
            m_space.wait(lk, [&] {
                return m_failed || m_stopping || m_bytes + bytes <= m_maxBytes || m_queue.empty();
            });
            m_queueWaitMsTotal += std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - w0).count();
            ++m_queueWaitCount;
        }
        // Enqueue after Finish() began would silently drop the frame — report
        // it as a failure rather than lie about having accepted it.
        if (m_failed || m_stopping) return false;
        m_bytes += bytes;
        m_queue.push_back(std::move(f));
        if (m_bytes > m_bytesHighWater) m_bytesHighWater = m_bytes;
        if (m_queue.size() > m_depthHighWater) m_depthHighWater = m_queue.size();
        m_work.notify_one();
        return true;
    }

    // [R3] Back-pressure telemetry for the record-timing summary: total ms
    // spent blocked on the queue cap + how often, and queue high-water marks.
    struct QueueStats { double waitMsTotal; unsigned waitCount;
                        size_t bytesHighWater; size_t depthHighWater; };
    QueueStats GetQueueStats() const
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return { m_queueWaitMsTotal, m_queueWaitCount,
                 m_bytesHighWater, m_depthHighWater };
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
        // [R3b] WIC needs COM on this thread (the worker is a bare
        // std::thread — nothing initialized COM here before). MTA: the
        // factory is created + used only on this thread. A failed init is
        // not fatal here — the WIC encode below will fail loudly and latch
        // m_failed, same as any encode failure.
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct ComScope
        {
            HRESULT hr;
            ~ComScope() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
        } comScope{coHr};

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
            // [R3b] WIC core (FilterOption=None) replacing the GDI+ encode —
            // measured ≈49 ms/frame at 3200×1460 on GDI+; see WicEncode.h.
            if (!host::EncodeBgraToPngWic(f.bgra.data(), f.w, f.h, f.path))
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
    // [R3] back-pressure telemetry (guarded by m_mu; see GetQueueStats).
    double                  m_queueWaitMsTotal = 0.0;
    unsigned                m_queueWaitCount   = 0;
    size_t                  m_bytesHighWater   = 0;
    size_t                  m_depthHighWater   = 0;
    std::thread             m_worker;
};

}  // namespace host
