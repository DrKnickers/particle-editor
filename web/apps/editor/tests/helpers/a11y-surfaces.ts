import type { Page } from "@playwright/test";

// Each surface is a "drive the app to this state" recipe used by the
// composition DOM-snapshot specs (captures via
// page.accessibility.snapshot()). id matches the golden filename:
// a11y-goldens/<id>.composition.golden.yaml. (The legacy HWND UIA specs
// + their <id>.golden.json goldens were removed.)
export type SurfaceCapture = {
  id: string;
  setup: (page: Page) => Promise<void>;
  teardown: (page: Page) => Promise<void>;
};

async function dismissModals(page: Page) {
  // Coarse cleanup — closes any open menu / dialog. If a test leaves
  // the editor mid-rename or mid-IME, this won't recover; surface that
  // through follow-ups if it bites.
  await page.keyboard.press("Escape");
  await page.keyboard.press("Escape");
}

/**
 * Force the canonical captured UI state before a golden run.
 *
 * The native a11y harness reuses the host's STABLE WebView2 user-data
 * folder (`ComputeUserDataFolder`, src/host/HostWindow.cpp), so whatever
 * theme / panel state a prior interactive session (e.g. a live smoke) left
 * in `localStorage` leaks into the next capture. Every golden is pinned to
 * **light theme + Spawner docked** — seed those keys and reload so the
 * module-init reads (ThemeToggle's `alo:theme`, the right-dock store's
 * `alo:right-dock`) pick them up regardless of persisted state. Without
 * this, a blanket `a11y:update` rewrites every golden with the machine's
 * incidental theme/panel state.
 *
 * Call once per a11y spec's `beforeAll`, after the bridge is ready and
 * before any capture (HWND lane: before `discoverHostHwnd`).
 */
export async function seedCanonicalUiState(page: Page): Promise<void> {
  await page.evaluate(() => {
    localStorage.setItem("alo:theme", "light");
    // Right-dock = "spawner" (session 11; was the legacy
    // `alo:spawner-visible=true`). The Spawner occupies the shared
    // right-dock slot for every full-page golden; the dialog-lighting
    // surface swaps it to Lighting for its own capture.
    localStorage.setItem("alo:right-dock", "spawner");
  });
  await page.reload({ waitUntil: "domcontentloaded" });
  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
}

// Note: EmitterPropertyTabs.tsx already exposes
// `data-testid="emitter-property-tabs"` on its Tabs.Root (used by
// existing vitest + Playwright property-tabs specs). The plan
// asked for a new `property-tabs` testid, but adding a duplicate
// would require either wrapping Tabs.Root in an otherwise-pointless
// div or renaming the existing testid (scope creep — 5+ active
// callers). Reusing the existing testid here is the surgical fix.
// MenuBar triggers are Radix Menubar.Trigger which renders as
// `<button>` with the menu name as direct text, so `button:has-text`
// selectors work as the plan expects.

