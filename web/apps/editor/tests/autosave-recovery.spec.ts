// autosave/check-recovery + autosave/recover round-trip spec ().
//
// The deterministic, harness-safe halves of the feature:
//   1. check-recovery is SUPPRESSED under --test-host (Risk 4) — the harness
//      must never get a real recovery prompt (it would pollute a11y captures,
//      cf.). The host returns { orphan: null }.
//   2. recover is a safe no-op when there's no pending orphan (no prior
//      check / double-recover) — returns {} and changes nothing.
//
// The actual restore/discard-with-orphan paths can't run here (the scan is
// suppressed under --test-host by design), so they're covered by the manual
// crash smoke in tasks/todo.md + the AutosaveRecoveryDialog vitest.

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

test("autosave/check-recovery returns no orphan under --test-host", async () => {
  const r = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    return await b.request({ kind: "autosave/check-recovery", params: {} });
  });
  expect(r).toEqual({ orphan: null });
});

test("autosave/recover{discard} is a safe no-op with no pending orphan", async () => {
  const { result, dirtyBefore, dirtyAfter } = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    const res = await b.request({ kind: "autosave/recover", params: { choice: "discard" } });
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { result: res, dirtyBefore: before.dirty, dirtyAfter: after.dirty };
  });
  expect(result).toEqual({});
  // No pending orphan ⇒ recover touches nothing (dirty bit unchanged).
  expect(dirtyAfter).toBe(dirtyBefore);
});
