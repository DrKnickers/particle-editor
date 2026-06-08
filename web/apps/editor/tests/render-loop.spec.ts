// render loop — Playwright spec that verifies the per-frame
// SpawnerDriver::Tick fires from real engine state (rather than the
// MockBridge in-memory counter). After this batch the host's main loop
// is PeekMessage idle-render and RenderD3D9 drives:
//
//   - SpawnerDriver::Tick(dt, particleSystem, engine)
//   - engine->Update / engine->Render
//   - spawner/active-count emit when Engine::GetNumInstances() changes
//
// The simplest end-to-end probe: configure a manual-mode spawner with a
// non-trivial burstSize, fire spawner/trigger, and observe at least one
// spawner/active-count event with count >= 1 arrive within ~2s of
// render-loop ticking. Render-loop specs inherently need time-based
// waits; that's a normal Playwright pattern.

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
  // Start from a clean ParticleSystem with one root emitter (so the
  // spawner has something to instantiate). file/new replaces the
  // host-owned system and notifies the engine via
  // Clear + OnParticleSystemChanged.
  await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({ kind: "file/new", params: {} });
  });
});

test("spawner/active-count event fires from real engine state when a burst is triggered", async () => {
  // 1. Subscribe to spawner/active-count from the page; collect every
  //    event into a window-scoped array so the C++ host can push at any
  //    cadence and we can poll the buffer afterwards.
  // 2. Pre-seed a burst-friendly spawner config (manual mode, burstSize 3).
  // 3. Fire spawner/trigger.
  // 4. Poll up to 2s for at least one event with count >= 1.
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const log: Array<{ count: number }> = [];
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const off = b.on("spawner/active-count", (e: any) => {
      log.push({ count: e.payload.count });
    });

    // Manual mode with burstSize 3, spacing 0 (all three fire on the
    // same tick after Trigger). MaxLifetimeSec 5 so they live long
    // enough for active-count to read non-zero on at least one frame.
    await b.request({
      kind: "spawner/start",
      params: {
        mode: "manual",
        enabled: false,
        burstSize: 3,
        spacingSec: 0,
        intervalSec: 0,
        position: [0, 0, 0],
        velocity: [0, 0, 0],
        maxLifetimeSec: 5,
        jitterPosition: [0, 0, 0],
        jitterVelocity: [0, 0, 0],
      },
    });

    await b.request({ kind: "spawner/trigger", params: {} });

    // Poll up to 2s for an event with count >= 1.
    const deadline = Date.now() + 2000;
    let observed = false;
    while (Date.now() < deadline) {
      if (log.some((e) => e.count >= 1)) {
        observed = true;
        break;
      }
      await new Promise((r) => setTimeout(r, 50));
    }
    off();
    return { observed, events: log };
  });

  expect(result.observed).toBe(true);
});
