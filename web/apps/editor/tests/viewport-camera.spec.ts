// viewport interaction — Playwright spec covering the
// `engine/set/camera` bridge path. The C++ viewport handler at
// src/host/HostWindow.cpp `ViewportWndProc` mutates the camera via
// `engine->SetCamera` directly (it bypasses the dispatcher's setter
// ladder), so the actual mouse-drag path cannot be exercised from
// Playwright — Playwright drives WebView2 input, not the sibling
// D3D9 HWND that receives WM_LBUTTONDOWN / WM_MOUSEMOVE / etc.
//
// What this spec verifies instead: the underlying camera setter
// round-trips through the engine and the snapshot reports the new
// state. That's the same wiring the mouse handler depends on
// (Engine::SetCamera → next snapshot read). If this path regresses,
// the mouse handler is silently broken too.
//
// The schema uses lowercase keys (position / target / up — see
// web/packages/bridge-schema/src/index.ts CameraDto) and returns
// the camera under `state.camera` in `engine/state/snapshot`.

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

test("engine/set/camera round-trips through the engine snapshot", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // Pre-seed a known camera pose. The values are picked to avoid
    // colliding with the engine's default starting camera so the
    // round-trip is unambiguous.
    await b.request({
      kind: "engine/set/camera",
      params: {
        position: [10, 0, 0],
        target:   [0, 0, 0],
        up:       [0, 0, 1],
      },
    });

    // Read back via engine/state/snapshot. The snapshot is synchronous
    // (no need to wait for an event) because the dispatcher rebuilds
    // it on demand from the live engine state.
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    return snap;
  });

  // The snapshot returns a flat EngineStateDto (see BuildEngineStateSnapshot
  // in src/host/BridgeDispatcher.cpp). `camera` is a CameraDto with
  // lowercase keys.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const cam = (result as any).camera;
  const eps = 1e-4;

  expect(Math.abs(cam.position[0] - 10)).toBeLessThan(eps);
  expect(Math.abs(cam.position[1] -  0)).toBeLessThan(eps);
  expect(Math.abs(cam.position[2] -  0)).toBeLessThan(eps);

  expect(Math.abs(cam.target[0] - 0)).toBeLessThan(eps);
  expect(Math.abs(cam.target[1] - 0)).toBeLessThan(eps);
  expect(Math.abs(cam.target[2] - 0)).toBeLessThan(eps);

  // Up may be normalized by the engine — accept any positive-Z
  // unit-ish vector. We only assert sign + dominant axis.
  expect(cam.up[2]).toBeGreaterThan(0.9);
});
