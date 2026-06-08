// Phase 3 Screen 7 contract tests: primitive gallery smoke tests.
//
// Exercises the ?demo=primitives gallery route inside the native WebView2
// host. Uses the same CDP-attach harness as sibling specs (toolbar.spec.ts,
// background-picker.spec.ts). The gallery supplies its own static fixture
// data — no bridge round-trips from primitive components.
//
// To reach the demo route the test navigates to ?demo=primitives. The native
// host's WebView2 origin is https://app.local and the built dist/index.html
// loads as file:/// in debug mode, so we use page.evaluate to update
// window.location.search and wait for the gallery to appear.

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
    { timeout: 15_000 }
  );

  // Navigate to the demo route. Replacing the query param causes a React
  // re-render without a full page reload because Vite's SPA build serves
  // the same index.html for any path. App.tsx reads URLSearchParams once
  // at module evaluation time (DEMO_PARAM const), so we need a real
  // navigation. Use history.replaceState + force-remount via page.reload
  // is not needed — the const is at module scope. Instead, navigate to
  // the URL with the query param which triggers a proper reload.
  await page.evaluate(() => {
    window.location.href = window.location.href.split("?")[0] + "?demo=primitives";
  });

  // Wait for the gallery header to appear.
  await page.waitForSelector('text="Primitives gallery"', { timeout: 10_000 });
});

test.afterAll(async () => {
  // Restore the normal app route for subsequent test files.
  await page.evaluate(() => {
    window.location.href = window.location.href.split("?")[0];
  }).catch(() => {});
  await browser?.close();
});

// ── 1. Gallery page loads ────────────────────────────────────────────────────

test("?demo=primitives renders the gallery with all four sections", async () => {
  const sections = await page.evaluate(() => {
    return Array.from(document.querySelectorAll("h2")).map(
      (h) => h.textContent?.trim()
    );
  });
  expect(sections).toContain("Spinner");
  expect(sections).toContain("ColorButton");
  expect(sections).toContain("TexturePalette");
  expect(sections).toContain("RandomParam");
});

// ── 2. Spinner: scroll-wheel commits a new value ─────────────────────────────

test("Spinner: scroll-wheel on the input changes the displayed value", async () => {
  const result = await page.evaluate(async () => {
    // Find the first Spinner input in the gallery.
    const input = document.querySelector<HTMLInputElement>(
      'input[type="text"][aria-label="Demo spinner 1"]'
    );
    if (!input) return { found: false, before: "", after: "" };
    const before = input.value;
    // Dispatch a wheel event (deltaY < 0 = scroll up = increment).
    input.dispatchEvent(
      new WheelEvent("wheel", { bubbles: true, cancelable: true, deltaY: -120 })
    );
    await new Promise((r) => setTimeout(r, 50));
    return { found: true, before, after: input.value };
  });
  expect(result.found).toBe(true);
  // After scroll-wheel up, value should have incremented.
  expect(Number(result.after)).toBeGreaterThan(Number(result.before));
});

// ── 3. ColorButton: opens popover ───────────────────────────────────────────

test("ColorButton: clicking the swatch opens the color picker popover", async () => {
  const result = await page.evaluate(async () => {
    const btn = document.querySelector<HTMLButtonElement>(
      'button[aria-label="Demo color 1"]'
    );
    if (!btn) return { found: false, popoverVisible: false };
    btn.click();
    await new Promise((r) => setTimeout(r, 200));
    // The popover content contains "Basic colors" text.
    const popover = document.body.querySelector('[data-radix-popper-content-wrapper]');
    return { found: true, popoverVisible: !!popover };
  });
  expect(result.found).toBe(true);
  expect(result.popoverVisible).toBe(true);
});

// ── 4. TexturePalette: right-click opens context menu ───────────────────────

test("TexturePalette: right-click opens context menu with Browse/Clear/Reveal", async () => {
  // Close any open popover first.
  await page.evaluate(() => { document.body.click(); });
  await page.waitForTimeout(100);

  const result = await page.evaluate(async () => {
    // Find the first texture cell (role=option) in the palette.
    const cell = document.querySelector<HTMLButtonElement>(
      '[role="listbox"] [role="option"]'
    );
    if (!cell) return { found: false, menuItems: [] as string[] };
    // Dispatch contextmenu event.
    cell.dispatchEvent(
      new MouseEvent("contextmenu", { bubbles: true, cancelable: true })
    );
    await new Promise((r) => setTimeout(r, 200));
    // Radix portals the context menu into the body.
    const items = Array.from(
      document.querySelectorAll('[role="menuitem"]')
    ).map((el) => el.textContent?.trim() ?? "");
    return { found: true, menuItems: items };
  });
  expect(result.found).toBe(true);
  expect(result.menuItems).toContain("Browse for file…");
  expect(result.menuItems).toContain("Clear");
  expect(result.menuItems).toContain("Open texture folder");
});

// ── 5. RandomParam: mode renders correct spinner count ───────────────────────

test("RandomParam: Normal mode section shows two spinners with µ and σ labels", async () => {
  const result = await page.evaluate(() => {
    // The "Starts Normal" section has a RandomParam with mode=Normal.
    // Look for the µ and σ labels which only appear in Normal mode.
    const muLabels = Array.from(document.querySelectorAll("span")).filter(
      (el) => el.textContent?.trim() === "µ"
    );
    const sigmaLabels = Array.from(document.querySelectorAll("span")).filter(
      (el) => el.textContent?.trim() === "σ"
    );
    return { mu: muLabels.length, sigma: sigmaLabels.length };
  });
  // At least one RandomParam is in Normal mode (the third demo instance).
  expect(result.mu).toBeGreaterThanOrEqual(1);
  expect(result.sigma).toBeGreaterThanOrEqual(1);
});
