// Contract tests: Edit → Preferences… (PreferencesDialog) against the
// real native bridge inside ParticleEditor.exe --test-host. Same
// CDP-attach harness as sibling specs (dialogs.spec.ts).
//
// What the specs cover:
//   1. The menu gesture mounts the modal and the Antialiasing select is
//      populated from the hardware-gated engine/query/msaa-levels
//      response (seeded disabled until the query resolves — the fix for
//      the past bug where a FAILED AA query silently collapsed the
//      select to "Off" and persisted it; the failure branch itself is
//      jsdom-covered in PreferencesDialog.test.tsx, since the native
//      host always answers the query).
//   2. Changing the Antialiasing level via the real <select> fires
//      engine/set/msaa-level, the engine applies it on the next render
//      frame (observed via engine/query/msaa-levels `current`), and the
//      choice persists across a dialog close/reopen (localStorage).
//   3. The Model-shadows toggle persists across reopen. There is no
//      host-side getter for engine/set/model-shadows (view-only, no DTO
//      field), so the round-trip assertion is persistence + reopened
//      checkbox state; the dispatch itself is jsdom-covered.
//
// Every test restores the state it mutates (mirrors the restore pattern
// in tools.spec.ts / background-picker.spec.ts).

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

// Radix Modal content (portalled). The BackgroundPicker/ToolPanel
// surfaces also use role="dialog" but carry no data-state — see the
// selector note in dialogs.spec.ts.
const RADIX_DIALOG = '[role="dialog"][data-state="open"]';

// Helper — open Edit → Preferences… and wait for the modal to mount.
async function openPreferences() {
  await page.keyboard.press("Escape").catch(() => {});
  await page.locator('[role="menubar"] >> text=Edit').first().click();
  await page.waitForSelector('[role="menu"]', { timeout: 2000 });
  await page.locator('[role="menuitem"]:has-text("Preferences")').first().click();
  await page.waitForSelector(RADIX_DIALOG, { timeout: 2000 });
  return page.locator(RADIX_DIALOG);
}

// Helper — close the modal via Escape and wait for the detach so the
// next gesture starts clean (dialogs.spec.ts precedent).
async function closePreferences() {
  await page.keyboard.press("Escape");
  await page.waitForSelector(RADIX_DIALOG, { state: "detached", timeout: 2000 });
}

// Helper — the engine's MSAA state through the same query the dialog uses.
async function queryMsaa(): Promise<{ levels: number[]; current: number }> {
  return page.evaluate(async () => {
    const bridge = (window as Window & { bridge?: {
      request: (req: { kind: string; params: unknown }) => Promise<unknown>;
    } }).bridge;
    if (!bridge) throw new Error("bridge missing");
    return await bridge.request({
      kind: "engine/query/msaa-levels",
      params: {},
    }) as { levels: number[]; current: number };
  });
}

// ── 1. Menu gesture + hardware-gated Antialiasing select ─────────────

test("Edit → Preferences opens; Antialiasing select is populated from engine/query/msaa-levels", async () => {
  const dialog = await openPreferences();

  // Rendering section controls present.
  const select = page.locator("#pref-msaa-level");
  await expect(select).toBeVisible({ timeout: 5_000 });
  // Enabled once the hardware query resolves (disabled while in-flight —
  // the seeded-null state).
  await expect(select).toBeEnabled({ timeout: 5_000 });
  await expect(dialog.locator("#pref-model-shadows")).toBeAttached();
  await expect(dialog.locator("#pref-soft-shadows")).toBeAttached();

  // The option list mirrors the engine's supported-levels response
  // (filtered to the valid {0,2,4,8} set), and the displayed value is
  // one of them.
  const { levels } = await queryMsaa();
  const validLevels = levels.filter((l) => l === 0 || l === 2 || l === 4 || l === 8);
  const optionValues = await select.locator("option").evaluateAll(
    (opts) => opts.map((o) => Number((o as HTMLOptionElement).value)),
  );
  expect(optionValues.sort((a, b) => a - b)).toEqual(validLevels.sort((a, b) => a - b));
  const displayed = Number(await select.inputValue());
  expect(validLevels).toContain(displayed);

  await closePreferences();
});

// ── 2. Antialiasing round-trip + persistence across reopen ───────────

