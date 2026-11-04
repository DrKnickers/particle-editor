// Unit test for the resize-slack sizing (src/host/ResizeSlackPolicy.h).
//
// LayoutBroker grows the hidden viewport popup with Cover during a window drag;
// the drag itself needs a live window, so only the sizing is pinned here:
//   - a size the popup still covers keeps the popup, so the tick skips a reset;
//   - a larger size rounds up to a whole step, at most one step past it;
//   - a 1 px drag across a wide range resets about once per step, not per pixel.

#include "host/ResizeSlackPolicy.h"

#include <cstdio>

using namespace resizeslack;

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

int main()
{
    std::printf("test_resize_slack_policy\n");

    CHECK(Cover(1000, 1000) == 1000, "an unchanged size keeps the popup");
    CHECK(Cover(900, 1000) == 1000, "a smaller size keeps the popup: the target never shrinks mid-drag");
    CHECK(Cover(1001, 1000) == 1024, "growing past the popup rounds up to a whole step");
    CHECK(Cover(1024, 1000) == 1024, "a size on a step boundary is that step");
    CHECK(Cover(1025, 1024) == 1152, "one pixel past a step takes the next step");
    CHECK(Cover(1, 0) == kResizeSlackPx, "a first size takes one whole step");

    bool withinOneStep = true;
    for (int need = 1; need <= 4096; ++need)
    {
        const int got = Cover(need, 0);
        withinOneStep = withinOneStep && got >= need && got - need < kResizeSlackPx && got % kResizeSlackPx == 0;
    }
    CHECK(withinOneStep, "every grown size covers the need by less than one step, on a step boundary");

    int popup = 1264, resets = 0;
    for (int w = 1265; w <= 1264 + 512; ++w)
    {
        const int next = Cover(w, popup);
        if (next != popup)
        {
            ++resets;
            popup = next;
        }
    }
    for (int w = 1264 + 511; w >= 1264; --w)
    {
        const int next = Cover(w, popup);
        if (next != popup)
        {
            ++resets;
            popup = next;
        }
    }
    CHECK(resets == 5, "a 512 px drag out and back from 1264 px resets 5 times instead of 1023");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_failed ? 1 : 0;
}
