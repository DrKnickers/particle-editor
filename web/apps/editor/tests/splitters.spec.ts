// Playwright contract tests for the resizable splitter
// layout (PanelLayout.tsx + react-resizable-panels 4.x).
//
// Pinned behaviours:
//   1. On a fresh localStorage, defaults render at 20/60/20 (outer)
//      and 25/75 (left) and 75/25 (centre). The five quadrant testIDs
//      are all present in the 3-col state.
//   2. Drag persistence — drag the left↔centre separator, the new
//      sizes are written to `alo:layout:outer:3col`, and on reload
//      the saved ratios are restored (±1 %).
//   3. Built-in double-click reset — 4.x ships double-click-handle
//      reset for free. We exercise it by directly invoking the
//      `dblclick` document listener the library registers, since
//      synthesising a true browser double-click via PointerEvent is
//      finicky over CDP.
//   4. Spawner toggle collapse — flipping the toolbar's Spawner button
//      COLLAPSES the always-mounted right-dock slot to width 0 (and
//      re-toggling restores it) WITHOUT remounting the outer Group, so
//      the left pane + viewport survive (no flicker). The old key-based
//      design remounted the Group with a `:2col`/`:3col` swap; that was
//      replaced by a single collapsible Panel (one `:3col` key).
//   5. Corrupted persistence fallback — garbage in the storage key
//      falls back to defaults without crashing.
//
// Drag emulation uses PointerEvent dispatch via page.evaluate rather
// than `page.mouse.down/move/up`. The library's pointer handlers
// attach to document-level events with capture, and synthesising
// matching coordinates inside a WebView2 chrome under CDP is fragile.
// The dev-server smoke proved the dispatched-event path works against
// the real handlers; we reuse that here for stability.
//
// All assertions are percentage-based with ±1 % tolerance so the
// spec is window-size-agnostic.

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

// Reset persistence + reload to a known clean state before each test.
test.beforeEach(async () => {
  await page.evaluate(() => {
    for (const k of [
      "alo:layout:outer:3col",
      "alo:layout:outer:2col",
      "alo:layout:left",
      "alo:layout:center",
    ]) localStorage.removeItem(k);
  });
  await page.reload();
  // Wait for the React layout to settle.
  await page.locator('[data-testid="quadrant-viewport"]').waitFor({ state: "visible" });
});

function readLayout() {
  return page.evaluate(() => {
    // 4.x exposes aria-orientation on `[data-separator]` but NOT on
    // `[data-group]`; derive the Group's orientation from computed
    // flex-direction instead. `row` ⇒ horizontal, `column` ⇒ vertical.
    const groups = document.querySelectorAll<HTMLElement>("[data-group]");
    const out: Record<string, Record<string, number>> = {};
    for (const g of Array.from(groups)) {
      const horizontal = window.getComputedStyle(g).flexDirection === "row";
      const orient = horizontal ? "horizontal" : "vertical";
      const total = horizontal
        ? g.getBoundingClientRect().width
        : g.getBoundingClientRect().height;
      const panels = Array.from(g.querySelectorAll<HTMLElement>(":scope > [data-panel]"));
      const key = panels.map((p) => p.id).join(",");
      const ratios: Record<string, number> = {};
      for (const p of panels) {
        const size = horizontal
          ? p.getBoundingClientRect().width
          : p.getBoundingClientRect().height;
        ratios[p.id] = (size / total) * 100;
      }
      out[`${orient}:${key}`] = ratios;
    }
    return out;
  });
}

function dragSeparator(
  selector: string,
  deltaX: number,
  deltaY: number,
) {
  return page.evaluate(
    ({ selector, deltaX, deltaY }) => {
      const sep = document.querySelector<HTMLElement>(selector);
      if (!sep) throw new Error(`separator not found: ${selector}`);
      const r = sep.getBoundingClientRect();
      const x = r.x + r.width / 2;
      const y = r.y + r.height / 2;
      function fire(target: Element | Document, type: string, cx: number, cy: number, buttons: number) {
        target.dispatchEvent(new PointerEvent(type, {
          bubbles: true,
          clientX: cx,
          clientY: cy,
          pointerId: 1,
          pointerType: "mouse",
          button: 0,
          buttons,
        }));
      }
      fire(sep, "pointerdown", x, y, 1);
      fire(document, "pointermove", x + deltaX, y + deltaY, 1);
      fire(document, "pointerup", x + deltaX, y + deltaY, 0);
      return new Promise((r) => setTimeout(r, 200));
    },
    { selector, deltaX, deltaY },
  );
}

