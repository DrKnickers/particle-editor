// Unit tests for PreviewEncodeWorker's queue contract (src/host/PreviewEncodeWorker.h),
// specifically the stale-job drop added for the 2026-07 audit finding an-audit-finding.
//
// WHAT THE BUG WAS. The encode queue has no byte cap -- deliberately, because
// this Enqueue runs on the UI thread and AsyncFrameEncoder's block-while-full
// would freeze the window. What bounded it instead was the dispatcher's
// in-flight dedupe set: a texture already queued is never queued twice.
// PreviewCacheClear() cleared that set and left the queue alone, so the bound
// vanished at exactly the moment the queue was about to grow -- after a mod
// switch the palette re-requests everything, every key misses both the LRU and
// the dedupe gate, and the previous epoch's jobs stay queued holding up to 4 MB
// of raw BGRA apiece. The worker encoded them in full too, because only the
// RESULT carries an epoch check, at drain time.
//
// PRODUCTION ORACLE. Section E invokes the actual
// BridgeDispatcher::PreviewCacheClear member. Its small body is header-visible
// so this standalone harness does not have to link the monolithic dispatcher
// translation unit. Always-present private tags let the fixture construct a
// dispatcher and a worker without registry reads or a racing encode thread;
// no test macro changes either class layout.
//
// TIMING NOTE. The worker thread pops concurrently, so QueuedCount() is
// advisory. Every assertion below is written to hold regardless of how far the
// worker has got: the drop COUNT for a queue with nothing stale is exactly 0 no
// matter what has drained, and the batches are large enough (and each encode
// heavy enough -- a real 256x256 PNG encode) that the worker cannot plausibly
// have drained a whole batch during the microseconds it takes to enqueue one.
// Margins are wide on purpose; no assertion sits on a knife edge.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>    // IStream -- gdiplus.h needs it, and LEAN_AND_MEAN drops it
#include <gdiplus.h>

#include "../src/host/AcceleratorBridge.h"
#include "../src/host/BridgeDispatcher.h"
#include "../src/host/LayoutBroker.h"

#include <cstdio>
#include <string>
#include <vector>

namespace host {

struct PreviewQueueWiringTestAccess
{
    static std::unique_ptr<BridgeDispatcher> MakeDispatcher(
        LayoutBroker& layout, AcceleratorBridge& accel)
    {
        return std::unique_ptr<BridgeDispatcher>(
            new BridgeDispatcher(layout, accel,
                                 BridgeDispatcher::PreviewQueueTestTag{}));
    }

    static PreviewEncodeWorker& InstallIdleWorker(BridgeDispatcher& dispatcher)
    {
        dispatcher.m_previewWorker.reset(
            new PreviewEncodeWorker(NULL, 0,
                                    PreviewEncodeWorker::NoThreadForTest{}));
        return *dispatcher.m_previewWorker;
    }

    static void SetEpoch(BridgeDispatcher& dispatcher, unsigned epoch)
    {
        dispatcher.m_previewEpoch = epoch;
    }

    static unsigned Epoch(const BridgeDispatcher& dispatcher)
    {
        return dispatcher.m_previewEpoch;
    }

    static size_t Clear(BridgeDispatcher& dispatcher)
    {
        return dispatcher.PreviewCacheClear();
    }
};

} // namespace host

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

// A job carrying a real (if small) BGRA image, so the worker's encode costs
// real time rather than failing instantly -- an encode that fails fast would
// drain the batch and make the timing notes above untrue.
static host::PreviewEncodeWorker::Job makeJob(int i, unsigned epoch)
{
    const int w = 256, h = 256;
    host::PreviewEncodeWorker::Job j;
    j.key = "tex" + std::to_string(i) + "|1";
    j.filename = L"tex.tga";
    j.flattenAlpha = true;
    j.srcW = w; j.srcH = h;
    j.outW = w; j.outH = h;
    j.bgra.assign(static_cast<size_t>(w) * h * 4, static_cast<uint8_t>(i & 0xFF));
    j.epoch = epoch;
    return j;
}

static const size_t kBatch = 120;

