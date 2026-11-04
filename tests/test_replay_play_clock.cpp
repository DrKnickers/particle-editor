// The interactive timeline's play clock (src/simulation/ReplayPlayClock.h).
//
//   1. PACING: at 60 frames per second of wall time, a slot advances to the
//      frame that is due, and never past it.
//   2. CATCH-UP: a stall advances at most kReplayMaxCatchUpFrames and asks for a
//      re-anchor, so playback resumes at normal speed instead of bursting.
//   3. END: without loop, the step stops at maxFrames and reports ended; with
//      loop, a slot at maxFrames wraps to frame 0 and re-anchors.
//   4. GUARDS: no frequency, or a clock that has not moved, advances nothing;
//      seeks clamp to [0, maxFrames].
//   5. PRODUCTION BINDING: the timeline session calls the header.
//
// See the test_replay_play_clock entry in tests/native-tests.json.

#include "simulation/ReplayPlayClock.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static int g_failed = 0;
#define CHECK(cond, msg)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (cond)                                                                                                      \
        {                                                                                                              \
            std::printf("  ok: %s\n", msg);                                                                            \
        }                                                                                                              \
        else                                                                                                           \
        {                                                                                                              \
            ++g_failed;                                                                                                \
            std::printf("  FAIL: %s\n", msg);                                                                          \
        }                                                                                                              \
    } while (0)

// Plays `slots` idle slots `slotTicks` apart from frame `start`, the way the
// host does: advance to the step's target, re-anchor when asked.
struct Played
{
    int  frame;
    int  maxStep;
    bool ended;
    int  wraps;
};
static Played Play(int          start,
                   int          slots,
                   std::int64_t slotTicks,
                   std::int64_t frequency,
                   int          maxFrames,
                   bool         loop,
                   std::int64_t stallAt    = -1,
                   std::int64_t stallTicks = 0)
{
    ReplayPlayAnchor anchor{start, 0};
    std::int64_t     now = 0;
    Played           played{start, 0, false, 0};
    for (int i = 0; i < slots && !played.ended; ++i)
    {
        now += slotTicks;
        if (i == stallAt) now += stallTicks;
        const ReplayPlayStep step = NextReplayPlayStep(anchor, played.frame, now, frequency, maxFrames, loop);
        if (step.wrap) ++played.wraps;
        if (!step.wrap && step.target - played.frame > played.maxStep) played.maxStep = step.target - played.frame;
        played.frame = step.target;
        played.ended = step.ended;
        if (step.reanchor) anchor = ReplayPlayAnchor{step.target, now};
    }
    return played;
}

int main()
{
    std::printf("test_replay_play_clock\n");

    // 1. Pacing. A 1000 Hz clock makes the arithmetic readable.
    const ReplayPlayAnchor zero{0, 0};
    CHECK(NextReplayPlayStep(zero, 0, 16, 1000, 600, false).target == 0, "16 ms in, frame 1 is not yet due");
    CHECK(NextReplayPlayStep(zero, 0, 17, 1000, 600, false).target == 1, "17 ms in, frame 1 is due");
    CHECK(NextReplayPlayStep(zero, 1, 17, 1000, 600, false).target == 1, "a due frame already shown is not repeated");
    CHECK(NextReplayPlayStep(ReplayPlayAnchor{100, 5000}, 100, 5050, 1000, 600, false).target == 103,
          "the anchor's frame and ticks offset the due frame");
    const Played second = Play(0, 60, 16'667, 1'000'000, 600, false);
    CHECK(second.frame == 60 && second.maxStep == 1, "60 slots of 1/60 s play 60 frames, one per slot");
    const Played slowDisplay = Play(0, 30, 33'334, 1'000'000, 600, false);
    CHECK(slowDisplay.frame == 60 && slowDisplay.maxStep == 2, "a 30 Hz display still plays 60 frames a second");

    // 2. Catch-up is capped and re-anchors.
    const ReplayPlayStep burst = NextReplayPlayStep(zero, 0, 1000, 1000, 600, false);
    CHECK(burst.target == kReplayMaxCatchUpFrames && burst.reanchor,
          "a 1 s stall advances only the cap and re-anchors");
    const Played stalled = Play(0, 60, 16'667, 1'000'000, 600, false, 10, 2'000'000);
    CHECK(stalled.maxStep == kReplayMaxCatchUpFrames, "a 2 s stall mid-play advances at most the cap in one slot");
    CHECK(stalled.frame >= 60 && stalled.frame <= 60 + kReplayMaxCatchUpFrames,
          "after the stall, play resumes at normal speed (the stall's frames are dropped)");

    // 3. End of the review window.
    const ReplayPlayStep last = NextReplayPlayStep(ReplayPlayAnchor{598, 0}, 598, 1000, 1000, 600, false);
    CHECK(last.target == 600 && last.ended, "without loop, play stops on maxFrames and reports ended");
    const ReplayPlayStep lastLoop = NextReplayPlayStep(ReplayPlayAnchor{598, 0}, 598, 1000, 1000, 600, true);
    CHECK(lastLoop.target == 600 && !lastLoop.ended && !lastLoop.wrap, "with loop, maxFrames is still shown");
    const ReplayPlayStep atEnd = NextReplayPlayStep(zero, 600, 5, 1000, 600, false);
    CHECK(atEnd.target == 600 && atEnd.ended && !atEnd.wrap, "at maxFrames without loop, stay ended");
    const ReplayPlayStep wrap = NextReplayPlayStep(zero, 600, 5, 1000, 600, true);
    CHECK(wrap.target == 0 && wrap.wrap && wrap.reanchor && !wrap.ended,
          "at maxFrames with loop, wrap to 0 and re-anchor");
    const Played looping = Play(0, 300, 16'667, 1'000'000, 120, true);
    CHECK(looping.wraps == 2 && !looping.ended, "5 s of a looping 2 s window wraps twice and never ends");
    const Played once = Play(0, 300, 16'667, 1'000'000, 120, false);
    CHECK(once.frame == 120 && once.ended, "without loop the same window ends on its last frame");

    // 4. Guards and clamping.
    CHECK(NextReplayPlayStep(zero, 5, 1000, 0, 600, false).target == 5, "no frequency advances nothing");
    CHECK(NextReplayPlayStep(ReplayPlayAnchor{5, 900}, 5, 800, 1000, 600, false).target == 5,
          "a clock behind the anchor advances nothing");
    CHECK(ClampReplayFrame(-1, 600) == 0 && ClampReplayFrame(601, 600) == 600 && ClampReplayFrame(32, 600) == 32,
          "seeks clamp to the review window");
    CHECK(ClampReplayFrame(1LL << 40, 600) == 600 && ClampReplayFrame(-(1LL << 40), 600) == 0,
          "out-of-range requests clamp without overflow");

    // 5. Production binding.
    std::ifstream source(std::filesystem::current_path() / "src/host/BridgeDispatch_Timeline.cpp", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(source)), std::istreambuf_iterator<char>());
    CHECK(text.find("#include \"simulation/ReplayPlayClock.h\"") != std::string::npos &&
              text.find("NextReplayPlayStep(") != std::string::npos &&
              text.find("ClampReplayFrame(") != std::string::npos,
          "the timeline session paces play and clamps seeks through the header");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_failed ? 1 : 0;
}
