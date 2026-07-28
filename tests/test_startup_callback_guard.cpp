// Regression test for the async-startup liveness guard
// (src/host/StartupCallbackGuard.h).
//
// WebView2's environment-completed and composition-controller-completed
// callbacks are one-shot: they cannot be unsubscribed, so HostWindow's
// WM_DESTROY sweep — which removes every registered handler precisely because
// the lambdas capture `this` — never reached them. And WM_DESTROY does not stop
// the pump (PostQuitMessage only posts WM_QUIT, delivered after the queue
// drains), so a completion the runtime already queued still dispatches against
// a torn-down owner (2026-07 audit, an-audit-finding).
//
// The case that matters is #3: a token issued BEFORE the owner existed no more,
// read AFTER the owner is gone. That is the exact use-after-free shape, and it
// is the assertion that flips when the guard is reverted.
//
// Case #1 is the overreach half and is just as load-bearing: a guard that
// answered false while the owner is alive would abort startup every time and
// the editor would never show UI at all. "Always false" must not pass this
// suite.
//
// Header-only; see tests/build_test_startup_callback_guard.bat.

#include "host/StartupCallbackGuard.h"

#include <cstdio>
#include <memory>

static int g_failed = 0;

#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_startup_callback_guard\n");

    // --- 1. THE OVERREACH GUARD. A live, un-retired owner must let its
    // callback through. An "always false" guard passes every other case in this
    // file and fails only here — which is why this case exists.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        CHECK(token.OwnerAlive(), "live owner: callback proceeds");
        CHECK(guard.Alive(),      "live owner: guard reports alive");
    }

    // --- 2. Retire closes the door for a token issued beforehand. This is the
    // WM_DESTROY case: the token was handed to the runtime at startup, the user
    // closed the window, and the completion has not fired yet.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        CHECK(token.OwnerAlive(), "pre-retire: token is live");
        guard.Retire();
        CHECK(!token.OwnerAlive(), "post-retire: token issued earlier is dead");
        CHECK(!guard.Alive(),      "post-retire: guard reports dead");
    }

    // --- 3. THE REVERT ASSERTION. Token outlives the owner entirely. Reading
    // it must be well-defined and must answer false — this is the dispatch that
    // used to run `make_unique<Compositor>(hMain)` on a destroyed window.
    {
        host::StartupCallbackGuard::Token token{std::shared_ptr<const bool>()};
        {
            host::StartupCallbackGuard guard;
            token = guard.Issue();
            CHECK(token.OwnerAlive(), "in scope: token is live");
        }   // guard destructs here
        CHECK(!token.OwnerAlive(), "owner destroyed: token reads dead, not freed memory");
    }

    // --- 4. One-way. There is no revive, and a token issued AFTER the retire
    // is dead on arrival — a late callback must not be able to resurrect an
    // owner that is already partway through teardown.
    {
        host::StartupCallbackGuard guard;
        guard.Retire();
        host::StartupCallbackGuard::Token late = guard.Issue();
        CHECK(!late.OwnerAlive(), "token issued after retire is dead on arrival");
    }

    // --- 5. Retire is idempotent. WM_DESTROY calls it and the destructor calls
    // it again; the second call must not throw, flip state back, or crash.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token token = guard.Issue();
        guard.Retire();
        guard.Retire();
        guard.Retire();
        CHECK(!token.OwnerAlive(), "triple retire stays dead");
    }

    // --- 6. Guards do not cross-talk. Rules out the "one process-global flag"
    // shortcut, which would work in this single-window host today and break the
    // moment anything else adopts the primitive.
    {
        host::StartupCallbackGuard a;
        host::StartupCallbackGuard b;
        host::StartupCallbackGuard::Token ta = a.Issue();
        host::StartupCallbackGuard::Token tb = b.Issue();
        a.Retire();
        CHECK(!ta.OwnerAlive(), "retiring A kills A's token");
        CHECK(tb.OwnerAlive(),  "retiring A leaves B's token live");
    }

    // --- 7. Multiple tokens from one guard all observe the retire. Startup
    // issues two (environment + composition controller) and both must decline.
    {
        host::StartupCallbackGuard guard;
        host::StartupCallbackGuard::Token envToken  = guard.Issue();
        host::StartupCallbackGuard::Token ctlToken  = guard.Issue();
        guard.Retire();
        CHECK(!envToken.OwnerAlive(), "env-creation token declines after retire");
        CHECK(!ctlToken.OwnerAlive(), "controller token declines after retire");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