int main()
{
    std::printf("test_preview_encode_worker\n");

    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput gdiIn;
    Gdiplus::GdiplusStartup(&token, &gdiIn, nullptr);

    // ---- A: an epoch bump drops the jobs it invalidated --------------------
    {
        host::PreviewEncodeWorker w(NULL, 0);
        for (size_t i = 0; i < kBatch; ++i) w.Enqueue(makeJob((int)i, 1u));

        const size_t dropped = w.DropStaleQueued(2u);
        // The point of the fix: queued work from the old epoch is gone. Without
        // it this is 0 and the queue still holds ~kBatch jobs of 256KB each.
        CHECK(dropped >= 1, "stale batch: DropStaleQueued removed queued work");
        CHECK(w.QueuedCount() == 0, "stale batch: nothing from the old epoch is left queued");
        w.Finish();
    }

    // ---- B: a bump that invalidates nothing drops nothing -------------------
    // The discriminating case, and fully deterministic: for a correct predicate
    // this is exactly 0 however far the worker has drained. An implementation
    // that clears the queue wholesale -- the obvious wrong fix, and the one that
    // would still pass section A -- returns non-zero here and loses previews the
    // user is waiting on.
    {
        host::PreviewEncodeWorker w(NULL, 0);
        for (size_t i = 0; i < kBatch; ++i) w.Enqueue(makeJob((int)i, 7u));

        const size_t dropped = w.DropStaleQueued(7u);
        CHECK(dropped == 0, "current batch: DropStaleQueued drops NOTHING at the same epoch");
        CHECK(w.QueuedCount() > 0, "current batch: current-epoch work survives the call");
        w.Finish();
    }

    // ---- C: a mixed queue keeps exactly the current epoch --------------------
    // Enqueued newest-epoch-last so the survivors sit at the tail, where the
    // worker has not reached them.
    {
        host::PreviewEncodeWorker w(NULL, 0);
        for (size_t i = 0; i < kBatch; ++i) w.Enqueue(makeJob((int)i, 3u));
        for (size_t i = 0; i < kBatch; ++i) w.Enqueue(makeJob((int)(kBatch + i), 4u));

        const size_t before  = w.QueuedCount();
        const size_t dropped = w.DropStaleQueued(4u);
        const size_t after   = w.QueuedCount();

        CHECK(dropped >= 1, "mixed queue: the epoch-3 half is dropped");
        // Never more than the stale half -- catches a predicate inverted or
        // widened to eat the survivors.
        CHECK(dropped <= kBatch, "mixed queue: no more than the stale half is dropped");
        CHECK(after >= kBatch - 10,
              "mixed queue: the epoch-4 half survives essentially intact");
        CHECK(before >= after + dropped - 10 && before + 10 >= after + dropped,
              "mixed queue: before ~= after + dropped (accounting holds)");
        w.Finish();
    }

    // ---- D: dropping an empty queue is a no-op, not a crash -----------------
    {
        host::PreviewEncodeWorker w(NULL, 0);
        CHECK(w.DropStaleQueued(1u) == 0, "empty queue: drop returns 0");
        CHECK(w.QueuedCount() == 0, "empty queue: still empty");
        w.Finish();
        // Idempotent after Finish (PreviewCacheClear can run during teardown).
        CHECK(w.DropStaleQueued(2u) == 0, "after Finish: drop is still safe");
    }

    // ---- E: the production cache-clear call drops only stale work ------------
    {
        host::LayoutBroker layout;
        host::AcceleratorBridge accel;
        std::unique_ptr<host::BridgeDispatcher> dispatcher =
            host::PreviewQueueWiringTestAccess::MakeDispatcher(layout, accel);
        host::PreviewEncodeWorker& worker =
            host::PreviewQueueWiringTestAccess::InstallIdleWorker(*dispatcher);

        host::PreviewQueueWiringTestAccess::SetEpoch(*dispatcher, 3u);
        worker.Enqueue(makeJob(1000, 3u));  // stale after the clear
        worker.Enqueue(makeJob(1001, 4u));  // current after the clear

        const size_t before = worker.QueuedCount();
        const size_t dropped =
            host::PreviewQueueWiringTestAccess::Clear(*dispatcher);
        const unsigned epoch =
            host::PreviewQueueWiringTestAccess::Epoch(*dispatcher);
        const size_t queued = worker.QueuedCount();

        // This second cleanup identifies the survivor, not just its count. A
        // wrong call with epoch 3 also reports dropped=1 and queued=1, but it
        // leaves the stale epoch-3 job; the correct epoch-4 cleanup exposes it.
        const size_t cleanupDropped = worker.DropStaleQueued(4u);
        const size_t cleanupQueued = worker.QueuedCount();

        std::printf(
            "  production clear observed: before=%zu epoch=%u dropped=%zu "
            "queued=%zu cleanupDropped=%zu cleanupQueued=%zu\n",
            before, epoch, dropped, queued, cleanupDropped, cleanupQueued);
        CHECK(before == 2, "production clear: fixture queues exactly two jobs");
        CHECK(epoch == 4, "production clear: epoch advances exactly from 3 to 4");
        CHECK(dropped == 1, "production clear: exactly one stale job is dropped");
        CHECK(queued == 1, "production clear: exactly one current job survives");
        CHECK(cleanupDropped == 0,
              "production clear: survivor already belongs to epoch 4");
        CHECK(cleanupQueued == 1,
              "production clear: epoch-4 survivor remains queued");
    }

    Gdiplus::GdiplusShutdown(token);

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
