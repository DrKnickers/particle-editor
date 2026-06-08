// Phase 3 Screen 8 Batch 4 — Playwright specs covering the Spawner
// panel, Import Emitters modal, and Mod Nickname dialog (demo-route
// gated). Same CDP-attach harness as sibling specs.
//
// Four specs:
//   1. Tools → Spawner opens the SpawnerPanel; opening Background
//      closes it (mutual exclusion via the openToolPanel atom).
//   2. Changing the Burst size Spinner fires `engine/state/changed`
//      with the new `spawner.burstSize` value.
//   3. File → Import Emitters opens the Import Emitters modal.
//   4. ?demo=mod-nickname renders the Mod Nickname dialog.

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

async function openMenuItem(p: Page, menu: string, item: string) {
  await p.keyboard.press("Escape").catch(() => {});
  const trigger = p.locator(`[role="menubar"] >> text=${menu}`).first();
  await trigger.click();
  await p.waitForSelector('[role="menu"]', { timeout: 2000 });
  await p.locator(`[role="menuitem"]:has-text("${item}")`).first().click();
}

async function waitForPanel(p: Page, title: string) {
  await p.waitForSelector(`[aria-label="${title}"].panel, [role="dialog"][aria-label="${title}"]`, {
    timeout: 2000,
  });
}

/** Task 2.4: Spawner panel is a right-side column. Session 11: it shares
 *  the right-dock slot with Lighting, tracked in
 *  localStorage('alo:right-dock'). Force it visible before the assertions
 *  via the toolbar toggle so we don't depend on test-order leftovers. */
async function ensureSpawnerVisible(p: Page) {
  const visible = await p.evaluate(() => {
    return !!document.querySelector('[aria-label="Spawner"].panel');
  });
  if (!visible) {
    await p.locator('button[aria-label="Toggle Spawner panel"]').first().click();
  }
}

async function ensureSpawnerHidden(p: Page) {
  const visible = await p.evaluate(() => {
    return !!document.querySelector('[aria-label="Spawner"].panel');
  });
  if (visible) {
    await p.locator('button[aria-label="Toggle Spawner panel"]').first().click();
  }
}

async function closeAnyPanel(p: Page) {
  // Task 2.4: SpawnerPanel is a docked column (session 11: shares the
  // right-dock slot with the docked Lighting pane via alo:right-dock).
  // The remaining ToolPanel-chrome dialogs are the docked Lighting pane
  // plus the Background/Ground popovers. Keep cleaning those up so
  // earlier-test residue can't leak forward.
  const dialog = p.locator('[role="dialog"]:not([data-state])').first();
  if (await dialog.count()) {
    const closeBtn = dialog.locator('button[aria-label="Close"]').first();
    if (await closeBtn.count()) {
      await closeBtn.click().catch(() => {});
    }
  }
}

test("Emitters → Spawner toggles the Spawner column", async () => {
  // Task 2.4: SpawnerPanel is a permanent right column. The Emitters
  // menu's "Spawner" entry now toggles the column (not opens a
  // slide-in). Start hidden so the menu click flips it visible.
  await closeAnyPanel(page);
  await ensureSpawnerHidden(page);
  await openMenuItem(page, "Emitters", "Spawner");
  await waitForPanel(page, "Spawner");
});

test("Opening the Background popover does not close the Spawner column (independent surfaces)", async () => {
  // Task 2.2: Background lives in a Radix Popover anchored to the
  // toolbar, separate from the workspace grid. Opening the popover
  // must not affect the permanent Spawner column.
  await closeAnyPanel(page);
  await ensureSpawnerVisible(page);
  await waitForPanel(page, "Spawner");

  await page.locator('button[aria-label="Background"]').first().click();
  await page.waitForSelector('[data-radix-popper-content-wrapper]', { timeout: 2000 });
  const stillSpawner = await page
    .locator('[aria-label="Spawner"].panel')
    .count();
  expect(stillSpawner).toBe(1);

  // Dismiss the Radix popover.
  await page.keyboard.press("Escape");
});

