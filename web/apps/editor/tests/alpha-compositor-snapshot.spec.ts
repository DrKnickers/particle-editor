// AlphaCompositor snapshot regression.
//
// Pins the contract that the `viewport/capture-snapshot` bridge command
// continues to produce a valid base64 PNG after the per-frame
// `lastRawDib` cache was deferred. The host runs in the
// WS_EX_LAYERED popup architecture under
// the default Playwright config, so this spec exercises exactly the
// path that lost its cache: `Composite()` skips the cache copy, and
// `CaptureSnapshotPng()` must do its own `GetRenderTargetData` +
// `LockRect` + GDI+ PNG encode on demand.
//
// What this spec proves:
//
//   1. The first snapshot after host boot returns a non-empty
//      imageBase64 + non-zero dimensions. Pre-refactor this was served
//      from the per-frame cache; post-refactor it's served from a
//      fresh readback. Failure mode would be either `false` (returned
//      empty imageBase64) — which the prior code path simulated via
//      `lastRawDib.empty()` — or a malformed payload.
//
//   2. Two consecutive snapshots both succeed. Validates that the new
//      readback path doesn't leave the SYSTEMMEM surface in a bad
//      state (LockRect/UnlockRect pairing intact).
//
//   3. After a `layout/viewport-rect` mutation, the snapshot
//      dimensions update accordingly. Confirms the new readback
//      reflects the *current* RT size, not stale dims from a prior
//      Resize.
//
// Out-of-scope here: the React Modal's frosted-glass <img> render —
// that's `dialogs.spec.ts` territory. This spec is purely the bridge
// contract.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";

const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

type SnapshotDto = { imageBase64: string; w: number; h: number };

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

// Decode a base64 JPEG in the page and report how much it actually VARIES.
//
// Length + magic-number + dimension assertions (which is all this file used to
// do) are satisfied by a correctly-sized solid-black frame — exactly what a
// broken readback produces (2026-07 audit, an-audit-finding). Counting distinct luminance
// buckets separates "the compositor read back real pixels" from "the encoder
// produced a valid image of nothing".
// Count DISTINCT colours in a base64 JPEG, decoded in the page.
//
// Length + magic-number + dimension assertions (all this file used to do) are
// satisfied by a correctly-sized solid-black frame — exactly what a broken
// readback produces (2026-07 audit, an-audit-finding).
//
// Distinct colours rather than luminance SPREAD, deliberately. The first
// attempt bucketed by brightness and failed on a healthy snapshot: the boot
// scene is a dark purple gradient spanning luminance 0-18, so coarse
// brightness buckets collapsed real content into two. Colour count makes no
// assumption about how bright the scene happens to be.
async function distinctColors(page: Page, base64: string): Promise<number> {
  return page.evaluate(async (b64) => {
    const img = new Image();
    await new Promise<void>((resolve, reject) => {
      img.onload = () => resolve();
      img.onerror = () => reject(new Error("snapshot did not decode as an image"));
      img.src = `data:image/jpeg;base64,${b64}`;
    });
    const c = document.createElement("canvas");
    c.width = img.naturalWidth;
    c.height = img.naturalHeight;
    const ctx = c.getContext("2d");
    if (!ctx) throw new Error("no 2d context");
    ctx.drawImage(img, 0, 0);
    const { data } = ctx.getImageData(0, 0, c.width, c.height);
    const seen = new Set<number>();
    for (let i = 0; i < data.length; i += 4) {
      seen.add((data[i]! << 16) | (data[i + 1]! << 8) | data[i + 2]!);
    }
    return seen.size;
  }, base64);
}

// A solid fill, JPEG-encoded at the same size, run through the SAME oracle.
// This is the negative control, and it runs every time: it proves the check
// above can still tell a rendered scene from a blank one, rather than us
// trusting a threshold that was tuned once and never re-checked.
async function solidFillColors(page: Page, w: number, h: number): Promise<number> {
  const b64 = await page.evaluate(({ w, h }) => {
    const c = document.createElement("canvas");
    c.width = w; c.height = h;
    const ctx = c.getContext("2d")!;
    ctx.fillStyle = "#000";
    ctx.fillRect(0, 0, w, h);
    return c.toDataURL("image/jpeg").split(",")[1]!;
  }, { w, h });
  return distinctColors(page, b64);
}


