// Phase 3 Screen 8 Batch 2 contract tests: the Lighting tool pane (now
// docked, with Bloom folded in as a section — session 11) + the Ground /
// Background toolbar popovers, wired against the real native bridge inside
// ParticleEditor.exe --new-ui --test-host. Same CDP-attach harness as
// sibling specs.
//
// What the specs cover:
//   1. View → Lighting opens the (now docked, session-11) Lighting pane.
//   2. Opening the Background popover does NOT close Lighting (the two
//      surfaces are orthogonal post-Task-2.2).
//   3. Toggling Enable inside Lighting's Bloom section fires
//      engine/set/bloom, observed via the engine/state/changed event.
//   4. The Ground popover opens from the toolbar dropdown trigger
//      (Task 2.3: View → Ground Texture… became a popover on the
//      Toolbar).
//   5. Clicking a bundled ground slot in the popover updates the
//      snapshot's groundTexture.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  if (!context) throw new Error("CDP: no browser contexts attached");
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));

  await page.waitForFunction(
    () => typeof (window as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

// Helper — open a menu by name and click an item by its visible text.
// Radix portals the content so the selector matches against [role="menu"]
// at the document root, not inside [role="menubar"].
async function openMenuItem(p: Page, menu: string, item: string) {
  await p.keyboard.press("Escape").catch(() => {});
  const trigger = p.locator(`[role="menubar"] >> text=${menu}`).first();
  await trigger.click();
  await p.waitForSelector('[role="menu"]', { timeout: 2000 });
  await p.locator(`[role="menuitem"]:has-text("${item}")`).first().click();
}

// Helper — wait for a ToolPanel with the given title to mount. ToolPanel
// uses role="dialog" with the title as aria-label; the only menu-opened
// ToolPanel after session 11 is "Lighting" (docked; Bloom folded into it,
// Background and Ground are toolbar popovers).
async function waitForPanel(p: Page, title: string) {
  await p.waitForSelector(`[role="dialog"][aria-label="${title}"]`, {
    timeout: 2000,
  });
}

async function closeAnyPanel(p: Page) {
  // The close glyph in any open ToolPanel carries aria-label="Close".
  // Background picker uses the same title so this selector is unique
  // per-panel scoped via the dialog ancestor.
  const dialog = p.locator('[role="dialog"]:not([data-state])').first();
  if (await dialog.count()) {
    const closeBtn = dialog.locator('button[aria-label="Close"]').first();
    if (await closeBtn.count()) {
      await closeBtn.click().catch(() => {});
    }
  }
}

test("View → Lighting opens the Lighting panel", async () => {
  //: Lighting moved from Tools to View.
  await closeAnyPanel(page);
  await openMenuItem(page, "View", "Lighting");
  await waitForPanel(page, "Lighting");
});

test("Opening the Background popover does not close the Lighting panel (independent surfaces)", async () => {
  // Task 2.2: Background was extracted from the slide-in ToolPanel
  // mutual-exclusion group and now lives in a Radix Popover anchored
  // to the toolbar. Opening the popover should therefore NOT close
  // whatever ToolPanel is currently open — the two are orthogonal
  // state machines.
  await closeAnyPanel(page);
  await openMenuItem(page, "View", "Lighting");
  await waitForPanel(page, "Lighting");

  // Click the Background dropdown trigger in the toolbar.
  await page.locator('button[aria-label="Background"]').first().click();

  // The popover should mount as a Radix popper wrapper, and the
  // Lighting tool panel should still be present.
  await page.waitForSelector('[data-radix-popper-content-wrapper]', { timeout: 2000 });
  const stillLighting = await page
    .locator('[role="dialog"][aria-label="Lighting"]')
    .count();
  expect(stillLighting).toBe(1);

  // Cleanup so the next test starts from a clean slate. Press Escape
  // to dismiss the Radix popover, then close the lingering ToolPanel.
  await page.keyboard.press("Escape");
  await closeAnyPanel(page);
});

test("Toggling Enable Bloom fires engine/set/bloom (observed via state/changed)", async () => {
  // Bloom settings folded into the Lighting pane (session 11): open
  // Lighting, then expand the collapsible Bloom section to reach the
  // Enable checkbox.
  await closeAnyPanel(page);
  await openMenuItem(page, "View", "Lighting");
  await waitForPanel(page, "Lighting");
  await page
    .locator('[role="dialog"][aria-label="Lighting"] [role="button"]:has-text("Bloom")')
    .first()
    .click();

  // Subscribe to engine/state/changed and capture the bloom flag from
  // each payload. The React panel commits Enable changes directly to
  // the bridge; the host emits state/changed in response.
  await page.evaluate(() => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const w = window as any;
    if (w.__bloomUnsub) w.__bloomUnsub();
    w.__bloomChanges = [] as boolean[];
    w.__bloomUnsub = w.bridge.on("engine/state/changed", (e: { payload: { bloom: boolean } }) => {
      w.__bloomChanges.push(e.payload.bloom);
    });
  });

  const before = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    return snap.bloom as boolean;
  });

  // Flip the checkbox.
  await page.locator('input[aria-label="Enable bloom"]').first().click();

  // Wait for the host to emit the resulting state/changed.
  await page.waitForTimeout(300);

  const changes = await page.evaluate(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    () => (window as any).__bloomChanges as boolean[],
  );
  // At least one event with the flipped value should have arrived.
  expect(changes).toContain(!before);

  // Restore previous state to keep the test idempotent.
  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/bloom", params: { enabled: orig } });
  }, before);

  await closeAnyPanel(page);
});

