#include "host/ViewportUnavailablePolicy.h"
#include "host/WebViewModalPolicy.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_viewport_unavailable_policy\n");
    using namespace host;
    CHECK(DecideViewportUnavailable(true) == ViewportUnavailableAction::ShowNotice,
          "interactive session keeps editing with a persistent notice");
    CHECK(DecideViewportUnavailable(false) == ViewportUnavailableAction::Exit,
          "noninteractive session exits instead of producing invalid results");

    // Mirror the host's complete session predicate, including mixed flags.
    // automation covers both --drive and --record (visible or headless).
    for (int capture = 0; capture < 2; ++capture)
        for (int automation = 0; automation < 2; ++automation)
            for (int testHost = 0; testHost < 2; ++testHost)
            {
                const bool interactive = IsFullyInteractiveSession(
                    capture != 0, automation != 0, testHost != 0);
                const auto action = DecideViewportUnavailable(interactive);
                CHECK(action == ((capture || automation || testHost)
                      ? ViewportUnavailableAction::Exit : ViewportUnavailableAction::ShowNotice),
                      "capture/drive/record/test-host flags select fatal policy");
            }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    return g_failed ? 1 : 0;
}
