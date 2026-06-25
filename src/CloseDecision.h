#pragma once

// Data-loss guard: decide whether a WM_CLOSE (native frame-X /
// Alt-F4) should be vetoed so the user is prompted to save. Pure + header-only
// so the wndproc logic is unit-testable (tests/test_close_guard.cpp).
//
// Veto only a real interactive session with unsaved work. Ephemeral (--drive /
// --capture) and --test-host runs have no user to prompt and must close cleanly.
// A confirmed quit never re-enters WM_CLOSE (it goes WM_APP_QUIT_CONFIRMED →
// DestroyWindow → WM_DESTROY), so no quit-confirmed flag is needed here.
inline bool ShouldVetoClose(bool dirty, bool ephemeral, bool testHost)
{
    return dirty && !ephemeral && !testHost;
}
