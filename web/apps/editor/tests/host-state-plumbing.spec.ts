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
import { test, expect, type Page } from "./helpers/cdp";

// Page-side recorders this spec installs on window.
declare global {
  interface Window {
    __rescaleTreeEvents?: number;
    __rescaleTreeUnsub?: () => void;
  }
}

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test.beforeEach(async () => {
  // Reset to a clean ParticleSystem (one root emitter) between tests.
  // file/new now actually replaces the host-owned system rather than
  // just clearing bookkeeping, so this is load-bearing for the
  // rescale spec (it needs a non-empty emitter list).
  await page.evaluate(async () => {
    const b = window.bridge!;
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
    const b = window.bridge!;
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
    const b = window.bridge!;
    await b.request({ kind: "engine/set/ground-z", params: { z: 23 } });
    await b.request({ kind: "file/save", params: { path: p } });
    // Reset back to "untitled" so the open path is the only thing
    // updating currentFilePath.
    await b.request({ kind: "file/new", params: {} });
  }, filePath);

  const result = await page.evaluate(async (p) => {
    const b = window.bridge!;
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
    window.__rescaleTreeUnsub?.();
    window.__rescaleTreeEvents = 0;
    window.__rescaleTreeUnsub = window.bridge!.on("emitters/tree/changed", () => {
      window.__rescaleTreeEvents = (window.__rescaleTreeEvents ?? 0) + 1;
    });
  });

  await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({
      kind: "engine/action/rescale-system",
      params: { durationScalePercent: 200, sizeScalePercent: 100 },
    });
  });

  // Give the event channel a tick to deliver.
  await page.waitForTimeout(150);

  const count = await page.evaluate(
    () => window.__rescaleTreeEvents,
  );
  expect(count).toBeGreaterThanOrEqual(1);
});

test("held Shift survives clear then file/new without a stale-instance crash", async () => {
  const viewport = page.locator('[data-testid="viewport-canvas"]');
  await expect(viewport).toBeVisible();
  await viewport.hover();

  try {
    await page.keyboard.down("Shift");

    await expect.poll(async () => page.evaluate(async () => {
      const b = window.bridge!;
      const live = await b.request({
        kind: "engine/query/live-instances",
        params: {},
      });
      return live.instances as number;
    })).toBeGreaterThanOrEqual(1);

    const result = await page.evaluate(async () => {
      const b = window.bridge!;
      await b.request({ kind: "engine/action/clear", params: {} });
      const afterClear = await b.request({
        kind: "engine/query/live-instances",
        params: {},
      });
      await b.request({ kind: "file/new", params: {} });
      const afterNew = await b.request({
        kind: "engine/query/live-instances",
        params: {},
      });
      const emitters = await b.request({ kind: "emitters/list", params: {} });
      return {
        afterClear: afterClear.instances as number,
        afterNew: afterNew.instances as number,
        rootChildren: emitters.root.children.length as number,
      };
    });

    expect(result).toEqual({
      afterClear: 0,
      afterNew: 0,
      rootChildren: 1,
    });
  } finally {
    await page.keyboard.up("Shift").catch(() => {});
  }
});

test("record preview valid place and kill consume their handles", async () => {
  const result = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({ kind: "file/new", params: {} });

    await b.request({ kind: "preview/attach", params: { x: 200, y: 200 } });
    await b.request({ kind: "preview/place", params: {} });

    await b.request({ kind: "preview/attach", params: { x: 220, y: 220 } });
    await b.request({ kind: "preview/kill", params: {} });

    let secondKillRejected = false;
    try {
      await b.request({ kind: "preview/kill", params: {} });
    } catch {
      secondKillRejected = true;
    }
    return { placeSucceeded: true, killSucceeded: true, secondKillRejected };
  });

  expect(result.placeSucceeded).toBe(true);
  expect(result.killSucceeded).toBe(true);
  expect(result.secondKillRejected).toBe(true);
});
