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
  // ariaSnapshot on body captures the whole document tree. Returns
  // canonical YAML (newline-terminated, deterministic key order).
  return page.locator("body").ariaSnapshot();
}
