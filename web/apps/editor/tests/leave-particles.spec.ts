// leave-particles round-trip spec (Task 2.7). Drives the new
// `engine/set/leave-particles` bridge surface against the live native
// host and verifies the next snapshot reflects the mutated value.
//
// Restores the original value at the end so the spec is order-
// independent within the native-tests run.

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
    () => typeof (window as unknown as { bridge?: unknown }).bridge !== "undefined",
    null,
    { timeout: 15_000 },
  );
});

test.afterAll(async () => {
  await browser?.close();
});

test("engine/set/leave-particles round-trips through snapshot", async () => {
  const original = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.leaveParticles as boolean;
  });

  const after = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "engine/set/leave-particles",
      params: { enabled: false },
    });
    const s = await b.request({ kind: "engine/state/snapshot", params: {} });
    return s.leaveParticles as boolean;
  });
  expect(after).toBe(false);

  // Restore so the spec doesn't bleed into later runs.
  await page.evaluate(async (orig) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    await b.request({
      kind: "engine/set/leave-particles",
      params: { enabled: orig },
    });
  }, original);
});