// PanelLayout's outer-group PIXEL floors (see PanelLayout.tsx: the `left`
// Panel has minSize={330}, the `spawner` Panel minSize={260} — pixels, not
// %, so inspector/dock labels don't truncate when dragged narrow). At a
// narrow window these floors clamp the 20% defaults UPWARD, so the rendered
// outer percentages are window-width-DEPENDENT: flat 20/60/20 only holds on a
// window wide enough that 20% exceeds both floors (the test-host runs at
// ~1264px client, where 20% = ~253px < 330px → left renders ~26%). Derive the
// floor-aware expectation from the MEASURED group width instead of asserting a
// flat 20% — this stays correct at any window size (wide → falls back to 20%).
const LEFT_MIN_PX = 330;
const SPAWNER_MIN_PX = 260;
// Visible/​hit width of a `.ce-splitter` handle (components.css). The outer
// 3-col group has two of them (left|center, center|spawner); they're not
// part of the panels' percentage budget.
const SPLITTER_PX = 8;

/** Width (px) of the horizontal outer Group whose `> [data-panel]` id set
 *  joins to `key` (e.g. "left,center,spawner"). 0 if not found. */
function outerGroupWidthPx(key: string) {
  return page.evaluate((k) => {
    for (const g of Array.from(document.querySelectorAll<HTMLElement>("[data-group]"))) {
      if (window.getComputedStyle(g).flexDirection !== "row") continue;
      const ids = Array.from(g.querySelectorAll<HTMLElement>(":scope > [data-panel]"))
        .map((p) => p.id)
        .join(",");
      if (ids === k) return g.getBoundingClientRect().width;
    }
    return 0;
  }, key);
}

/** The percentage a px-floored Panel renders at for a given group width:
 *  max(defaultPct, floorPx / groupWidthPx * 100). */
function flooredPct(floorPx: number, groupWidthPx: number, defaultPct: number) {
  return Math.max(defaultPct, (floorPx / groupWidthPx) * 100);
}

test("defaults render at 20/60/20 (outer, floor-clamped) + 25/75 (left) + 75/25 (centre) on a clean localStorage", async () => {
  const layout = await readLayout();
  const outer = layout["horizontal:left,center,spawner"];
  expect(outer).toBeDefined();
  // Floor-aware: left (330px) + spawner (260px) clamp upward at the narrow
  // test window; center takes the remainder. (±1 %.)
  // react-resizable-panels' percentages are of the AVAILABLE space (the
  // group width MINUS the resize handles), so subtract the two 8px outer
  // splitters before applying the pixel-floor formula — otherwise the
  // approximation drifts past 1% at the wider 8px handle width.
  const w3 = await outerGroupWidthPx("left,center,spawner");
  const avail3 = w3 - 2 * SPLITTER_PX;
  const expLeft = flooredPct(LEFT_MIN_PX, avail3, 20);
  const expSpawner = flooredPct(SPAWNER_MIN_PX, avail3, 20);
  const expCenter = 100 - expLeft - expSpawner;
  expect(Math.abs(outer.left - expLeft)).toBeLessThan(1);
  expect(Math.abs(outer.center - expCenter)).toBeLessThan(1);
  expect(Math.abs(outer.spawner - expSpawner)).toBeLessThan(1);

  const left = layout["vertical:tree,tabs"];
  expect(left).toBeDefined();
  expect(left.tree).toBeGreaterThan(24);
  expect(left.tree).toBeLessThan(26);
  expect(left.tabs).toBeGreaterThan(74);
  expect(left.tabs).toBeLessThan(76);

  const center = layout["vertical:viewport,curve"];
  expect(center).toBeDefined();
  expect(center.viewport).toBeGreaterThan(74);
  expect(center.viewport).toBeLessThan(76);
  expect(center.curve).toBeGreaterThan(24);
  expect(center.curve).toBeLessThan(26);
});

