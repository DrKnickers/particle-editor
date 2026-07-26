// Real-browser layout guard for the texture palette popover (#683).
//
// At small window sizes the palette grew past the editor window and its lower
// tiles were unreachable — Radix flips/shifts a colliding popover but never
// shrinks it, so content taller than the viewport simply clipped. The fix caps
// the popover container at Radix's measured available space
// (--radix-popover-content-available-height/-width) and scrolls inside.
//
// jsdom can't measure any of that (clientHeight=0), so this runs in headless
// Chromium against the Vite mock app. The palette is seeded through the
// dev-only window.__paletteTest seam (browser mode's palette is inert by
// default); the popover is then opened through the real UI — emitter row →
// Appearance tab → palette button — and its geometry measured.
import { test, expect, type Page } from "@playwright/test";

const POPOVER = '[aria-label="Texture palette"]';
// The issue's repro used a ~900×571 window; 520 of height makes overflow
// certain with 12 pinned + 16 recent seeded entries (7 grid rows + chrome).
const VIEWPORT = { width: 900, height: 520 };

async function openSeededPalette(page: Page): Promise<void> {
  await page.setViewportSize(VIEWPORT);
  await page.goto("/");
  await page.waitForFunction(() => typeof window.__paletteTest?.seedPalette === "function");
  await page.evaluate(() => window.__paletteTest!.seedPalette({ pinned: 12, recent: 16 }));
  await page.click('[data-testid="emitter-row:0"]');
  await page.click('[data-testid="tab-trigger-appearance"]');
  await page.click('[data-testid="texture-palette-trigger-color"]');
  await expect(page.locator(POPOVER)).toBeVisible();
  // The seeded grid is actually populated (not the inert no-mod hint).
  await expect(page.locator('[data-testid="palette-apply-p_pin_00.dds"]')).toBeAttached();
}

test.describe("Texture palette popover layout (real browser)", () => {
  test("stays inside a small window and scrolls instead of clipping (#683)", async ({ page }) => {
    await openSeededPalette(page);

    const m = await page.evaluate((sel) => {
      const el = document.querySelector(sel) as HTMLElement | null;
      if (!el) throw new Error(`popover not found: ${sel}`);
      const r = el.getBoundingClientRect();
      return {
        top: r.top,
        bottom: r.bottom,
        left: r.left,
        right: r.right,
        clientHeight: el.clientHeight,
        scrollHeight: el.scrollHeight,
        viewportH: window.innerHeight,
        viewportW: window.innerWidth,
      };
    }, POPOVER);

    // The container itself stays fully inside the window (±1px sub-pixel slack).
    expect(m.top).toBeGreaterThanOrEqual(-1);
    expect(m.bottom).toBeLessThanOrEqual(m.viewportH + 1);
    expect(m.left).toBeGreaterThanOrEqual(-1);
    expect(m.right).toBeLessThanOrEqual(m.viewportW + 1);

    // And the seeded content genuinely overflows it — i.e. the guard is doing
    // work in this scenario, not passing vacuously on a short popover.
    expect(m.scrollHeight).toBeGreaterThan(m.clientHeight);
  });

  test("the last tile is reachable by scrolling inside the popover", async ({ page }) => {
    await openSeededPalette(page);

    const lastTile = '[data-testid="palette-apply-p_recent_15.dds"]';
    // Before scrolling, the last tile sits below the popover's visible box.
    // scrollIntoView inside the container must bring it fully on-screen —
    // this is exactly the "lower tiles cannot be viewed or selected" repro.
    await page.locator(lastTile).scrollIntoViewIfNeeded();
    const v = await page.evaluate(
      ({ tileSel, popSel }) => {
        const tile = document.querySelector(tileSel) as HTMLElement | null;
        const pop = document.querySelector(popSel) as HTMLElement | null;
        if (!tile || !pop) throw new Error("tile or popover missing");
        const tr = tile.getBoundingClientRect();
        const pr = pop.getBoundingClientRect();
        return {
          tileVisibleInPopover: tr.top >= pr.top - 1 && tr.bottom <= pr.bottom + 1,
          tileOnScreen: tr.bottom <= window.innerHeight + 1 && tr.top >= -1,
        };
      },
      { tileSel: lastTile, popSel: POPOVER },
    );
    expect(v.tileVisibleInPopover).toBe(true);
    expect(v.tileOnScreen).toBe(true);
  });
});
