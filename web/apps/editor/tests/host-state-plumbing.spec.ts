// host-state plumbing — Playwright specs that exercise the three
// forward-deferred handlers now activated: file/save (real disk
// write), file/open (real disk read + ParticleSystem replace), and
// engine/action/rescale-system (real emitter mutation + tree-changed
// event).
//
// These tests rely on `C:/Temp/` existing on the build host (already
// the case — file-ops.spec.ts test 3 writes there). They use unique
// filenames per test run so parallel CI runs don't trample each other.

import * as fs from "node:fs";
import * as path from "node:path";
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
  // Reset to a clean ParticleSystem (one root emitter) between tests.
  // file/new now actually replaces the host-owned system rather than
  // just clearing bookkeeping, so this is load-bearing for the
  // rescale spec (it needs a non-empty emitter list).
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "file/new", params: {} });
  });
});

// ── 1. Save round-trip writes a real file on disk ──────────────────────────

test("file/save with a path writes a non-zero-byte .alo to disk", async () => {
  const filePath = `C:/Temp/host-state-plumbing-save-${Date.now()}.alo`;
  // Mutate an engine setter so the save isn't on a "never touched"
  // system. (Not strictly necessary — even a fresh system serialises
  // to a non-empty file because of headers + the single root emitter
  // — but it's a more realistic exercise.)
  const result = await page.evaluate(async (p) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/ground-z", params: { z: 17 } });
    const r = await b.request({ kind: "file/save", params: { path: p } });
    return r;
  }, filePath);

  expect(result.ok).toBe(true);
  // The native handler returns `{ ok: true, path: <normalised> }`.
  // The path string may be returned with backslashes; just verify the
  // file actually landed on disk at the requested location.
  const stat = fs.statSync(filePath);
  expect(stat.size).toBeGreaterThan(0);

  // Clean up — keep the worktree tidy across runs.
  try { fs.unlinkSync(filePath); } catch { /* best-effort */ }
});

// ── 2. Open round-trip reads back the saved file ───────────────────────────

test("file/open after file/save reads the file back; snapshot reflects the path", async () => {
  const filePath = `C:/Temp/host-state-plumbing-open-${Date.now()}.alo`;

  // Save first so we have something to open.
  await page.evaluate(async (p) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "engine/set/ground-z", params: { z: 23 } });
    await b.request({ kind: "file/save", params: { path: p } });
    // Reset back to "untitled" so the open path is the only thing
    // updating currentFilePath.
    await b.request({ kind: "file/new", params: {} });
  }, filePath);

  const result = await page.evaluate(async (p) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const r = await b.request({ kind: "file/open", params: { path: p } });
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { r, currentFilePath: snap.currentFilePath, dirty: snap.dirty };
  }, filePath);

  expect(result.r.ok).toBe(true);
  // Compare paths case-insensitively + with normalized separators —
  // Windows is case-insensitive on filesystem paths, and the native
  // OPENFILENAMEW round-trip may flip separator style.
  const normalize = (s: string) => s.replace(/\\/g, "/").toLowerCase();
  expect(normalize(result.currentFilePath as string)).toBe(normalize(filePath));
  expect(result.dirty).toBe(false);

  // Clean up.
  try { fs.unlinkSync(filePath); } catch { /* best-effort */ }
});

// ── 3. Rescale fires emitters/tree/changed ─────────────────────────────────

test("engine/action/rescale-system fires emitters/tree/changed", async () => {
  // Subscribe BEFORE firing the action so the event isn't missed.
  await page.evaluate(() => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const w = window as any;
    if (w.__rescaleTreeUnsub) w.__rescaleTreeUnsub();
    w.__rescaleTreeEvents = 0;
    w.__rescaleTreeUnsub = w.bridge.on("emitters/tree/changed", () => {
      w.__rescaleTreeEvents += 1;
    });
  });

  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "engine/action/rescale-system",
      params: { durationScalePercent: 200, sizeScalePercent: 100 },
    });
  });

  // Give the event channel a tick to deliver.
  await page.waitForTimeout(150);

  const count = await page.evaluate(
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    () => (window as any).__rescaleTreeEvents as number,
  );
  expect(count).toBeGreaterThanOrEqual(1);
});
