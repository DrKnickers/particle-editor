// Regression test for the host's blocking-modal visibility policy.
//
// A blocking MessageBoxW is only useful when a human is present to dismiss it.
// Every fatal/preflight modal in HostWindow is gated on IsFullyInteractiveSession
// so a headless run (capture / drive-or-record / test-host) never hangs forever
// behind a dialog nobody can click — it logs + exits instead.

#include "host/WebViewModalPolicy.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_webview_modal_policy\n");

    // Args: (capture, automation, testHost). Interactive == none set.
    CHECK(IsFullyInteractiveSession(false, false, false),
          "interactive launch shows the modal");

    // Each headless mode alone suppresses the modal.
    CHECK(!IsFullyInteractiveSession(true, false, false),
          "capture (--capture/--capture-ref) suppresses the modal");
    CHECK(!IsFullyInteractiveSession(false, true, false),
          "automation (--drive/--record) suppresses the modal");
    CHECK(!IsFullyInteractiveSession(false, false, true),
          "test-host (--test-host, CDP/Playwright) suppresses the modal");

    // Any combination stays suppressed (no way to become interactive again).
    CHECK(!IsFullyInteractiveSession(true, true, false),
          "capture+automation suppresses the modal");
    CHECK(!IsFullyInteractiveSession(true, false, true),
          "capture+test-host suppresses the modal");
    CHECK(!IsFullyInteractiveSession(false, true, true),
          "automation+test-host suppresses the modal");
    CHECK(!IsFullyInteractiveSession(true, true, true),
          "all headless flags suppress the modal");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
