// ARIA snapshot wrapper for composition-mode goldens (T10).
//
// Win32 UIA cannot reach WebView2's composition-mode tree (no HWND on
// the IDCompositionVisual that hosts the React DOM — see
// tasks/phase-0-a11y-cross-mode-probe.md). Playwright's CDP-based
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
// lanes are not cross-comparable; T11 documents the structural
// divergence as a negative-contract spec.
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
// pivot to YAML strings is documented in T10's commit message and
//-adjacent notes if any new lesson is warranted.

import type { Page } from "@playwright/test";

export async function captureDomA11y(page: Page): Promise<string> {
  // Tooltip exit-animation settle. Keyboard-focus surfaces open
  // a Radix tooltip on the focused control (instant, deterministic) —
  // but the PREVIOUS tab stop's tooltip plays a 110ms exit animation
  // (Radix Presence keeps it mounted while data-state="closed"), so a
  // snapshot taken mid-exit races it: kbd-tab-cycle-stop-2 and
  // kbd-emitter-rename-mode flaked golden mismatches on exactly this.
  // Wait until no exiting tooltip remains; the focused control's OPEN
  // tooltip stays in the snapshot, which is stable and intended. Costs
  // ~0ms when no tooltip is exiting (the common case for non-keyboard
  // surfaces); reduced-motion or a dropped animation can't hang it —
  // Radix unmounts on animation end and the 2s timeout backstops.
  await page
    .waitForFunction(
      () => !document.querySelector('.tip-animate[data-state="closed"]'),
      null,
      { timeout: 2_000 },
    )
    .catch(() => {
      /* backstop: snapshot anyway; the golden diff will say what's left */
    });
  // [curve-morph / Part B] Curve morph-animation settle. The WebView2
  // host is Chromium, so window.matchMedia exists and the curve morph
  // (sample-and-tween) RUNS live in the harness whenever a track edit
  // restructures a curve. A snapshot taken mid-morph would capture the
  // transient overlay <g data-testid="curve-morph-overlay"> + hidden
  // static layer instead of the settled curve. Wait until no morph
  // overlay remains; ~0ms when none is animating (the common case).
  // Same 2s backstop + catch so a stuck/edge case can't hang capture.
  await page
    .waitForFunction(
      () => !document.querySelector('[data-testid="curve-morph-overlay"]'),
      null,
      { timeout: 2_000 },
    )
    .catch(() => {
      /* backstop: snapshot anyway */
    });
  // ariaSnapshot on body captures the whole document tree. Returns
  // canonical YAML (newline-terminated, deterministic key order).
  return page.locator("body").ariaSnapshot();
}
