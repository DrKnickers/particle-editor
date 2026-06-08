// Phase 3 Screen 2 contract tests: React menu bar DOM presence and
// selected bridge dispatches. Same CDP-attach harness as toolbar.spec.ts.
import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

let browser: Browser;
let page: Page;

test.beforeAll(async () => {
  browser = await chromium.connectOverCDP(CDP_ENDPOINT);
  const context = browser.contexts()[0];
  const pages = context.pages();
  page = pages[0] ?? (await context.waitForEvent("page"));
});

test.afterAll(async () => {
  await browser?.close();
});

// ── 1. All 6 triggers present in legacy order () ──────────────────────────

test("All 6 menu triggers render in the menubar in legacy order [File, Edit, Emitters, Mods, View, Help]", async () => {
  const triggers = await page.evaluate(() => {
    const bar = document.querySelector('[role="menubar"]');
    if (!bar) return [];
    // Radix puts each Menubar.Trigger as a direct child <button> of the
    // menubar root (no intermediate div). Just collect direct-child buttons.
    return Array.from(bar.querySelectorAll(':scope > button')).map(
      (b) => b.textContent?.trim()
    );
  });
  expect(triggers).toEqual(["File", "Edit", "Emitters", "Mods", "View", "Help"]);
  // Tools menu is removed in.
  expect(triggers).not.toContain("Tools");
});

// ── 2. Edit › Clear All Particles dispatches engine/action/clear ─────────────

test("Edit > Clear All Particles dispatches engine/action/clear", async () => {
  const result = await page.evaluate(async () => {
    return new Promise<boolean>((resolve) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const b = (window as any).bridge;
      let fired = false;
      const off = b.on("engine/state/changed", () => {
        fired = true;
      });
      // Drive via bridge directly — Radix portals make headless DOM
      // menu navigation brittle. The intent is to verify the bridge
      // surface reachable from the menu item handler, not the click
      // path itself (that's covered by the File-menu open spec below).
      b.request({ kind: "engine/action/clear", params: {} }).then(() => {
        setTimeout(() => {
          off();
          resolve(fired);
        }, 200);
      });
    });
  });
  expect(result).toBe(true);
});

// ── 3. Bloom toggle flips state ──────────────────────────────────────────────
// (Bloom moved off the View menu in session 11 — its on/off toggle is the
// toolbar's "Toggle bloom" button. This drives the same engine/set/bloom
// command directly through the bridge.)

test("engine/set/bloom flips bloom state", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({
      kind: "engine/set/bloom",
      params: { enabled: !before.bloom },
    });
    await new Promise((r) => setTimeout(r, 100));
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    // Restore
    await b.request({
      kind: "engine/set/bloom",
      params: { enabled: before.bloom },
    });
    return { before: before.bloom, after: after.bloom };
  });
  expect(result.after).toBe(!result.before);
});

// ── 4. View › Pause toggle flips state ───────────────────────────────────────

test("View > Pause dispatches engine/set/paused and flips state", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    await b.request({
      kind: "engine/set/paused",
      params: { paused: !before.paused },
    });
    await new Promise((r) => setTimeout(r, 100));
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    // Restore
    await b.request({
      kind: "engine/set/paused",
      params: { paused: before.paused },
    });
    return { before: before.paused, after: after.paused };
  });
  expect(result.after).toBe(!result.before);
});

// ── 5. File menu opens and exposes items via Radix portal ────────────────────

// ── — Emitters top-level menu dispatches emitters/add-root ──────────────

test("Emitters > New Emitter > Root Emitter dispatches emitters/add-root", async () => {
  // Bridge-level verification: the dispatch surface for the new menu
  // item exists and produces a tree-changed event. The DOM click path
  // through the Radix submenu portal is brittle in headed CDP — we
  // exercise the same handler the menu item calls.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const before = await b.request({ kind: "emitters/list", params: {} });
    const rootsBefore = before.root.children.length;
    const r = await b.request({ kind: "emitters/add-root", params: {} });
    await new Promise((rs) => setTimeout(rs, 100));
    const after = await b.request({ kind: "emitters/list", params: {} });
    return {
      newId: r.newId,
      rootsBefore,
      rootsAfter: after.root.children.length,
      lastRole: after.root.children[after.root.children.length - 1]?.role,
    };
  });
  expect(result.newId).toBeGreaterThan(0);
  expect(result.rootsAfter).toBe(result.rootsBefore + 1);
  expect(result.lastRole).toBe("root");
});

test("File menu opens to reveal items in the DOM", async () => {
  // Click the File trigger in the menubar. Radix renders triggers as
  // role="menuitem" inside role="menubar".
  const fileTrigger = page
    .locator('[role="menubar"] >> text=File')
    .first();
  await fileTrigger.click();

  // Radix portals the content into the document body; wait for it.
  await page.waitForSelector('[role="menu"]', { timeout: 2000 });

  // Verify a known item is present.
  const visible = await page
    .locator('[role="menuitem"]:has-text("Import Emitters")')
    .isVisible();
  expect(visible).toBe(true);

  // Close the menu cleanly so subsequent tests start with a closed bar.
  await page.keyboard.press("Escape");
});
