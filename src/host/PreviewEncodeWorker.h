// [C3] PreviewEncodeWorker — background PNG-encode + base64 for texture
// previews (`textures/get-preview`), so the UI thread never blocks on the
// measured-heavy half of a preview build (the GDI+ PNG encode + base64 of an
// up-to-1024² image). The DEVICE-BOUND half (file/MEG read + D3DX decode +
// alpha flatten + one packed pixel copy) stays on the UI thread —
// D3DXCreateTextureFromFileInMemoryEx takes the device, and IFileManager is
// not thread-safe (the catalog builder uses its OWN FileManager for exactly
// that reason).
//
// Threading model (mirrors AsyncFrameEncoder):
//   UI thread:  Enqueue(job with raw BGRA) — never blocks. No byte cap, unlike
//               AsyncFrameEncoder: preview jobs are user-driven and sparse, and
//               this Enqueue runs on the UI thread, where AsyncFrameEncoder's
//               block-while-full would freeze the window. The queue is bounded
//               instead by the dispatcher's in-flight dedupe set plus
//               DropStaleQueued below — see that comment for why the dedupe set
//               alone was not enough (2026-07 audit, an-audit-finding).
//   worker:     EncodePackedBgraToPngBytes + Base64Encode, pushes the result
//               to the done queue, PostMessage(hwnd, doneMsg) so the UI
//               thread drains promptly even when idle-paced.
//   UI thread:  TakeFinished() under the message handler.
//   ctor pre-warms the GDI+ CLSID cache on the UI thread (its cache is not
//   first-call thread-safe); Finish() drains + joins, idempotent, called
//   before GdiplusShutdown.
//
// Epoch: jobs carry the dispatcher's preview-cache epoch; results from a
// stale epoch (mod switch mid-encode) are dropped by the consumer, so a
// cross-mod same-name texture can never populate the new mod's cache.

#ifndef HOST_PREVIEW_ENCODE_WORKER_H
#define HOST_PREVIEW_ENCODE_WORKER_H

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "GdiplusEncode.h"
#include "../UI/TexturePalette.h"

namespace host {

class PreviewEncodeWorker {
public:
    struct Job {
        std::string  key;           // dispatcher cache key (filename|flatten)
        std::wstring filename;      // for the ready event payload
        bool         flattenAlpha = true;
        int          srcW = 0, srcH = 0;
        int          outW = 0, outH = 0;
        std::vector<uint8_t> bgra;  // tightly packed outW*outH*4
        unsigned     epoch = 0;
    };
    struct Result {
        std::string  key;
        std::wstring filename;
        bool         flattenAlpha = true;
        std::string  status;        // "ok" | "broken" (encode failure)
        std::string  dataUri;
        int          srcW = 0, srcH = 0;
        unsigned     epoch = 0;
    };

    // hwnd/doneMsg: posted once per finished job so the UI thread drains
    // TakeFinished() promptly (the paced idle loop sleeps between frames).
    PreviewEncodeWorker(HWND hwnd, UINT doneMsg)
        : m_hwnd(hwnd), m_doneMsg(doneMsg)
    {
        // Pre-warm the shared GDI+ CLSID cache on THIS (UI) thread. The cache
        // is an unlocked-growth vector (GdiplusEncode.h): once every MIME the
        // process ever encodes is present, it never grows again, so concurrent
        // worker READS are safe. This worker only needs png, but a concurrent
        // FIRST jpeg call on the UI thread (a stray AlphaCompositor JPEG
        // snapshot) would push_back while the worker iterates for its png read
        // — so warm jpeg too, closing that axis outright (same fix as
        // AsyncFrameEncoder's ctor).
        CLSID warm = {};
        (void)host::GdiplusEncoderClsid(L"image/png", warm);
        (void)host::GdiplusEncoderClsid(L"image/jpeg", warm);
        m_worker = std::thread([this] { WorkerLoop(); });
    }

    PreviewEncodeWorker(const PreviewEncodeWorker&) = delete;
    PreviewEncodeWorker& operator=(const PreviewEncodeWorker&) = delete;

    ~PreviewEncodeWorker() { Finish(); }

