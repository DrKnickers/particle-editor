// Unit test for the WebView2 process-failure policy (src/host/WebViewCrashPolicy.h).
//
// The ProcessFailed handler itself needs a live WebView2 and a killed renderer,
// so it is exercised manually; the decisions it takes are pinned here:
//   - a browser-process exit is always fatal (the WebView is closed for good);
//   - a main-frame renderer exit reloads in an interactive session until the
//     per-session budget is spent, then is fatal; a headless run never reloads;
//   - a renderer hang reloads like an exit once it outlasts one report, except
//     while a reload is already pending, and is only logged in a headless run
//     (its watchdogs decide);
//   - a reload the page never answers within the deadline is fatal;
//   - every other kind (GPU, utility, iframe renderer) is only logged;
//   - closing with a dead web writes the recovery copy and keeps the autosave
//     session only for a dirty interactive session, and notifies only a human.

#include "host/WebViewCrashPolicy.h"

#include <cstdio>

using namespace webviewcrash;

static int g_failed = 0;
#define CHECK(cond, msg) do {                              \
    if (cond) { std::printf("  ok: %s\n", msg); }          \
    else { ++g_failed; std::printf("  FAIL: %s\n", msg); } \
} while (0)

int main()
{
    std::printf("test_webview_crash_policy\n");

    // Browser process exit: fatal in every mode, whatever the reload budget.
    for (int interactive = 0; interactive < 2; ++interactive)
        for (int used = 0; used <= kMaxWebReloads; ++used)
            for (int pending = 0; pending < 2; ++pending)
                if (DecideWebFailure(WebFailure::BrowserExited, interactive != 0, used, pending != 0, 0)
                    != WebFailureAction::MarkDead)
                    CHECK(false, "browser exit must always mark the web dead");
    CHECK(true, "browser exit -> MarkDead in every mode");

    // Renderer exit, interactive: Reload while budget remains (pending or not).
    for (int used = 0; used < kMaxWebReloads; ++used)
    {
        CHECK(DecideWebFailure(WebFailure::RenderExited, true, used, false, 0) == WebFailureAction::Reload,
              "interactive renderer exit under budget -> Reload");
        CHECK(DecideWebFailure(WebFailure::RenderExited, true, used, true, 0) == WebFailureAction::Reload,
              "interactive renderer exit during a pending reload -> Reload (counts)");
    }
    CHECK(DecideWebFailure(WebFailure::RenderExited, true, kMaxWebReloads, false, 0) == WebFailureAction::MarkDead,
          "interactive renderer exit with the budget spent -> MarkDead");
    CHECK(DecideWebFailure(WebFailure::RenderExited, true, kMaxWebReloads + 7, false, 0) == WebFailureAction::MarkDead,
          "interactive renderer exit far past the budget -> MarkDead");

    // Renderer exit, headless: never reload -- exit nonzero promptly instead.
    CHECK(DecideWebFailure(WebFailure::RenderExited, false, 0, false, 0) == WebFailureAction::MarkDead,
          "headless renderer exit -> MarkDead (no reload)");

    // Renderer hang, interactive.
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, 0, false, kHangReportsBeforeReload) == WebFailureAction::Reload,
          "interactive renderer hang -> Reload");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, 0, true, kHangReportsBeforeReload) == WebFailureAction::LogOnly,
          "interactive renderer hang while a reload is pending -> LogOnly (no double spend)");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, kMaxWebReloads, false, kHangReportsBeforeReload) == WebFailureAction::MarkDead,
          "interactive renderer hang with the budget spent -> MarkDead");

    // A single hang report is often transient: only a sustained hang reloads.
    CHECK(kHangReportsBeforeReload >= 2, "one hang report alone never reloads");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, 0, false, 1) == WebFailureAction::LogOnly,
          "interactive first hang report -> LogOnly (may be transient)");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, kMaxWebReloads, false, 1) == WebFailureAction::LogOnly,
          "interactive first hang report with the budget spent -> still LogOnly");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, true, 0, false, kHangReportsBeforeReload + 5)
              == WebFailureAction::Reload,
          "interactive long hang -> Reload");

    // Reload deadline: a reload the page never answered marks the web dead,
    // so a renderer that stays hung can't leave the window unclosable.
    CHECK(DecideReloadDeadline(true) == WebFailureAction::MarkDead,
          "reload deadline expired with the reload still pending -> MarkDead");
    CHECK(DecideReloadDeadline(false) == WebFailureAction::LogOnly,
          "reload deadline after the page came back -> LogOnly");
    CHECK(kWebReloadDeadlineMs >= 5000, "the reload deadline leaves time for a real reload");

    // Renderer hang, headless: log only; the run's watchdogs bound the wait.
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, false, 0, false, kHangReportsBeforeReload) == WebFailureAction::LogOnly,
          "headless renderer hang -> LogOnly");
    CHECK(DecideWebFailure(WebFailure::RenderUnresponsive, false, kMaxWebReloads, false, kHangReportsBeforeReload) == WebFailureAction::LogOnly,
          "headless renderer hang past the budget -> LogOnly");

    // Self-recovering kinds: log only, everywhere.
    for (int interactive = 0; interactive < 2; ++interactive)
        CHECK(DecideWebFailure(WebFailure::Other, interactive != 0, kMaxWebReloads, false, kHangReportsBeforeReload)
                  == WebFailureAction::LogOnly,
              "other process kinds -> LogOnly");

    CHECK(kMaxWebReloads >= 1, "the reload budget allows at least one reload");
    CHECK(kWebProcessFailedExitCode != 0, "the headless web-failure exit code is nonzero");

    // Dead-web close plan.
    {
        const DeadWebClosePlan p = PlanDeadWebClose(/*dirty*/true, /*interactive*/true);
        CHECK(p.writeRecoveryHandoff && p.keepAutosaveSession && p.notifyUser,
              "dirty interactive: handoff + keep session + notify");
    }
    {
        const DeadWebClosePlan p = PlanDeadWebClose(false, true);
        CHECK(!p.writeRecoveryHandoff && !p.keepAutosaveSession && p.notifyUser,
              "clean interactive: notify only");
    }
    {
        const DeadWebClosePlan p = PlanDeadWebClose(true, false);
        CHECK(!p.writeRecoveryHandoff && !p.keepAutosaveSession && !p.notifyUser,
              "dirty headless: nothing (no autosave session, no user)");
    }
    {
        const DeadWebClosePlan p = PlanDeadWebClose(false, false);
        CHECK(!p.writeRecoveryHandoff && !p.keepAutosaveSession && !p.notifyUser,
              "clean headless: nothing");
    }

    std::printf("%s\n", g_failed ? "=== FAILED ===" : "=== ALL PASS ===");
    std::printf("(%d failure%s)\n", g_failed, g_failed == 1 ? "" : "s");
    return g_failed ? 1 : 0;
}
