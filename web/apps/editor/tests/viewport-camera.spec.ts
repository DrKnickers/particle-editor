// viewport interaction — Playwright spec covering the
// `engine/set/camera` bridge path. The C++ viewport handler at
// src/host/HostWindow_Viewport.cpp `ViewportWndProc` mutates the camera via
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

import { test, expect, type Page } from "./helpers/cdp";

let page: Page;

test.beforeAll(async ({ cdpPage }) => {
  page = cdpPage;
});

test("engine/set/camera round-trips through the engine snapshot", async () => {
  const result = await page.evaluate(async () => {
    const b = window.bridge!;

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
  const cam = result.camera;
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

test("engine/set/camera refuses a degenerate camera and leaves the engine camera untouched", async () => {
  // A missing field used to become (0,0,0), and a coincident
  // eye/target or a zero/view-parallel up vector reached Engine::SetCamera,
  // whose LookAt then filled the view matrix with NaN. The host now answers
  // ok:false (src/host/CameraParams.h) and keeps the previous camera.
  const result = await page.evaluate(async () => {
    const b = window.bridge!;
    await b.request({
      kind: "engine/set/camera",
      params: { position: [10, 0, 0], target: [0, 0, 0], up: [0, 0, 1] },
    });

    const bad = [
      { position: [5, 5, 5], target: [5, 5, 5], up: [0, 0, 1] },   // coincident
      { position: [10, 0, 0], target: [0, 0, 0], up: [0, 0, 0] },  // zero up
      { position: [0, 0, 10], target: [0, 0, 0], up: [0, 0, 1] },  // up parallel to view
      { target: [0, 0, 0], up: [0, 0, 1] },                        // missing position
    ];
    const rejected: boolean[] = [];
    for (const params of bad) {
      try {
        // Deliberately malformed CameraDto — cast past the schema.
        await b.request({ kind: "engine/set/camera", params: params as never });
        rejected.push(false);
      } catch {
        rejected.push(true);
      }
    }
    const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { rejected, cam: snap.camera };
  });

  expect(result.rejected).toEqual([true, true, true, true]);
  const eps = 1e-4;
  expect(Math.abs(result.cam.position[0] - 10)).toBeLessThan(eps);
  expect(Math.abs(result.cam.position[1])).toBeLessThan(eps);
  expect(Math.abs(result.cam.position[2])).toBeLessThan(eps);
  expect(result.cam.up[2]).toBeGreaterThan(0.9);
});