test("Ground popover opens from the toolbar dropdown trigger", async () => {
  // Task 2.3: the GroundTexturePanel slide-in ToolPanel was replaced by
  // a Radix Popover triggered from the Toolbar's Group 4 dropdown. The
  // dropdown button carries aria-label="Ground"; the mounted content is
  // a popover wrapper (data-radix-popper-content-wrapper) rather than
  // role="dialog". Mirrors the Task 2.2 Background popover spec.
  await closeAnyPanel(page);
  const probe = await page.evaluate(async () => {
    const btn = document.querySelector<HTMLButtonElement>('button[aria-label="Ground"]');
    if (!btn) return { clicked: false, popover: false, slots: 0 };
    btn.click();
    await new Promise((r) => setTimeout(r, 250));
    const popover = document.querySelector('[data-radix-popper-content-wrapper]');
    const slots = popover?.querySelectorAll("button[aria-pressed]").length ?? 0;
    return { clicked: true, popover: !!popover, slots };
  });
  expect(probe.clicked).toBe(true);
  expect(probe.popover).toBe(true);
  // GroundTexturePanelBody renders 8 aria-pressed slot buttons:
  //   solid colour (1) + bundled 0..3 (4) + custom 5..7 (3) = 8.
  expect(probe.slots).toBe(8);

  // Cleanup: dismiss the Radix popover so the next test starts clean.
  await page.keyboard.press("Escape");
});

test("Clicking a bundled ground slot in the popover updates groundTexture", async () => {
  await closeAnyPanel(page);

  const before = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.groundTexture as number;
  });

  // Pick a slot different from the current selection so the change is
  // observable. Grass (slot 1) and Sand (slot 2) are both bundled and
  // safe to switch to without affecting any custom paths.
  const targetName = before === 1 ? "Sand" : "Grass";
  const targetSlot = targetName === "Grass" ? 1 : 2;

  // Open the Ground popover from the toolbar.
  await page.locator('button[aria-label="Ground"]').first().click();
  await page.waitForSelector('[data-radix-popper-content-wrapper]', { timeout: 2000 });

  // Slot buttons live inside the popover wrapper now.
  await page
    .locator(`[data-radix-popper-content-wrapper] button[aria-label="${targetName}"]`)
    .click();

  await page.waitForTimeout(300);

  const after = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.groundTexture as number;
  });
  expect(after).toBe(targetSlot);

  // Restore.
  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/ground-texture", params: { slot: orig } });
  }, before);

  // Dismiss popover.
  await page.keyboard.press("Escape");
});
