// Phase 3 Screen 4 Batch C Playwright contract specs.
//
// Verifies:
//   1. F2 on a focused tree row enters inline rename mode (an
//      `<input>` is rendered in that row).
//   2. Delete dispatched while a row is selected fires
//      `emitters/delete` and the row count decreases.
//   3. Copy + paste round-trip via the bridge directly produces a
//      new root whose contents echo the source emitter.
//
// We drive the F2 + Delete keystrokes via Playwright keyboard
// dispatch (the React handlers attach to the tree container). The
// clipboard round-trip uses the bridge directly — CDP keyboard
// dispatch for Ctrl+C/V can be flaky depending on the focus state
// and shift bookkeeping; bridge-driven gives us a deterministic
// gate. The React-side Ctrl+C/X/V wiring is exercised by Vitest.

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

// ── 1. F2 enters inline rename mode ──────────────────────────────────

test("F2 on focused tree row enters inline rename (input appears)", async () => {
  // Ensure at least one root exists.
  const seed = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const tree = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    return { firstId: tree.root.children[0]?.id ?? -1, firstName: tree.root.children[0]?.name ?? "" };
  });
  if (seed.firstId < 0) {
    test.skip(true, "no root emitter present in host seed");
    return;
  }

  // Click the row to focus it.
  const rowSelector = `button[data-emitter-id="${seed.firstId}"]`;
  await page.waitForSelector(rowSelector, { timeout: 5_000 });
  await page.click(rowSelector);
  // Press F2 — the React keyboard handler should swap the label for
  // an <input> with the current name.
  await page.keyboard.press("F2");

  const inputSelector = `[data-testid="emitter-rename-input-${seed.firstId}"]`;
  await page.waitForSelector(inputSelector, { timeout: 5_000 });
  const value = await page.locator(inputSelector).inputValue();
  expect(value).toBe(seed.firstName);

  // Cancel the rename so the test leaves no edit state behind.
  await page.keyboard.press("Escape");
  await expect(page.locator(inputSelector)).toHaveCount(0);
});

// ── 2. Delete fires emitters/delete on selection ─────────────────────

test("Delete key on focused tree fires emitters/delete on the selection", async () => {
  // Seed a duplicate root so we have something safe to delete (the
  // host seed's only root may be one tests downstream rely on).
  const seed = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number }[] } };
    const firstId = before.root.children[0]?.id ?? -1;
    if (firstId < 0) return { error: "no seed root" };
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    }) as { ok: boolean; newId?: number };
    if (!dup.ok || typeof dup.newId !== "number") return { error: "duplicate failed" };
    return { dupId: dup.newId };
  });
  if ("error" in seed) {
    test.skip(true, seed.error!);
    return;
  }

  const before = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
    }).bridge!;
    const tree = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    return tree.root.children.length;
  });

  // Click the duplicate row to select it, then press Delete.
  const rowSelector = `button[data-emitter-id="${seed.dupId}"]`;
  await page.waitForSelector(rowSelector, { timeout: 5_000 });
  await page.click(rowSelector);
  await page.keyboard.press("Delete");

  // Allow the tree-changed round trip to flush.
  await page.waitForFunction(
    ({ countBefore }) => {
      const bridge = (window as Window & {
        bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
      }).bridge;
      if (!bridge) return false;
      return bridge
        .request({ kind: "emitters/list", params: {} })
        .then((t: unknown) => {
          const tree = t as { root: { children: { id: number }[] } };
          return tree.root.children.length < countBefore;
        });
    },
    { countBefore: before },
    { timeout: 5_000 },
  );

  const after = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
    }).bridge!;
    const tree = await bridge.request({ kind: "emitters/list", params: {} }) as {
      root: { children: { id: number }[] };
    };
    return tree.root.children.length;
  });
  expect(after).toBeLessThan(before);
});

// ── 3. Copy + Paste round-trip via the bridge ────────────────────────

test("emitters/copy + paste round-trip via the bridge creates a new root", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: { request: (req: { kind: string; params: unknown }) => Promise<unknown> };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const srcId = before.root.children[0]?.id ?? -1;
    if (srcId < 0) return { error: "no seed root" };
    const srcName = before.root.children[0]!.name;
    const countBefore = before.root.children.length;

    await bridge.request({ kind: "emitters/copy", params: { ids: [srcId] } });
    const pasteRes = await bridge.request({
      kind: "emitters/paste",
      params: {},
    }) as { newIds: number[] };

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const countAfter = after.root.children.length;

    // Cleanup — drop the pasted root(s).
    for (const id of pasteRes.newIds) {
      await bridge.request({ kind: "emitters/delete", params: { id } });
    }

    // The pasted root takes a fresh id + a suffixed name (GenerateDuplicateName).
    const pastedId = pasteRes.newIds[0] ?? -1;
    const pastedRow = after.root.children.find((c) => c.id === pastedId);

    return {
      newIdsLen: pasteRes.newIds.length,
      countBefore,
      countAfter,
      srcName,
      pastedName: pastedRow?.name ?? "",
    };
  });

  if ("error" in result) {
    test.skip(true, result.error!);
    return;
  }
  expect(result.newIdsLen).toBe(1);
  expect(result.countAfter).toBe(result.countBefore + 1);
  // Pasted name should be the source name suffixed (GenerateDuplicateName).
  expect(result.pastedName.startsWith(result.srcName)).toBe(true);
  expect(result.pastedName).not.toBe(result.srcName);
});