test("Changing Burst size fires engine/state/changed with new spawner.burstSize", async () => {
  await closeAnyPanel(page);
  await ensureSpawnerVisible(page);
  await waitForPanel(page, "Spawner");

  // Subscribe to engine/state/changed and capture each spawner.burstSize.
  await page.evaluate(() => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const w = window as any;
    if (w.__spawnerUnsub) w.__spawnerUnsub();
    w.__spawnerBurstChanges = [] as number[];
    w.__spawnerUnsub = w.bridge.on(
      "engine/state/changed",
      (e: { payload: { spawner?: { burstSize: number } } }) => {
        if (e.payload.spawner) {
          w.__spawnerBurstChanges.push(e.payload.spawner.burstSize);
        }
      },
    );
  });

  // Type a new burstSize into the Spinner and blur to commit.
  const burst = page
    .locator('[aria-label="Spawner"].panel input[aria-label="Burst size"]')
    .first();
  await burst.click();
  await page.keyboard.press("Control+A");
  await page.keyboard.type("4");
  await burst.blur();

  await page.waitForTimeout(300);

  const changes = await page.evaluate(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    () => (window as any).__spawnerBurstChanges as number[],
  );
  expect(changes).toContain(4);

  // Restore by snapping back to 1 (the default) so the next test starts
  // from a clean spawner config.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({
      kind: "spawner/start",
      params: { ...snap.spawner, burstSize: 1 },
    });
  });
});

// Radix Dialog content carries data-state="open" (matches dialogs.spec.ts).
// The Modal title becomes an aria-labelledby target; we assert presence by
// looking up the visible Dialog.Title text inside the open dialog.
const RADIX_DIALOG = '[role="dialog"][data-state="open"]';

test("File → Import Emitters opens the Import Emitters modal", async () => {
  await closeAnyPanel(page);
  await openMenuItem(page, "File", "Import Emitters");
  await page.waitForSelector(RADIX_DIALOG, { timeout: 2000 });

  // Assert the dialog's heading carries the modal title so we know we
  // opened the right one (BackgroundPicker also uses role="dialog" but
  // without data-state — RADIX_DIALOG already filters that out).
  const title = await page
    .locator(RADIX_DIALOG)
    .locator("text=Import Emitters")
    .first()
    .textContent();
  expect(title).toMatch(/Import Emitters/);

  // Dismiss the modal so the next test starts clean.
  await page.keyboard.press("Escape");
  await page.waitForSelector(RADIX_DIALOG, {
    state: "detached",
    timeout: 2000,
  });
});

test("Mod Nickname dialog opens via window.__promptModNickname (no menu entry in Batch 4)", async () => {
  // Mod Nickname has no menu trigger in Batch 4 (real auto-trigger
  // lands in the file-load batch). The lib/mod-nickname.ts module
  // exposes the imperative trigger on window.__promptModNickname so
  // tests + DevTools can open the dialog without a menu wire-up.
  await closeAnyPanel(page);
  await page.keyboard.press("Escape").catch(() => {});

  // Fire the trigger (returns a Promise<string | null>) and DON'T
  // await it — the dialog is now open and the resolver fires on
  // dismiss.
  await page.evaluate(() => {
    const w = window as unknown as { __promptModNickname?: () => unknown };
    if (w.__promptModNickname) void w.__promptModNickname();
  });

  await page.waitForSelector(RADIX_DIALOG, { timeout: 5000 });
  const title = await page
    .locator(RADIX_DIALOG)
    .locator("text=Set mod nickname")
    .first()
    .textContent();
  expect(title).toMatch(/Set mod nickname/);

  // Dismiss so the next spec starts with a clean page.
  await page.keyboard.press("Escape");
  await page.waitForSelector(RADIX_DIALOG, {
    state: "detached",
    timeout: 2000,
  });
});
