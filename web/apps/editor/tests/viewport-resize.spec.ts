// Phase 4.1 Fix dispatch 4 — viewport resize smoke spec.
//
// Drives `layout/viewport-rect` with three different physical-pixel
// sizes in sequence (small → medium → large). After each, asks for
// `engine/state/snapshot` to confirm the host is still responsive
// (a hung or device-lost engine would fail to answer). The point is
// to exercise LayoutBroker::Apply's SetWindowPos + Engine::Reset
// chain — particularly the D3DPOOL_DEFAULT release/recreate inside
// Engine::Reset — under a realistic resize cadence. A crash or
// device-lost on resize would surface as a snapshot timeout or
// `ok:false`.
//
// This test doesn't (and can't, without GPU pixel inspection) verify
// that the rendered output is crisp; that's a manual smoke-test gate
// noted in the dispatch handoff.

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

test("host survives a sequence of layout/viewport-rect resizes and keeps snapshot responsive", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    const sizes = [
      { x: 0, y: 0, w: 320,  h: 240  },
      { x: 0, y: 0, w: 800,  h: 600  },
      { x: 0, y: 0, w: 1600, h: 1200 },
    ];

    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const snapshots: Array<{ ok: boolean; size: any; error?: string }> = [];

    for (const s of sizes) {
      // Fire-and-forget by contract (host returns ok with empty data).
      await b.request({ kind: "layout/viewport-rect", params: s });
      // Snapshot proves the host still services bridge requests after
      // each Engine::Reset. The NativeBridge contract resolves with the
      // data envelope's `data` field on ok:true and rejects (throws) on
      // ok:false, so a try/catch is enough to gate "the host kept
      // answering after this resize".
      try {
        const snap = await b.request({ kind: "engine/state/snapshot", params: {} });
        snapshots.push({ ok: snap != null, size: s });
      } catch (err) {
        snapshots.push({ ok: false, size: s, error: (err as Error).message });
      }
    }

    return { snapshots };
  });

  expect(result.snapshots).toHaveLength(3);
  for (const s of result.snapshots) {
    if (!s.ok) {
      throw new Error(`snapshot failed after resize ${JSON.stringify(s.size)}: ${s.error ?? "(no error)"}`);
    }
  }
});