test("changing Antialiasing fires engine/set/msaa-level (observed via query `current`) and persists across reopen", async () => {
  const before = await queryMsaa();
  const storedBefore = await page.evaluate(() => localStorage.getItem("alo:msaa-quality"));

  // Pick a target that differs from the engine's current level. "Off"
  // (0) always applies deterministically; when already Off, flip to the
  // highest supported real level instead.
  const realLevels = before.levels.filter((l) => l === 2 || l === 4 || l === 8);
  test.skip(
    before.current === 0 && realLevels.length === 0,
    "GPU reports no MSAA support and the engine is already at Off — nothing to flip",
  );
  const target = before.current !== 0 ? 0 : Math.max(...realLevels);

  // Restore in finally: a mid-test failure must not leak a mutated MSAA
  // level or persisted pref into later specs of the shared CDP session.
  try {
    const select = page.locator("#pref-msaa-level");
    await openPreferences();
    await expect(select).toBeEnabled({ timeout: 5_000 });
    await select.selectOption(String(target));

    // The setter marks the level dirty; the render thread applies it on
    // the next frame — poll the query's authoritative `current`.
    await expect
      .poll(async () => (await queryMsaa()).current, { timeout: 10_000 })
      .toBe(target);

    await closePreferences();

    // Reopen — the persisted choice (localStorage) seeds the select and
    // survives the reconcile against the fresh hardware query.
    await openPreferences();
    await expect(select).toBeEnabled({ timeout: 5_000 });
    await expect(select).toHaveValue(String(target));
  } finally {
    // Best-effort dialog dismiss (may or may not be open on failure),
    // then restore the original persisted value + engine level.
    await page.keyboard.press("Escape").catch(() => {});
    await page.evaluate(async (args: { stored: string | null; level: number }) => {
      if (args.stored === null) localStorage.removeItem("alo:msaa-quality");
      else localStorage.setItem("alo:msaa-quality", args.stored);
      const bridge = (window as Window & { bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      } }).bridge;
      await bridge!.request({
        kind: "engine/set/msaa-level",
        params: { level: args.level },
      });
    }, { stored: storedBefore, level: before.current });
    await expect
      .poll(async () => (await queryMsaa()).current, { timeout: 10_000 })
      .toBe(before.current);
  }
});

// ── 3. Model-shadows toggle persistence across reopen ────────────────

test("Model shadows toggle persists across dialog reopen (localStorage-backed)", async () => {
  const storedBefore = await page.evaluate(() => localStorage.getItem("alo:model-shadows"));

  await openPreferences();
  const checkbox = page.locator("#pref-model-shadows");
  await expect(checkbox).toBeAttached({ timeout: 5_000 });
  const wasChecked = await checkbox.isChecked();

  // Restore in finally: a mid-test failure must not leak the flipped
  // shadow pref/engine state into later specs of the shared CDP session.
  try {
    // The real <input> is visually hidden (sr-only) behind the styled box,
    // so an actionability-gated Playwright click refuses it — dispatch the
    // DOM click directly (background-picker.spec.ts evaluate-click
    // precedent). This still runs the React onChange commit path.
    await page.evaluate(() => {
      document.getElementById("pref-model-shadows")?.click();
    });
    await expect(checkbox).toBeChecked({ checked: !wasChecked });
    // Persisted as "1"/"0" (boolean-pref convention).
    await expect
      .poll(() => page.evaluate(() => localStorage.getItem("alo:model-shadows")), { timeout: 2_000 })
      .toBe(wasChecked ? "0" : "1");

    await closePreferences();

    // Reopen — the toggle state survives (localStorage seed).
    await openPreferences();
    await expect(checkbox).toBeChecked({ checked: !wasChecked });
  } finally {
    // Best-effort dialog dismiss, restore the raw stored value (so an
    // originally-unset key doesn't linger), and push the original state
    // to the engine directly — the UI path may be unavailable on failure.
    await page.keyboard.press("Escape").catch(() => {});
    await page.evaluate(async (args: { stored: string | null; enabled: boolean }) => {
      if (args.stored === null) localStorage.removeItem("alo:model-shadows");
      else localStorage.setItem("alo:model-shadows", args.stored);
      const bridge = (window as Window & { bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      } }).bridge;
      await bridge!.request({
        kind: "engine/set/model-shadows",
        params: { enabled: args.enabled },
      }).catch(() => {});
    }, { stored: storedBefore, enabled: wasChecked });
  }
});
