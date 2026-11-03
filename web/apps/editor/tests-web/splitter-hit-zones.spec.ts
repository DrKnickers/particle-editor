// Real-browser guard for the splitter resize-cursor zone (#950). The panel
// library hit-tests separators by pointer coordinates, not by the element under
// the pointer, and it inflates a thin handle to `resizeTargetMinimumSize`, so
// the cursor zone can drift from the visible 8px band (PanelLayout's
// RESIZE_HIT_MIN pins it). jsdom has no layout, so this runs in headless
// Chromium against the mock app.
//
// The library shows its hover cursor through a global `*` rule, so it reaches
// <html>; a control's own cursor (the spinners' ns-resize) does not. Reading
// <html>'s cursor therefore detects splitter hover alone.
import { test, expect, type Page } from "@playwright/test";

/** For each separator: the perpendicular offsets (px from its leading edge) at
 *  which a pointer shows the splitter cursor, sampled at three points along it. */
async function sweep(page: Page) {
  return page.evaluate(() => {
    const splitterCursor = (x: number, y: number) => {
      const target = document.elementFromPoint(x, y) ?? document.body;
      target.dispatchEvent(new PointerEvent("pointermove", { clientX: x, clientY: y, bubbles: true, pointerType: "mouse" }));
      return getComputedStyle(document.documentElement).cursor.includes("resize");
    };
    const result = Array.from(document.querySelectorAll<HTMLElement>("[data-separator]")).map((sep) => {
      const r = sep.getBoundingClientRect();
      const vertical = r.height > r.width;
      const thickness = vertical ? r.width : r.height;
      const offsets = new Set<number>();
      for (const along of [0.2, 0.5, 0.8])
        for (let d = -12; d <= thickness + 12; d++) {
          const x = vertical ? r.left + d : r.left + r.width * along;
          const y = vertical ? r.top + r.height * along : r.top + d;
          if (splitterCursor(x, y)) offsets.add(d);
        }
      return { disabled: sep.getAttribute("aria-disabled") === "true", thickness, offsets: [...offsets].sort((a, b) => a - b) };
    });
    splitterCursor(1, 1);
    return result;
  });
}

const toggleDock = (page: Page) =>
  page.evaluate(() => document.querySelector<HTMLButtonElement>('.toolbar button[aria-label="Toggle Spawner panel"]')?.click());

/** An enabled separator's cursor zone is exactly its band (both edges inclusive);
 *  a disabled one has none. */
function expectBandOnly(seps: Awaited<ReturnType<typeof sweep>>) {
  for (const sep of seps) {
    const band = sep.disabled ? [] : Array.from({ length: sep.thickness + 1 }, (_, i) => i);
    expect(sep.offsets).toEqual(band);
  }
}

test.beforeEach(async ({ page }) => {
  await page.setViewportSize({ width: 1400, height: 900 });
  await page.goto("/");
  await page.locator('[data-testid="quadrant-viewport"]').waitFor({ state: "visible" });
});

test("the resize cursor shows only over each splitter's visible band", async ({ page }) => {
  const seps = await sweep(page);
  expect(seps.length).toBe(4);
  expectBandOnly(seps);
});

test("toggling the dock moves its splitter's cursor zone with it", async ({ page }) => {
  const before = await sweep(page);
  await toggleDock(page);
  await page.waitForTimeout(500);
  const after = await sweep(page);
  // One state has the dock closed: its splitter is disabled and shows no cursor.
  expect(before.concat(after).some((sep) => sep.disabled)).toBe(true);
  expectBandOnly(after);
});
