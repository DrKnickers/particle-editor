// Phase 3 Screen 8 Batch 3 — File-ops Playwright suite.
//
// Coverage:
//   1. File → New on a clean system dispatches file/new with no
//      SaveChangesPrompt modal (assert dirty/changed arrives with
//      `dirty:false`).
//   2. File → New on a dirty system shows the SaveChangesPrompt
//      (pre-seed dirty via an engine setter; click New; assert modal
//      is in the DOM).
//   3. file/save with a pre-seeded path clears dirty and the snapshot
//      carries the new currentFilePath.
//   4. document.title reflects dirty + currentFilePath.
//
// Notes:
//   - We can't pop the native GetSaveFileNameW picker from Playwright,
//     so the save tests pass `path` in params to bypass the dialog.
//     The native handler treats this branch identically to the
//     post-picker flow (path is committed, dirty cleared, etc.).
//   - The save-changes prompt is a Radix Modal mounted alongside the
//     About / Rescale modals. Its <Modal> uses role="dialog" — selector
//     hygiene per: filter by visible "Save changes?" title text.
//   - We pre-clean the file state at the start of each test via
//     file/new, so leftover dirty / currentFilePath from prior tests
//     don't bleed.

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

test.beforeEach(async () => {
  // Clean slate: every test starts with the editor in a known
  // dirty:false / currentFilePath:null state.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "file/new", params: {} });
  });
});

// ── 1. File → New on a clean system fires file/new without a prompt ─────────

test("File → New on a clean system fires file/new (no prompt)", async () => {
  // Subscribe to dirty/changed; on a clean→clean call we still expect
  // SetDirty(false) to be a no-op (debounced). But the bridge
  // response itself proves the round-trip happened, so we assert on
  // that + the absence of the SaveChangesPrompt in the DOM.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const r = await b.request({ kind: "file/new", params: {} });
    const snap = await b.request({
      kind: "engine/state/snapshot",
      params: {},
    });
    return {
      response: r,
      currentFilePath: snap.currentFilePath,
      dirty: snap.dirty,
    };
  });
  expect(result.response).toEqual({});
  expect(result.currentFilePath).toBeNull();
  expect(result.dirty).toBe(false);

  // The save-changes prompt should NOT be present in the DOM.
  const hasPrompt = await page
    .locator('text="Save changes?"')
    .count();
  expect(hasPrompt).toBe(0);
});

// ── 2. File → New on a dirty system shows the Save Changes prompt ───────────

test("File → New on a dirty system shows the Save Changes prompt", async () => {
  // Pre-seed dirty via an engine setter. ground-z is a safe choice —
  // value-equivalent to default doesn't matter, the host marks dirty
  // on any setter call.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "engine/set/ground-z",
      params: { z: 42 },
    });
  });

  // Verify dirty is now true.
  const dirty = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.dirty;
  });
  expect(dirty).toBe(true);

  // Open File → New via DOM. Close any open menus first.
  await page.keyboard.press("Escape").catch(() => {});
  const fileTrigger = page.locator('[role="menubar"] >> text=File').first();
  await fileTrigger.click();
  await page.waitForSelector('[role="menu"]', { timeout: 2000 });
  await page
    .locator('[role="menuitem"]:has-text("New")')
    .first()
    .click();

  // The SaveChangesPrompt modal should now be mounted. Title-text
  // selector — the modal renders the literal "Save changes?" string.
  await page.waitForSelector('text="Save changes?"', { timeout: 2000 });
  const promptVisible = await page
    .locator('text="Save changes?"')
    .isVisible();
  expect(promptVisible).toBe(true);

  // Click Cancel to dismiss and leave the next test with a clean DOM.
  await page
    .locator('[role="dialog"][data-state="open"]')
    .getByRole("button", { name: "Cancel" })
    .click();
  await page
    .waitForSelector('text="Save changes?"', { state: "detached", timeout: 2000 })
    .catch(() => {});
});

// ── 3. file/save with a pre-seeded path clears dirty ────────────────────────

test("File → Save (with pre-seeded path) commits the path and clears dirty", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    // Dirty the editor.
    await b.request({ kind: "engine/set/ground-z", params: { z: 12 } });
    const beforeSnap = await b.request({
      kind: "engine/state/snapshot",
      params: {},
    });
    // Save with explicit path — bypasses the native picker so the test
    // doesn't hang on user input. The C++ handler treats this branch
    // identically to the post-picker flow.
    const saveR = await b.request({
      kind: "file/save",
      params: { path: "C:/Temp/file-ops-spec.alo" },
    });
    const afterSnap = await b.request({
      kind: "engine/state/snapshot",
      params: {},
    });
    return {
      beforeDirty: beforeSnap.dirty,
      saveR,
      currentFilePath: afterSnap.currentFilePath,
      dirty: afterSnap.dirty,
    };
  });
  expect(result.beforeDirty).toBe(true);
  expect(result.saveR.ok).toBe(true);
  expect(result.currentFilePath).toBe("C:/Temp/file-ops-spec.alo");
  expect(result.dirty).toBe(false);
});

// ── 4. document.title reflects dirty + currentFilePath ─────────────────────

test("Window title reflects dirty + currentFilePath", async () => {
  // Pre-seed a path so the title's basename branch is exercised.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "file/save",
      params: { path: "C:/Temp/title-test.alo" },
    });
  });

  // Give React a tick to react to the snapshot event + update title.
  await page.waitForFunction(
    () => /title-test\.alo/.test(document.title),
    null,
    { timeout: 3000 },
  );

  // Clean + named — exact match (NOT toContain: "Particle Editor" is a
  // substring of the old "AloParticleEditor", so contains-checks can't
  // prove the rebrand).
  const cleanTitle = await page.title();
  expect(cleanTitle).toBe("title-test.alo — Particle Editor");

  // Now mutate and assert the ● prefix appears.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/ground-z", params: { z: 5 } });
  });
  await page.waitForFunction(
    () => document.title.startsWith("● "),
    null,
    { timeout: 3000 },
  );
  const dirtyTitle = await page.title();
  expect(dirtyTitle).toBe("● title-test.alo — Particle Editor");
});

// ── 5. Untitled state: file/new resets the title to the placeholder ────────

test("Window title shows Untitled.alo after file/new", async () => {
  // Establish a NAMED doc first so file/new has a real named→untitled
  // reset to perform. (The spec-level beforeEach already file/new's, so
  // without naming the doc here the assertion would pass vacuously on the
  // pre-existing untitled state.)
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "file/save",
      params: { path: "C:/Temp/untitled-reset.alo" },
    });
  });
  await page.waitForFunction(
    () => /untitled-reset\.alo/.test(document.title),
    null,
    { timeout: 3000 },
  );

  // file/new clears the current path → the title returns to the
  // Untitled placeholder.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "file/new", params: {} });
  });
  await page.waitForFunction(
    () => document.title === "Untitled.alo — Particle Editor",
    null,
    { timeout: 3000 },
  );
  expect(await page.title()).toBe("Untitled.alo — Particle Editor");
});
