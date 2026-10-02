#pragma once

// Data-loss guard: decide whether a WM_CLOSE (native frame-X /
// Alt-F4) should be vetoed so the user is prompted to save. Pure + header-only
// so the wndproc logic is unit-testable (tests/test_close_guard.cpp).
//
// Veto only a real interactive session with unsaved work. Ephemeral (--drive /
// --capture) and --test-host runs have no user to prompt and must close cleanly.
// A confirmed quit never re-enters WM_CLOSE (it goes WM_APP_QUIT_CONFIRMED →
// DestroyWindow → WM_DESTROY), so no quit-confirmed flag is needed here.
//
// webAlive: the veto hands the decision to the web page's Save/Discard/Cancel
// prompt, and only the page can end it (app/quit). With the web process dead
// that prompt can never appear, so a veto would make the window unclosable —
// the host closes through its own dead-web path instead (WebViewCrashPolicy.h).
inline bool ShouldVetoClose(bool dirty, bool ephemeral, bool testHost, bool webAlive)
{
    return dirty && !ephemeral && !testHost && webAlive;
}

// Data-loss guard for the bridge requests that replace the open document
// (file/new, and file/open of an .alo): refuse while it has unsaved work unless
// the request says the user already chose to discard it (Don't Save in the
// web's save prompt). The web gates both on that prompt, but it decides from a
// dirty bit that can trail the host's, so the host has the final say.
// Automation (--drive / --record) and --test-host runs are exempt exactly as
// ShouldVetoClose exempts them: there is no user to prompt, and their scripts
// and contract specs replace documents freely.
inline bool ShouldRefuseDocumentReplace(bool dirty, bool discardConfirmed,
                                        bool automation, bool testHost)
{
    return dirty && !discardConfirmed && !automation && !testHost;
}