    void Enqueue(Job j)
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_stopping) return;
            m_queue.push_back(std::move(j));
        }
        m_work.notify_one();
    }

    // Discard every QUEUED job that predates `currentEpoch`; returns how many
    // went. Returns 0 when nothing is stale -- an epoch bump with an idle queue
    // costs one lock.
    //
    // Why this is needed (2026-07 audit, an-audit-finding). In steady state the queue is
    // bounded not by a byte cap but by the dispatcher's in-flight dedupe set:
    // a texture already queued is never queued twice. PreviewCacheClear() drops
    // that set -- and did NOT drop the queue -- so the bound disappeared exactly
    // when the queue was about to grow: after a mod switch the whole palette
    // re-requests, every key misses both the LRU and the in-flight gate, and the
    // previous epoch's jobs sit behind them still holding up to 4 MB of raw BGRA
    // apiece. The worker also encoded them in full, because only the RESULT is
    // epoch-checked, at drain.
    //
    // Dropping is always safe: DrainPreviewResults discards a stale-epoch result
    // on arrival, so these jobs cannot produce anything a caller will see. Note
    // the deliberate divergence from AsyncFrameEncoder, whose byte cap BLOCKS in
    // Enqueue -- that runs on the record thread; this Enqueue runs on the UI
    // thread, where blocking would freeze the window.
    size_t DropStaleQueued(unsigned currentEpoch)
    {
        std::lock_guard<std::mutex> lk(m_mu);
        const size_t before = m_queue.size();
        for (std::deque<Job>::iterator it = m_queue.begin(); it != m_queue.end(); )
            it = (it->epoch != currentEpoch) ? m_queue.erase(it) : it + 1;
        return before - m_queue.size();
    }

    // Queue depth, for tests and for the same observability AsyncFrameEncoder
    // exposes via Stats(). Advisory: the worker pops concurrently.
    size_t QueuedCount()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        return m_queue.size();
    }

    // Drain finished results (UI thread, under the doneMsg handler).
    std::vector<Result> TakeFinished()
    {
        std::lock_guard<std::mutex> lk(m_mu);
        std::vector<Result> out(std::make_move_iterator(m_done.begin()),
                                std::make_move_iterator(m_done.end()));
        m_done.clear();
        return out;
    }

    // Stop + join. Idempotent; must run before GdiplusShutdown (the worker
    // encodes via GDI+). Pending jobs are dropped — previews are refetchable.
    void Finish()
    {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            if (m_finished) return;
            m_stopping = true;
        }
        m_work.notify_one();
        if (m_worker.joinable()) m_worker.join();
        std::lock_guard<std::mutex> lk(m_mu);
        m_finished = true;
    }

private:
    void WorkerLoop()
    {
        for (;;)
        {
            Job j;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_work.wait(lk, [&] { return m_stopping || !m_queue.empty(); });
                if (m_stopping) return;   // pending jobs dropped by design
                j = std::move(m_queue.front());
                m_queue.pop_front();
            }

            Result r;
            r.key = std::move(j.key);
            r.filename = std::move(j.filename);
            r.flattenAlpha = j.flattenAlpha;
            r.srcW = j.srcW; r.srcH = j.srcH;
            r.epoch = j.epoch;
            std::vector<uint8_t> png;
            if (TexturePalette::EncodePackedBgraToPngBytes(
                    j.bgra.data(), j.outW, j.outH, png))
            {
                r.status  = "ok";
                r.dataUri = "data:image/png;base64,"
                          + host::Base64Encode(png.data(), png.size());
            }
            else
            {
                r.status = "broken";
            }

            {
                std::lock_guard<std::mutex> lk(m_mu);
                m_done.push_back(std::move(r));
            }
            if (m_hwnd) PostMessage(m_hwnd, m_doneMsg, 0, 0);
        }
    }

    HWND m_hwnd;
    UINT m_doneMsg;
    std::mutex              m_mu;
    std::condition_variable m_work;
    std::deque<Job>         m_queue;
    std::deque<Result>      m_done;
    bool m_stopping = false;
    bool m_finished = false;
    std::thread m_worker;
};

}  // namespace host

#endif  // HOST_PREVIEW_ENCODE_WORKER_H