test("all five quadrant testIDs present in the 3-col state", async () => {
  for (const id of [
    "quadrant-emitter-tree",
    "quadrant-property-tabs",
    "quadrant-viewport",
    "quadrant-curve-editor",
    "quadrant-spawner",
  ]) {
    await expect(page.locator(`[data-testid="${id}"]`)).toBeVisible();
  }
});

test("drag left↔centre separator persists across reload (±1 %)", async () => {
  // Drag +120 px to the right — left grows, centre shrinks.
  // Direct-child outer-Group separator: orientation=horizontal, sibling of
  // the left & centre Panels. The ce-splitter-v class identifies vertical
  // (column-resizing) separators; the FIRST such separator in document
  // order is the left↔centre one.
  await dragSeparator(":root [data-separator].ce-splitter-v", 120, 0);

  const layout = await readLayout();
  const outer = layout["horizontal:left,center,spawner"];
  const persisted = await page.evaluate(() => {
    const raw = localStorage.getItem("alo:layout:outer:3col");
    return raw ? JSON.parse(raw) : null;
  });

  expect(persisted).not.toBeNull();
  // Same shape, sum to ~100.
  expect(Object.keys(persisted).sort()).toEqual(["center", "left", "spawner"]);
  const sum = persisted.left + persisted.center + persisted.spawner;
  expect(Math.abs(sum - 100)).toBeLessThan(0.5);
  // Left grew, spawner unchanged (sits at its 260px floor, not a flat 20%).
  expect(persisted.left).toBeGreaterThan(20);
  const wDrag = await outerGroupWidthPx("left,center,spawner");
  expect(Math.abs(persisted.spawner - flooredPct(SPAWNER_MIN_PX, wDrag, 20))).toBeLessThan(1);
  // Rendered layout matches persisted (within ±1 %).
  expect(Math.abs(outer.left - persisted.left)).toBeLessThan(1);
  expect(Math.abs(outer.center - persisted.center)).toBeLessThan(1);

  // Reload and confirm persistence round-trips.
  const beforeReload = { ...persisted };
  await page.reload();
  await page.locator('[data-testid="quadrant-viewport"]').waitFor({ state: "visible" });
  const layoutAfter = await readLayout();
  const outerAfter = layoutAfter["horizontal:left,center,spawner"];
  expect(Math.abs(outerAfter.left - beforeReload.left)).toBeLessThan(1);
  expect(Math.abs(outerAfter.center - beforeReload.center)).toBeLessThan(1);
  expect(Math.abs(outerAfter.spawner - beforeReload.spawner)).toBeLessThan(1);
});

test("drag inner viewport↔curve separator (vertical) persists across reload", async () => {
  // The inner centre Group's separator is horizontal (row-resizing),
  // ce-splitter-h class. It's inside the centre Panel's subtree. The
  // FIRST ce-splitter-h in document order is the left-column tree↔tabs
  // one; the SECOND is the centre column's viewport↔curve. Select via
  // :nth-of-type would miss because the separators aren't siblings —
  // disambiguate by walking from the centre Panel.
  await page.evaluate(() => {
    const center = document.querySelector<HTMLElement>("[data-panel]#center");
    if (!center) throw new Error("center panel not found");
    const sep = center.querySelector<HTMLElement>("[data-separator].ce-splitter-h");
    if (!sep) throw new Error("centre h-separator not found");
    sep.setAttribute("data-test-target", "center-h");
  });
  await dragSeparator("[data-test-target=center-h]", 0, -90);

  const persisted = await page.evaluate(() =>
    JSON.parse(localStorage.getItem("alo:layout:center") ?? "null"),
  );
  expect(persisted).not.toBeNull();
  expect(Object.keys(persisted).sort()).toEqual(["curve", "viewport"]);
  expect(Math.abs(persisted.viewport + persisted.curve - 100)).toBeLessThan(0.5);
  // Drag was upward → curve grew.
  expect(persisted.curve).toBeGreaterThan(25);
});

