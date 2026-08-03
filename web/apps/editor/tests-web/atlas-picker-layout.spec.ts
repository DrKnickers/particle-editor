// Real-browser layout guard for the Atlas Picker grid (the centering fix from
// #287). jsdom can't measure layout (clientWidth=0), so this runs in headless
// Chromium via the Vite dev server (mock app). It opens the picker over a 64-cell
// atlas through the DEV test seam (window.__atlasTest) and measures real geometry.
import { test, expect, type Page } from "@playwright/test";

const PANEL = '[role="dialog"][aria-label="Atlas Frames"]';
// Post-#572 the grid is a single <canvas> inside a fixed-size mx-auto box; there
// are no per-cell elements. Centering + column count are read off the box + the
// geometry the canvas publishes (data-atlas-cols/-cell/-gap).
const CANVAS = '[data-testid="atlas-canvas"]';
const BOX = '[data-testid="atlas-grid-box"]';

/** Open the Atlas Picker over an N-cell atlas and wait for the grid to render. */
async function openAtlas(page: Page, textureSize = 64): Promise<void> {
  await page.goto("/");
  await page.waitForFunction(() => typeof window.__atlasTest?.seedAtlas === "function");
  await page.evaluate((ts) => window.__atlasTest!.seedAtlas({ textureSize: ts }), textureSize);
  await expect(page.getByRole("listbox", { name: /atlas frames/i })).toBeVisible();
  await expect(page.locator(BOX)).toBeVisible();
}

/** Measure the grid box's horizontal center vs the panel center, the column
 *  count (data-atlas-cols), the box width, and the width the published geometry
 *  implies (cols*cell + (cols-1)*gap). All in the browser. */
async function measure(page: Page) {
  return page.evaluate(
    ({ panelSel, boxSel, canvasSel }) => {
      const panel = document.querySelector(panelSel) as HTMLElement | null;
      if (!panel) throw new Error(`panel not found: ${panelSel}`); // explicit > opaque null-deref
      const box = document.querySelector(boxSel) as HTMLElement | null;
      if (!box) throw new Error(`grid box not found: ${boxSel}`);
      const canvas = document.querySelector(canvasSel) as HTMLElement | null;
      if (!canvas) throw new Error(`canvas not found: ${canvasSel}`);
      const br = box.getBoundingClientRect();
      const pr = panel.getBoundingClientRect();
      const cols = Number(canvas.getAttribute("data-atlas-cols"));
      const cell = Number(canvas.getAttribute("data-atlas-cell"));
      const gap = Number(canvas.getAttribute("data-atlas-gap"));
      return {
        cols,
        boxWidth: br.width,
        expectedWidth: cols * cell + (cols - 1) * gap,
        offset: (br.left + br.right) / 2 - (pr.left + pr.right) / 2,
      };
    },
    { panelSel: PANEL, boxSel: BOX, canvasSel: CANVAS },
  );
}

test.describe("Atlas Picker grid layout (real browser)", () => {
  test("the grid is horizontally centered in the panel", async ({ page }) => {
    await openAtlas(page, 64);
    const m = await measure(page);
    // The box width matches the published grid geometry (cols*cell + gaps), so
    // its center is a faithful stand-in for the old per-cell block center.
    expect(m.cols).toBeGreaterThan(0);
    expect(Math.abs(m.boxWidth - m.expectedWidth)).toBeLessThanOrEqual(1);
    // The #287 fix (ToolPanel bodyScroll={false} + scrollbar-gutter both-edges +
    // justify-center / mx-auto box) centers the grid; a regression to a one-sided
    // gutter would push it ~7-15px off. Allow ±3px for sub-pixel + scrollbar rounding.
    expect(Math.abs(m.offset)).toBeLessThanOrEqual(3);
  });

  test("the grid reflows to more columns at a wider dock, staying centered", async ({ page }) => {
    await openAtlas(page, 64);
    const narrow = await measure(page);

    // Widen the dock by dragging its splitter (the last vertical splitter, between
    // the centre column and the right dock) left by 140px. At ~48px/column that's
    // ~3 column-widths of travel — comfortably past the one boundary needed to add a
    // column, so the "cols increased" assertion isn't on a knife-edge.
    const splitter = page.locator(".ce-splitter-v").last();
    const box = await splitter.boundingBox();
    if (!box) throw new Error("dock splitter not found");
    await page.mouse.move(box.x + box.width / 2, box.y + box.height / 2);
    await page.mouse.down();
    await page.mouse.move(box.x - 140, box.y + box.height / 2, { steps: 8 });
    await page.mouse.up();

    // Column count must increase (responsive reflow) and centering must hold.
    await expect.poll(async () => (await measure(page)).cols).toBeGreaterThan(narrow.cols);
    const wide = await measure(page);
    expect(Math.abs(wide.offset)).toBeLessThanOrEqual(3);
  });
});
