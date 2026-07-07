#pragma once

// Modal-visibility policy for the host's blocking MessageBoxW dialogs.
//
// A blocking modal is only useful when a human is present to dismiss it. In any
// headless / automation mode there is no user — the modal just hangs the run
// forever (the process waits on a dialog nobody can click). Every fatal/preflight
// MessageBoxW in HostWindow must therefore be gated on this predicate; the log
// line + the non-zero exit already carry the failure to the automation harness.
//
// Modes that are NOT fully interactive:
//   - capture   : --capture / --capture-ref (one-shot headless render)
//   - automation: --drive OR --record (m_automationMode)
//   - testHost  : --test-host (CDP + Playwright contract specs; no user)
inline bool IsFullyInteractiveSession(bool capture, bool automation, bool testHost)
{
    return !capture && !automation && !testHost;
}