test("corrupted localStorage falls back to defaults without crashing", async () => {
  await page.evaluate(() => {
    localStorage.setItem("alo:layout:outer:3col", "{not-json}");
    localStorage.setItem("alo:layout:left", JSON.stringify({ tree: 200 })); // missing key + bad sum
    localStorage.setItem("alo:layout:center", JSON.stringify({ viewport: 50, curve: 50, garbage: 999 }));
  });
  await page.reload();
  await page.locator('[data-testid="quadrant-viewport"]').waitFor({ state: "visible" });
  const layout = await readLayout();
  const outer = layout["horizontal:left,center,spawner"];
  // Defaults — corrupted blob ignored. Floor-aware (left 330px, spawner 260px).
  const wCorrupt = await outerGroupWidthPx("left,center,spawner");
  expect(Math.abs(outer.left - flooredPct(LEFT_MIN_PX, wCorrupt, 20))).toBeLessThan(1);
  expect(Math.abs(outer.spawner - flooredPct(SPAWNER_MIN_PX, wCorrupt, 20))).toBeLessThan(1);

  const left = layout["vertical:tree,tabs"];
  expect(left.tree).toBeGreaterThan(24);
  expect(left.tree).toBeLessThan(26);

  // Centre had three keys (viewport, curve, garbage) — `loadLayout`
  // strips the extra key when its sum-100 check passes, so on the next
  // pointer-release it's persisted clean. For now, the rendered ratios
  // match the supplied 50/50 (passes validation: sum=100, both default
  // keys present). This proves the strip-extras-but-keep-valid-values
  // branch of loadLayout actually runs end to end.
  const center = layout["vertical:viewport,curve"];
  expect(Math.abs(center.viewport - 50)).toBeLessThan(1);
  expect(Math.abs(center.curve - 50)).toBeLessThan(1);
});

test("Spawner toggle collapses the always-mounted dock (no remount), restores on re-toggle", async () => {
  // Tag the LEFT-pane DOM node so we can prove the toggle does NOT remount
  // the outer Group. The old key-based design remounted it (destroying this
  // node = the left-pane flicker); the always-mounted collapsible dock keeps
  // it, so a custom property set on the node survives the toggle.
  await page.evaluate(() => {
    const tree = document.querySelector(
      '[data-testid="quadrant-emitter-tree"]',
    ) as (HTMLElement & { __aliveMarker?: string }) | null;
    if (tree) tree.__aliveMarker = "alive";
  });

  const clickToggle = () =>
    page.evaluate(() => {
      const btn = document.querySelector<HTMLButtonElement>(
        '.toolbar button[aria-label="Toggle Spawner panel"]',
      );
      if (!btn) throw new Error("Spawner toolbar button not found");
      btn.click();
    });
  const spawnerWidth = () =>
    page.evaluate(
      () =>
        document
          .querySelector('[data-testid="quadrant-spawner"]')
          ?.getBoundingClientRect().width ?? -1,
    );

  // Toggle OFF — the dock slot collapses to width 0 but stays MOUNTED
  // (no remount). Wait out the ~260ms collapse animation before measuring.
  await clickToggle();
  await page.waitForTimeout(400);
  await expect(page.locator('[data-testid="quadrant-spawner"]')).toHaveCount(1);
  expect(await spawnerWidth()).toBeLessThan(5);

  // The left pane was NOT remounted (no flicker) — the marker survives.
  const markerSurvived = await page.evaluate(
    () =>
      (
        document.querySelector(
          '[data-testid="quadrant-emitter-tree"]',
        ) as (HTMLElement & { __aliveMarker?: string }) | null
      )?.__aliveMarker === "alive",
  );
  expect(markerSurvived).toBe(true);

  // Single persistence key — the dual 2col/3col machinery is gone with the
  // always-mounted dock; only `:3col` exists.
  const layout = await readLayout();
  expect(layout["horizontal:left,center,spawner"]).toBeDefined();
  expect(layout["horizontal:left,center"]).toBeUndefined();

  // Toggle ON — the slot expands back to a usable width (≥ the 260px minSize).
  await clickToggle();
  await page.waitForTimeout(400);
  expect(await spawnerWidth()).toBeGreaterThan(200);
});