export const CHROME_SURFACES: SurfaceCapture[] = [
  {
    id: "menubar-closed",
    setup: async (page) => {
      await page.locator('[data-testid="app-shell"]').focus();
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "menubar-file-open",
    setup: async (page) => {
      await page.locator('button:has-text("File")').click();
      await page.waitForSelector('[role="menu"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "menubar-edit-open",
    setup: async (page) => {
      await page.locator('button:has-text("Edit")').click();
      await page.waitForSelector('[role="menu"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "menubar-emitters-open",
    setup: async (page) => {
      await page.locator('button:has-text("Emitters")').click();
      await page.waitForSelector('[role="menu"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  // menubar-mods-open is intentionally NOT a golden surface: the Mods menu
  // lists mods scanned asynchronously from disk, so its captured tree depends
  // on this machine's installed mod set AND on how complete the scan was when
  // the snapshot was taken (the menu appears before the scan settles). That
  // makes it a non-deterministic, machine-coupled golden — the weekly drift
  // check surfaced it as recurring false drift. The mods menu's behaviour is
  // covered functionally by menu-bar.spec.ts instead.
  {
    id: "menubar-view-open",
    setup: async (page) => {
      await page.locator('button:has-text("View")').click();
      await page.waitForSelector('[role="menu"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "menubar-help-open",
    setup: async (page) => {
      await page.locator('button:has-text("Help")').click();
      await page.waitForSelector('[role="menu"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "toolbar",
    setup: async (page) => {
      await page.locator('[data-testid="toolbar"]').focus();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "emitter-tree",
    setup: async (page) => {
      // Assumes fixture has at least one root emitter loaded.
      await page.locator('[data-testid="emitter-tree"]').focus();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "property-tabs-basic",
    setup: async (page) => {
      await page.locator('[role="tab"]:has-text("Basic")').click();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "property-tabs-appearance",
    setup: async (page) => {
      await page.locator('[role="tab"]:has-text("Appearance")').click();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "property-tabs-physics",
    setup: async (page) => {
      await page.locator('[role="tab"]:has-text("Physics")').click();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  // viewport-pill surface removed: the ViewportPill was deleted and its
  // three toggles moved into the toolbar (captured by the `toolbar`
  // surface + every full-page golden's toolbar region).
];

// ─── Dialog surfaces ──────────────────────────────────────────────────
//
// Every dialog captured here renders through `<Modal>` (Radix Dialog
// primitives) or `<ToolPanel>` (a self-rendered `role="dialog"`
// container). The two share enough structural a11y semantics — a
// labelled, dismissable, role="dialog" container with an X close
// glyph — that they can sit alongside each other in one surface list.
//
// Trigger discovery:
//   - Menu-triggered dialogs: click the Menubar.Trigger, then click
//     the Menubar.Item by visible text. The menu items render
//     `role="menuitem"` via Radix.
//   - Context-menu-triggered dialogs (tree-context atom): right-click
//     the first emitter-tree row to open the row's ContextMenu, then
//     click the item. Assumes the fixture loads at least one root
//     emitter (a11y-base-state.alo).
//
// Deferred surfaces:
//   - dialog-save-changes (needs a dirty in-memory document)
//   - dialog-link-group-settings (needs an emitter with linkGroup > 0)
//   - background-picker / ground-texture (no longer Modal — replaced
//     by toolbar Popovers)
//   - primitives-gallery (separate ?demo=primitives route, full-page
//     replacement — not a dialog overlay)
//   - spawner (always-on right column, not a dialog)
//
// Teardown: most dialogs close on Esc. The tree-context dialogs close
// via Esc too (Radix Dialog handles it), then a second Esc clears any
// lingering context-menu state for safety.

export const DIALOG_SURFACES: SurfaceCapture[] = [
  // ── Menu-triggered Modal dialogs ─────────────────────────────────
  {
    id: "dialog-keyboard-shortcuts",
    setup: async (page) => {
      await page.locator('button:has-text("Help")').click();
      await page.locator('[data-testid="menu-help-shortcuts"]').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-about",
    setup: async (page) => {
      await page.locator('button:has-text("Help")').click();
      await page.locator('[role="menuitem"]:has-text("About")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-rescale-system",
    setup: async (page) => {
      await page.locator('button:has-text("Edit")').click();
      await page.locator('[role="menuitem"]:has-text("Rescale")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-reset-view-settings",
    setup: async (page) => {
      await page.locator('button:has-text("View")').click();
      await page.locator('[role="menuitem"]:has-text("Reset View Settings")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-import-emitters",
    setup: async (page) => {
      // Modal opens with body in its "no file picked" state — the
      // Browse… button is the only enabled control. We capture this
      // state, not the post-preview tree state (which needs a real
      // file/open round-trip).
      await page.locator('button:has-text("File")').click();
      await page.locator('[role="menuitem"]:has-text("Import Emitters")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },

  // ── Menu-triggered ToolPanel dialogs (role="dialog" container) ───
  {
    id: "dialog-lighting",
    setup: async (page) => {
      await page.locator('button:has-text("View")').click();
      await page.locator('[role="menuitem"]:has-text("Lighting")').click();
      // ToolPanel renders `<div role="dialog" aria-label="Lighting">`
      // — not portalled like Modal, but still queryable as a dialog.
      await page.waitForSelector('[role="dialog"][aria-label="Lighting"]');
    },
    teardown: async (page) => {
      // ToolPanel does NOT close on Esc by design (modeless tool
      // window). Click its X glyph instead — it collapses the dock.
      await page
        .locator('[role="dialog"][aria-label="Lighting"] [aria-label="Close"]')
        .click();
    },
  },
  // dialog-bloom-settings removed (session 11): Bloom settings folded into
  // the Lighting pane as a section, so they're captured by dialog-lighting.

  {
    // Autosave crash-recovery. The real check-recovery is suppressed
    // under --test-host, so drive the dialog via the ?demo=autosave-recovery
    // route, which renders it with a FIXED both-tiers orphan + FIXED nowMs —
    // deterministic age text for the golden.
    id: "dialog-autosave-recovery",
    setup: async (page) => {
      await page.evaluate(() => {
        const base = window.location.href.split("?")[0];
        window.location.href = base + "?demo=autosave-recovery";
      });
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => {
      await page.keyboard.press("Escape");
      await page.evaluate(() => {
        window.location.href = window.location.href.split("?")[0];
      });
      await page.waitForSelector('[data-testid="app-shell"]');
    },
  },

  // ── Tree-context (right-click) Modal dialogs ─────────────────────
  // Each requires the fixture to have at least one root emitter so
  // `[data-testid="emitter-tree"] [role="treeitem"]` resolves to a
  // clickable row. The tree-context atom in lib/tree-context.ts is
  // driven by the row's onSelect handlers, which call
  // openDialog(<kind>, emitterId).
  {
    id: "dialog-increment-index",
    setup: async (page) => {
      const firstRow = page
        .locator('[data-testid="emitter-tree"] [role="treeitem"]')
        .first();
      await firstRow.click({ button: "right" });
      await page.locator('[role="menuitem"]:has-text("Increment Index")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-rescale-emitter",
    setup: async (page) => {
      const firstRow = page
        .locator('[data-testid="emitter-tree"] [role="treeitem"]')
        .first();
      await firstRow.click({ button: "right" });
      await page.locator('[role="menuitem"]:has-text("Rescale Emitter")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
  {
    id: "dialog-set-link-group",
    setup: async (page) => {
      const firstRow = page
        .locator('[data-testid="emitter-tree"] [role="treeitem"]')
        .first();
      await firstRow.click({ button: "right" });
      await page.locator('[role="menuitem"]:has-text("Set Link Group")').click();
      await page.waitForSelector('[role="dialog"]');
    },
    teardown: async (page) => { await dismissModals(page); },
  },
];

// ─── Keyboard / interaction surfaces ──────────────────────────────────
//
// These drivers are mode-agnostic — the same setup/teardown recipes are
// consumed by the HWND UIA specs and the composition DOM-snapshot
// specs. No menu or dialog is left open by any driver here, so
// dismissModals() is not needed; teardowns are either a single Escape
// (cancel rename) or no-op.
//
// Assumption: kbd-emitter-rename-mode assumes the loaded fixture exposes
// at least one root emitter and that F2 on the focused row enters rename
// mode. Whether rename-mode actually appears in the captured UIA tree is
// validated when goldens are generated.

export const KEYBOARD_SURFACES: SurfaceCapture[] = [
  {
    id: "kbd-tab-cycle-stop-1",
    setup: async (page) => {
      await page.locator('[data-testid="app-shell"]').focus();
      await page.keyboard.press("Tab");
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "kbd-tab-cycle-stop-2",
    setup: async (page) => {
      await page.locator('[data-testid="app-shell"]').focus();
      await page.keyboard.press("Tab");
      await page.keyboard.press("Tab");
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "kbd-emitter-rename-mode",
    setup: async (page) => {
      await page.locator('[data-testid="emitter-tree"] [role="treeitem"]').first().click();
      await page.keyboard.press("F2");
    },
    teardown: async (page) => { await page.keyboard.press("Escape"); },
  },
  {
    id: "kbd-arrow-tree-expanded",
    setup: async (page) => {
      await page.locator('[data-testid="emitter-tree"] [role="treeitem"]').first().focus();
      await page.keyboard.press("ArrowRight");
    },
    teardown: async (_page) => { /* no-op */ },
  },
];

// ─── Custom-primitive surfaces ────────────────────────────────────────
//
// CurveEditor canvas:
//   The plan referenced `[data-testid="curve-editor-canvas"]`, which
//   doesn't exist. `data-testid="curve-editor-svg"` is already on the
//   interactive SVG canvas at CurveEditor.tsx:802 and CurveEditor.tsx:1473
//   (two render paths — single-channel and multi-channel). The SVG has no
//   `tabIndex`, so `.click()` delivers pointer-active state rather than
//   keyboard focus — the surface id `curve-editor-focused` captures the
//   cursor-active UIA tree, which is what the goldens will pin down.
//   Reusing the existing testid is surgical (no React change needed) per
//   the precedent of preferring existing testids over new duplicate
//   attrs. The surface id remains unchanged — it's an internal label for
//   the golden file, not a selector.
//
// Spinner:
//   The plan referenced `[data-testid="spinner-emit-rate"]` — a
//   placeholder name that doesn't correspond to any real field. The
//   closest semantically correct control on the Basic tab is
//   "Particles/second:" (the `nParticlesPerSecond` FieldSpinner under the
//   Continuous stream radiogroup at EmitterPropertyTabs.tsx:491). This is
//   the first spinner on the Basic tab that represents a continuous
//   emission-rate quantity (analogous to "emit rate" in plain language).
//   The surgical fix: added optional `testId?: string` prop to
//   `FieldSpinner` in EmitterPropertyTabs.tsx and applied
//   `testId="spinner-particles-per-second"` at that one callsite. The prop
//   forwards onto FieldSpinner's outermost `.form-row` div, which already
//   wraps the input. No change to the generic Spinner.tsx primitive.
//   The surface selector targets `input` inside that row (the focusable
//   element), matching the plan's `.focus()` approach.

export const CUSTOM_PRIMITIVE_SURFACES: SurfaceCapture[] = [
  {
    id: "curve-editor-focused",
    setup: async (page) => {
      // Select the first emitter explicitly — file/open in beforeEach does
      // NOT reset m_selectedEmitterId in BridgeDispatcher, so selection
      // state may be -1 (no emitter) depending on test ordering. Without
      // an explicit selection, curve-editor-svg shows a placeholder
      // instead of the SVG and the locator never resolves.
      await page.locator('[data-testid="emitter-tree"] [role="treeitem"]').first().click();
      await page.locator('[role="tab"]:has-text("Basic")').click();
      await page.locator('[data-testid="curve-editor-svg"]').click();
    },
    teardown: async (_page) => { /* no-op */ },
  },
  {
    id: "spinner-focused",
    setup: async (page) => {
      // Same selection-state reason as curve-editor-focused above.
      await page.locator('[data-testid="emitter-tree"] [role="treeitem"]').first().click();
      await page.locator('[role="tab"]:has-text("Basic")').click();
      await page.locator('[data-testid="spinner-particles-per-second"] input').focus();
    },
    teardown: async (_page) => { /* no-op */ },
  },
];
