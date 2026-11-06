#pragma once

// Interactive timeline play clock.
//
// While the timeline plays, the host's paced idle slot asks which replay frame
// is due. Replay frames are a fixed 1/60 s apart, so this only paces playback:
// every frame the replay reaches is the frame a seek to it would produce, and
// the readout takes its time from the frame number, never from the wall clock.
//
// Catch-up is capped. After a stall (a modal dialog's own message pump, a
// minimized window) the slot advances at most kReplayMaxCatchUpFrames and
// re-anchors, dropping the missed wall time instead of replaying a burst.
//
// Wall time arrives as ticks plus a frequency (QueryPerformanceCounter), so the
// unit test drives it without a clock. Integer arithmetic only, no engine
// includes. tests/test_replay_play_clock.cpp pins it.

#include <cstdint>

static const int kReplayFramesPerSecond  = 60;
static const int kReplayMaxCatchUpFrames = 4;

// Where play last (re)started: the frame shown then and the wall clock then.
struct ReplayPlayAnchor
{
    int          frame = 0;
    std::int64_t ticks = 0;
};

struct ReplayPlayStep
{
    int  target   = 0;     // advance the replay to this frame in this slot
    bool wrap     = false; // loop at the end: seek to frame 0 instead
    bool ended    = false; // no loop: target is the last frame; pause there
    bool reanchor = false; // set the anchor to {target, now}
};

// A requested frame clamped to the review window [0, maxFrames].
inline int ClampReplayFrame(long long frame, int maxFrames)
{
    if (frame < 0) return 0;
    if (frame > maxFrames) return maxFrames;
    return static_cast<int>(frame);
}

// The step for one idle slot while playing. `frame` is the replay's current
// frame; `maxFrames` is the end of the review window.
inline ReplayPlayStep NextReplayPlayStep(
    const ReplayPlayAnchor& anchor, int frame, std::int64_t now, std::int64_t frequency, int maxFrames, bool loop)
{
    ReplayPlayStep step;
    step.target = frame;
    if (frame >= maxFrames)
    {
        // The last frame has been shown for a slot: loop back or stay ended.
        step.target   = loop ? 0 : maxFrames;
        step.wrap     = loop;
        step.ended    = !loop;
        step.reanchor = loop;
        return step;
    }
    if (frequency <= 0 || now <= anchor.ticks) return step;

    const std::int64_t due = anchor.frame + (now - anchor.ticks) * kReplayFramesPerSecond / frequency;
    if (due <= frame) return step;

    std::int64_t target = due;
    if (target - frame > kReplayMaxCatchUpFrames)
    {
        target        = frame + kReplayMaxCatchUpFrames;
        step.reanchor = true;
    }
    if (target >= maxFrames)
    {
        target     = maxFrames;
        step.ended = !loop;
    }
    step.target = static_cast<int>(target);
    return step;
}