// NOTE: the payload is a JPEG, not a PNG — the backdrop is shown blurred behind
// a dialog, so lossy is invisible and far cheaper. The test NAME said PNG while
// the assertion below required a JPEG magic number (audit an-audit-finding); renamed to match
// what it actually checks.
test("first viewport/capture-snapshot after boot returns a valid, non-blank JPEG (cache-flag-off path)", async () => {
  // Seed a known viewport size so the snapshot crop has deterministic
  // dimensions to assert against. The layout broker dispatches this
  // through the host, which calls AlphaCompositor::Resize → renders →
  // SetSceneRect to match. The next Composite tick is what would have
  // populated lastRawDib under the old code; after the refactor that
  // step is a no-op in this architecture.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "layout/viewport-rect",
      params: { x: 0, y: 0, w: 1024, h: 768 },
    });
    await b.request({
      kind: "layout/scene-rect",
      params: { x: 0, y: 0, w: 1024, h: 768 },
    });
    // Let at least one Composite tick happen — gives the engine time
    // to write into offscreenRT so the snapshot has actual scene
    // content (not just cleared pixels). 50 ms is comfortably > one
    // frame at any FPS we care about.
    await new Promise((r) => setTimeout(r, 50));
    return (await b.request({
      kind: "viewport/capture-snapshot",
      params: {},
    })) as SnapshotDto;
  });

  // imageBase64 must be a non-trivial payload — the backdrop is a
  // JPEG (shown blurred, so lossy is invisible and far cheaper than PNG).
  // A JPEG starts with the SOI marker FF D8 FF, whose base64 prefix is
  // `/9j/` — a stable invariant. Assert on length AND prefix to catch both
  // "empty string" (false path) and "garbage bytes" (encoder failure).
  expect(result.imageBase64.length).toBeGreaterThan(100);
  expect(result.imageBase64.startsWith("/9j/")).toBe(true);
  // ...and it must contain an actual rendered scene. A solid-black frame of the
  // right size satisfies every assertion above (audit an-audit-finding); a real viewport
  // (ground, skydome, particles) spans many luminance levels.
  // Measured on a healthy boot snapshot: 26 distinct colours. A solid fill
  // decodes to a handful, so 8 leaves margin in both directions — and the
  // control below keeps that claim honest instead of assumed.
  const colors = await distinctColors(page, result.imageBase64);
  const blank = await solidFillColors(page, result.w, result.h);
  expect(blank).toBeLessThan(8);          // the oracle can still detect a blank frame
  expect(colors).toBeGreaterThanOrEqual(8);
  expect(colors).toBeGreaterThan(blank);  // ...and the real snapshot clears it
  // The backdrop snapshot is downscaled before encoding (min 2x, capped at a
  // 1024 long edge — it's blurred behind the dialog; see
  // AlphaCompositor::CaptureSnapshotPng). 1024x768 is under the cap, so it
  // takes the 2x path: 512x384.
  expect(result.w).toBe(512);
  expect(result.h).toBe(384);
});

test("two consecutive snapshots both succeed (readback path is re-entrant)", async () => {
  // Validates that GetRenderTargetData → LockRect → UnlockRect is
  // properly paired. A bug where UnlockRect was missed would leave
  // the SYSTEMMEM surface locked; the second LockRect would return
  // D3DERR_INVALIDCALL and CaptureSnapshotPng would return false →
  // imageBase64 would be the empty string per the host fall-through.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const first = (await b.request({
      kind: "viewport/capture-snapshot",
      params: {},
    })) as SnapshotDto;
    const second = (await b.request({
      kind: "viewport/capture-snapshot",
      params: {},
    })) as SnapshotDto;
    return { first, second };
  });

  expect(result.first.imageBase64.length).toBeGreaterThan(100);
  expect(result.second.imageBase64.length).toBeGreaterThan(100);
  // Both must report the same dims (no in-between Resize was issued).
  expect(result.second.w).toBe(result.first.w);
  expect(result.second.h).toBe(result.first.h);
});

test("snapshot dimensions follow viewport resize (readback uses current RT, not stale cache)", async () => {
  // After a viewport-rect dispatch, AlphaCompositor::Resize recreates
  // the offscreenRT + sysMemSurface at the new size. The pre-refactor
  // cache would have held the OLD-size buffer until the next Composite
  // refreshed it; the new readback path reads directly from the new
  // surfaces, so the snapshot must reflect the new dims immediately.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "layout/viewport-rect",
      params: { x: 0, y: 0, w: 800, h: 600 },
    });
    await b.request({
      kind: "layout/scene-rect",
      params: { x: 0, y: 0, w: 800, h: 600 },
    });
    await new Promise((r) => setTimeout(r, 50));
    const small = (await b.request({
      kind: "viewport/capture-snapshot",
      params: {},
    })) as SnapshotDto;

    await b.request({
      kind: "layout/viewport-rect",
      params: { x: 0, y: 0, w: 1600, h: 900 },
    });
    await b.request({
      kind: "layout/scene-rect",
      params: { x: 0, y: 0, w: 1600, h: 900 },
    });
    await new Promise((r) => setTimeout(r, 50));
    const large = (await b.request({
      kind: "viewport/capture-snapshot",
      params: {},
    })) as SnapshotDto;

    return { small, large };
  });

  // The modal backdrop snapshot is downscaled before encoding (min 2x,
  // capped at a 1024 long edge — it's blurred behind the dialog; see
  // AlphaCompositor::CaptureSnapshotPng). Both these captures are under the
  // cap, so each takes the 2x path with aspect preserved: 800x600 -> 400x300,
  // 1600x900 -> 800x450. The dims still CHANGE between them, which is what
  // this test actually guards (a fresh readback, not a stale cache).
  expect(result.small.w).toBe(400);
  expect(result.small.h).toBe(300);
  expect(result.large.w).toBe(800);
  expect(result.large.h).toBe(450);
  expect(result.small.imageBase64.startsWith("/9j/")).toBe(true);
  expect(result.large.imageBase64.startsWith("/9j/")).toBe(true);
});
