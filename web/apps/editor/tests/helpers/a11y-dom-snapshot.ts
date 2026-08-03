// ARIA snapshot wrapper for composition-mode goldens.
//
// Win32 UIA cannot reach WebView2's composition-mode tree (no HWND on
// the IDCompositionVisual that hosts the React DOM). Playwright's CDP-based
// `locator.ariaSnapshot()` bypasses the hosting question entirely and
// walks the React tree directly via the Chromium accessibility API.
//
// Output shape is a YAML string (Playwright 1.42+ format), e.g.:
//   - banner:
//     - menubar:
//       - menuitem "File"
//       - menuitem "Edit"
// This is intentionally different from the HWND lane's UIA-tree JSON
// (role/name/level vs ControlType/AutomationId/ClassName). The two
// lanes are not cross-comparable; a negative-contract spec documents
// the structural divergence.
//
// Determinism: aria snapshots are emitted in DOM order, which is
// already deterministic (no per-run resort needed unlike UIA). No
// equivalent to the HWND lane's `alwaysStripWrappers` is needed —
// CDP reaches the React tree directly without intervening Chromium
// chrome wrappers.
//
// Note on the API rename: Playwright deprecated `page.accessibility
// .snapshot()` (JSON tree) in 1.42 and removed it by 1.60. The
// replacement is `locator.ariaSnapshot()` (YAML string). The plan
// (written before the API change) assumed the old JSON shape; the
// pivot to YAML strings is documented in the commit message.

import type { Page } from "@playwright/test";

// Exit/morph animations must finish before a snapshot, or the capture races
// them and records a dying layer. Radix exits are 110ms and the curve morph is
// sub-second, so this timeout is large headroom for the common case (~0ms when
// nothing is animating); it only bites when an animation is genuinely stuck.
// The old value was 2s WITH A SILENT BACKSTOP that snapshotted anyway on
// expiry — under heavy gate load (all lanes building + capturing) 2s
// occasionally expired mid-exit and the bad snapshot surfaced as an
// INTERMITTENT golden mismatch (menubar-view-open, 2026-08). Blocking with a
// LOUD failure replaces that: a genuinely stuck animation now names itself
// instead of masquerading as a golden content drift that a refresh would
// wrongly "fix".
const A11Y_SETTLE_TIMEOUT_MS = 10_000;

async function settleBeforeSnapshot(
  page: Page,
  // Serialized into the browser by Playwright — must close over nothing but
  // the global `document`.
  predicate: () => boolean,
  description: string,
): Promise<void> {
  try {
    await page.waitForFunction(predicate, null, {
      timeout: A11Y_SETTLE_TIMEOUT_MS,
    });
  } catch (err) {
    // ONLY a genuine settle timeout gets the "stuck animation / do not refresh"
    // framing. A closed page or context, a navigation, a browser crash, or an
    // in-page evaluation error is a different failure with its own diagnostic —
    // rethrow it untouched rather than mislabeling it as an animation that
    // never settled (and wrongly advising against a golden refresh).
    const isTimeout =
      err instanceof Error &&
      (err.name === "TimeoutError" || /Timeout.*exceeded/i.test(err.message));
    if (!isTimeout) throw err;
    throw new Error(
      `a11y snapshot settle timed out after ${A11Y_SETTLE_TIMEOUT_MS}ms: ` +
        `${description} did not finish before capture. This is a real stuck ` +
        `animation, not a golden drift — do NOT refresh the golden to silence it.`,
      { cause: err },
    );
  }
}

export async function captureDomA11y(page: Page): Promise<string> {
  // Tooltip + menu/popover exit-animation settle. Keyboard-focus surfaces open
  // a Radix tooltip on the focused control (instant, deterministic); the
  // PREVIOUS tab stop's tooltip, and any menu a surface was opened via (the
  // 2026-07-18 design pass put .popover-animate on the menubar dropdowns, e.g.
  // View → Lighting…), play a 110ms Radix exit while data-state="closed". A
  // snapshot taken mid-exit nondeterministically includes the dying subtree —
  // kbd-tab-cycle-stop-2, kbd-emitter-rename-mode, dialog-lighting and
  // menubar-view-open have all flaked on exactly this. Wait for both the
  // tooltip (.tip-animate) and menu/popover (.popover-animate) families to
  // finish; the focused control's OWN open tooltip stays and is stable.
  await settleBeforeSnapshot(
    page,
    () =>
      !document.querySelector('.tip-animate[data-state="closed"]') &&
      !document.querySelector('.popover-animate[data-state="closed"]'),
    "a menubar/tooltip exit animation",
  );
  // Curve morph-animation settle. The WebView2 host is Chromium, so the curve
  // morph (sample-and-tween) runs live whenever a track edit restructures a
  // curve. A snapshot taken mid-morph would capture the transient overlay
  // <g data-testid="curve-morph-overlay"> + the hidden static layer instead of
  // the settled curve. Wait until no morph overlay remains.
  await settleBeforeSnapshot(
    page,
    () => !document.querySelector('[data-testid="curve-morph-overlay"]'),
    "a curve morph overlay",
  );
  // ariaSnapshot on body captures the whole document tree. Returns canonical
  // YAML (newline-terminated, deterministic key order).
  return page.locator("body").ariaSnapshot();
}
