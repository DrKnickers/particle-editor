// Regression test for unattended WebView2 init-failure UI policy.
//
// Interactive launches should show the install/error modal. Unattended launch
// modes (--capture and --drive/ephemeral) must log and exit instead of blocking
// forever behind a MessageBox.

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

    CHECK(ShouldShowWebViewInitErrorModal(false, false),
          "interactive launch shows the modal");
    CHECK(!ShouldShowWebViewInitErrorModal(true, false),
          "capture launch suppresses the modal");
    CHECK(!ShouldShowWebViewInitErrorModal(false, true),
          "drive/ephemeral launch suppresses the modal");
    CHECK(!ShouldShowWebViewInitErrorModal(true, true),
          "capture+ephemeral launch suppresses the modal");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
