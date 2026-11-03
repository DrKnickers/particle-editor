#pragma once

// Resize-slack sizing for the engine's render target during a window drag (#866).
//
// The engine renders the scene into a sub-region of a render target the size of
// the hidden viewport popup, and the DComp clip shows only that sub-region, so a
// target larger than the popup changes no visible pixel. While the user drags a
// window edge, LayoutBroker grows the popup in kResizeSlackPx steps and never
// shrinks it, so most 1 px ticks keep the target and skip the device reset and
// the shared-texture rebuild. The exact size returns when the drag ends.
//
// Pure and header-only so tests/test_resize_slack_policy.cpp can exercise it.

namespace resizeslack {

// Steps of 128 px cost about one reset per 128 px of drag instead of one per
// pixel, and hold at most 127 px of unseen target per axis.
constexpr int kResizeSlackPx = 128;

// The popup extent along one axis that covers `need` px when it has `have` px:
// unchanged while it still covers, else `need` rounded up to a whole step.
constexpr int Cover(int need, int have)
{
    return need <= have ? have : (need + kResizeSlackPx - 1) / kResizeSlackPx * kResizeSlackPx;
}

} // namespace resizeslack
