#pragma once

// WebView2 process-failure policy.
//
// The editor's whole UI — menus, title bar, panels, the viewport canvas — lives
// in the WebView2 page, so a dead web process leaves a window the user can't
// work in or close. The host's ProcessFailed handler maps the WebView2 failure
// kind onto WebFailure and asks DecideWebFailure what to do:
//
//   Reload   — the main-frame renderer exited, or stayed hung across
//              kHangReportsBeforeReload reports. The document lives in the
//              host, so reloading the page loses only UI-only state. Interactive
//              sessions only, and at most kMaxWebReloads times per session so a
//              page that crashes on every load can't loop forever.
//   MarkDead — the browser process exited (the WebView is closed for good), the
//              reload budget is spent, a reload missed kWebReloadDeadlineMs,
//              or a headless run lost its renderer. The
//              host stops treating the web as alive: an interactive session
//              saves a recovery copy and closes; a headless run exits with
//              kWebProcessFailedExitCode.
//   LogOnly  — anything WebView2 restarts by itself (GPU, utility, iframe
//              renderers), a first hang report, a hang report while a reload
//              is already under way (the deadline covers it), or a hang in a
//              headless run (its own watchdogs and ack deadlines
//              already bound the wait, and a host that was briefly busy must
//              not turn a good run into a failure).
//
// Pure and header-only (no WebView2 types) so tests/test_webview_crash_policy.cpp
// can exercise it directly.

namespace webviewcrash {

enum class WebFailure
{
    BrowserExited,        // COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED
    RenderExited,         // ..._RENDER_PROCESS_EXITED (main frame)
    RenderUnresponsive,   // ..._RENDER_PROCESS_UNRESPONSIVE (main frame)
    Other,                // iframe renderer, GPU, utility, sandbox helper, ...
};

enum class WebFailureAction { LogOnly, Reload, MarkDead };

// Reloads allowed per session before the web is declared dead.
constexpr int kMaxWebReloads = 3;

// WebView2 keeps raising RENDER_PROCESS_UNRESPONSIVE every few seconds while a
// hang lasts, and a single report is often transient (a long render, a busy
// machine). Reload only once this many reports arrive with no web message in
// between — any message from the page proves the renderer is answering again.
constexpr int kHangReportsBeforeReload = 2;

// A Reload() must bring the page back (its app/ready) within this long. A
// renderer that is still hung can't commit the reload at all, and without a
// deadline every later hang report would be skipped as "reload pending" — the
// window would never be declared dead and could never close.
constexpr unsigned kWebReloadDeadlineMs = 30000;

// Exit code for a headless run (--capture / --drive / --record / --test-host)
// whose web process failed. Matches the startup/watchdog code those modes
// already use for "the run could not proceed".
constexpr int kWebProcessFailedExitCode = 5;

// interactive   — a human is present (IsFullyInteractiveSession).
// reloadsUsed   — Reload() calls already made this session.
// reloadPending — a Reload() was issued and the page hasn't sent app/ready yet.
// hangReports   — consecutive hang reports, this one included, with no web
//                 message in between (only read for RenderUnresponsive).
inline WebFailureAction DecideWebFailure(WebFailure failure, bool interactive,
                                         int reloadsUsed, bool reloadPending,
                                         int hangReports)
{
    switch (failure)
    {
    case WebFailure::BrowserExited:
        return WebFailureAction::MarkDead;
    case WebFailure::RenderExited:
        if (!interactive) return WebFailureAction::MarkDead;
        return reloadsUsed < kMaxWebReloads ? WebFailureAction::Reload
                                            : WebFailureAction::MarkDead;
    case WebFailure::RenderUnresponsive:
        if (!interactive || reloadPending) return WebFailureAction::LogOnly;
        if (hangReports < kHangReportsBeforeReload) return WebFailureAction::LogOnly;
        return reloadsUsed < kMaxWebReloads ? WebFailureAction::Reload
                                            : WebFailureAction::MarkDead;
    default:
        return WebFailureAction::LogOnly;
    }
}

// The reload deadline fired. If the reloaded page still hasn't reported in,
// the web is dead: another Reload() would go to the same stuck renderer.
inline WebFailureAction DecideReloadDeadline(bool reloadPending)
{
    return reloadPending ? WebFailureAction::MarkDead : WebFailureAction::LogOnly;
}

// What closing the window does once the web is dead. There is no page left to
// show the Save/Discard/Cancel prompt, so the host decides alone:
//   writeRecoveryHandoff — write the unsaved document as a verified recovery
//                          copy (Autosave::WriteRecoveryHandoff);
//   keepAutosaveSession  — skip the clean-exit DeleteOurSession, so the next
//                          launch finds the copy as an orphan and offers it;
//   notifyUser           — one native MessageBoxW explaining what happened.
// Headless runs have no autosave session and no user, so they get none of it.
struct DeadWebClosePlan
{
    bool writeRecoveryHandoff;
    bool keepAutosaveSession;
    bool notifyUser;
};

inline DeadWebClosePlan PlanDeadWebClose(bool dirty, bool interactive)
{
    return DeadWebClosePlan{ dirty && interactive, dirty && interactive, interactive };
}

} // namespace webviewcrash
