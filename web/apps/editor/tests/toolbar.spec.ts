// Particle Editor 2026 toolbar contract tests. The toolbar groups are
// file actions · playback · viewport toggles · Spawner · environment+theme.
// The three viewport toggles (ground / bloom / leave-particles) moved here
// from the deleted ViewportPill; Undo/Redo and Reload-* live in the menubar
// (see menu-bar.spec.ts). CDP-attach harness matches sibling app-shell.spec.ts.
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

test("Toolbar renders the 2026 button set", async () => {
  const labels = await page.evaluate(() => {
    return Array.from(document.querySelectorAll('.toolbar button[aria-label]'))
      .map((b) => b.getAttribute("aria-label"))
      .filter((l): l is string => l !== null);
  });
  // File group
  expect(labels).toEqual(expect.arrayContaining([
    "New",
    "Open",
    "Save",
    "Save As",
  ]));
  // Playback group — Pause/Play depending on current engine state.
  expect(labels.some((l) => /^(Pause|Play)$/.test(l))).toBe(true);
  expect(labels).toEqual(expect.arrayContaining([
    "Step",
    "Step 10",
    "Toggle Spawner panel",
  ]));
  // Viewport engine toggles (moved from the old ViewportPill).
  expect(labels).toEqual(expect.arrayContaining([
    "Show ground",
    "Toggle bloom",
    "Leave particles after instance death",
  ]));
  // (Theme switching moved out of the toolbar into Edit → Preferences.)
});

test("engine/set/paused mutates state and flips Play/Pause button", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const bridge = (window as any).bridge;
    await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
    await new Promise((r) => setTimeout(r, 150));
    const snap = await bridge.request({ kind: "engine/state/snapshot", params: {} });
    const btn = Array.from(document.querySelectorAll('.toolbar button[aria-label]'))
      .find((b) => /^(Pause|Play)$/.test(b.getAttribute("aria-label")!));
    return {
      paused: snap.paused,
      ariaLabel: btn?.getAttribute("aria-label") ?? null,
      ariaPressed: btn?.getAttribute("aria-pressed") ?? null,
    };
  });
  expect(result.paused).toBe(true);
  // When paused, the button shows "Play" (i.e. clicking it will resume).
  expect(result.ariaLabel).toBe("Play");
  // aria-pressed reflects "is playing" — false while paused.
  expect(result.ariaPressed).toBe("false");

  // Cleanup — un-pause for the next test.
  await page.evaluate(() => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    return (window as any).bridge.request({ kind: "engine/set/paused", params: { paused: false } });
  });
});

test("engine/action/step-frames is dispatched without error", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const bridge = (window as any).bridge;
    // Pause first so step-frames isn't a complete no-op host-side.
    await bridge.request({ kind: "engine/set/paused", params: { paused: true } });
    const r = await bridge.request({ kind: "engine/action/step-frames", params: { frames: 1 } });
    await bridge.request({ kind: "engine/set/paused", params: { paused: false } });
    return r;
  });
  expect(result).toEqual({});
});

test("Step 10 button is present, enabled, and clickable without error", async () => {
  // The shape of the dispatch (engine/action/step-frames with
  // frames=10) is verified by the Vitest unit test in
  // src/components/__tests__/Toolbar.test.tsx — we can't intercept
  // React's bridge prop from this Playwright harness because the
  // production bridge passed into the component tree is a different
  // instance from window.bridge under --test-host (expose.ts swaps in
  // a fresh TestHostBridge for the window slot). So here we just
  // verify the button renders, is enabled, and a real click completes
  // without throwing in the React handler.
  const btn = page.locator('.toolbar button[aria-label="Step 10"]');
  await expect(btn).toBeVisible();
  await expect(btn).toBeEnabled();
  await btn.click();
});

test("Spawner toggle button is present with default aria-pressed=true", async () => {
  // Default visible on first launch; the in-app localStorage may have
  // been mutated by prior runs, so we accept either pressed state but
  // verify the button exists with the planned aria-label.
  const info = await page.evaluate(() => {
    const btn = Array.from(document.querySelectorAll('.toolbar button[aria-label]'))
      .find((b) => b.getAttribute("aria-label") === "Toggle Spawner panel") as HTMLButtonElement | undefined;
    return {
      present: !!btn,
      text: btn?.textContent?.trim() ?? null,
      ariaPressed: btn?.getAttribute("aria-pressed") ?? null,
    };
  });
  expect(info.present).toBe(true);
  // Spawner is now an icon button (CirclePlus), not the old "Spawner" text.
  expect(info.text).toBe("");
  expect(info.ariaPressed).toMatch(/^(true|false)$/);
});

test("viewport toggles flip engine state via aria-pressed", async () => {
  // The three engine toggles that used to live in the floating
  // ViewportPill now live in the toolbar. Each reflects engine state via
  // aria-pressed and flips it on click; toggle back to leave state clean.
  for (const label of [
    "Show ground",
    "Toggle bloom",
    "Leave particles after instance death",
  ]) {
    const btn = page.locator(`.toolbar button[aria-label="${label}"]`);
    await expect(btn).toHaveAttribute("aria-pressed", /true|false/);
    const before = await btn.getAttribute("aria-pressed");
    await btn.click();
    await expect(btn).not.toHaveAttribute("aria-pressed", before ?? "");
    await btn.click(); // restore
  }
});
