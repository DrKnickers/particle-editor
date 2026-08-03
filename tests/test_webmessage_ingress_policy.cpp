// Regression test for the bridge-ingress size cap (src/host/WebMessageIngressPolicy.h,
// 2026-07 audit P2-02).
//
// OnWebMessage parses the whole inbound string, so before the cap one
// postMessage could drive an arbitrarily large allocation on the UI thread.
// Defence-in-depth rather than a live hole -- the origin check upstream means
// the only speaker is our own bundle -- but an unbounded parse behind a trust
// boundary is not something to leave standing.
//
// Same extraction rationale as tests/test_webview_modal_policy.cpp: the
// callback the policy is used from lives in HostWindow.cpp, which pulls in
// WebView2 + D3D9 and cannot be linked here. What is pinned is the boundary,
// which is where an off-by-one would hide -- exactly the failure mode the
// spawn-depth cap in this same audit round proved was worth asserting from
// both sides.

#include "host/WebMessageIngressPolicy.h"
#include "ResourceLimits.h"

#include <cstdio>

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_webmessage_ingress_policy\n");

    const std::size_t cap = 1000;

    // Ordinary traffic. Real bridge requests are a few KB against a 16 Mi-char
    // cap, so the overwhelmingly common answer must be "accept".
    CHECK(ShouldAcceptWebMessage(0, cap),    "an empty message is accepted");
    CHECK(ShouldAcceptWebMessage(1, cap),    "a one-character message is accepted");
    CHECK(ShouldAcceptWebMessage(999, cap),  "just under the cap is accepted");

    // The boundary, from both sides. Inclusive at the cap: the documented limit
    // is a size the bridge ACCEPTS, not one it refuses -- so a message of
    // exactly cap characters must get through.
    CHECK(ShouldAcceptWebMessage(cap, cap),  "exactly at the cap is accepted");
    CHECK(!ShouldAcceptWebMessage(cap + 1, cap), "one past the cap is refused");

    // Well past, including the degenerate huge value an overflowing length
    // computation would produce.
    CHECK(!ShouldAcceptWebMessage(cap * 1000, cap), "far over the cap is refused");
    CHECK(!ShouldAcceptWebMessage((std::size_t)-1, cap),
          "SIZE_MAX is refused (no wraparound in the comparison)");

    // The shipped constant must leave real traffic alone. A cap small enough to
    // reject an ordinary request would break the bridge rather than protect it.
    CHECK(ShouldAcceptWebMessage(64u * 1024u, kMaxWebMessageChars),
          "the shipped cap accepts a 64 KiB request (far above real traffic)");
    CHECK(!ShouldAcceptWebMessage(kMaxWebMessageChars + 1, kMaxWebMessageChars),
          "the shipped cap refuses one character past itself");

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
