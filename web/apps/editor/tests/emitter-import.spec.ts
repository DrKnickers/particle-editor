// G1 — emitters/import-from-file native handler contract spec.
//
// Drives the REAL host (window.bridge over CDP — no mocks). Verifies:
//   1. emitters/preview-from-file returns the source file's emitter tree.
//   2. Full import: every source emitter clones into the live system, and —
//      crucially — the imported subtree keeps its SHAPE (one new root with its
//      lifetime + death children re-parented, not three loose roots). The
//      count alone can't tell a correct rebind from a broken one, so we assert
//      the shape, not just the size.
//   3. Partial import: selecting only the root drops the (non-picked) child
//      links — exercises the spawn-rebind miss-branch.
//   4. Failed import (bad path): rejects, mutates nothing, and pushes NO undo
//      entry (a single undo reverts the prior successful import).
//
// Source fixture: a11y-base-state.alo = root[0] + lifetime-child[1] +
// death-child[2] (no link groups — link-group recreation is NOT covered here).
//
// NOTE: helper funcs are defined INSIDE each page.evaluate — that body runs in
// the browser/host page context, where module-scope helpers don't exist.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const CDP_ENDPOINT = process.env.CDP_ENDPOINT ?? "http://localhost:9222";

// Host accepts forward slashes on Windows; avoids backslash-escaping noise.
const SOURCE_ALO = resolve(__dirname, "fixtures", "a11y-base-state.alo").replace(/\\/g, "/");
const BAD_ALO = resolve(__dirname, "fixtures", "does-not-exist.alo").replace(/\\/g, "/");

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

test("preview-from-file returns the source file's emitter tree", async () => {
  const { ok, count } = await page.evaluate(async (path) => {
    type TreeNode = { id: number; name: string; children?: TreeNode[] };
    const countNodes = (node: TreeNode | undefined): number =>
      (node?.children ?? []).reduce((n, c) => n + 1 + countNodes(c), 0);
    // Keep the bridge object — request() relies on `this` (its pending-id
    // map); destructuring the method loses the binding.
    const bridge = (window as unknown as {
      bridge: { request: (r: { kind: string; params: unknown }) =>
        Promise<{ ok?: boolean; tree?: TreeNode }> };
    }).bridge;
    const r = await bridge.request({ kind: "emitters/preview-from-file", params: { path } });
    return { ok: r.ok === true, count: countNodes(r.tree) };
  }, SOURCE_ALO);
  expect(ok).toBe(true);
  expect(count).toBeGreaterThan(0);
});

test("full import re-nests the imported subtree (shape, not just count)", async () => {
  const result = await page.evaluate(async (path) => {
    type TreeNode = { id: number; name: string; role?: string; children?: TreeNode[] };
    type Resp = { ok?: boolean; imported?: number; tree?: TreeNode; root?: TreeNode; error?: string };
    const countNodes = (node: TreeNode | undefined): number =>
      (node?.children ?? []).reduce((n, c) => n + 1 + countNodes(c), 0);
    const collectIds = (node: TreeNode | undefined): number[] =>
      (node?.children ?? []).flatMap((c) => [c.id, ...collectIds(c)]);
    const topIds = (root: TreeNode | undefined): Set<number> =>
      new Set((root?.children ?? []).map((c) => c.id));
    const bridge = (window as unknown as {
      bridge: { request: (r: { kind: string; params: unknown }) => Promise<Resp> };
    }).bridge;
    const list = async () => (await bridge.request({ kind: "emitters/list", params: {} })).root;

    // Preview → import every source index.
    const preview = await bridge.request({ kind: "emitters/preview-from-file", params: { path } });
    const selected = collectIds(preview.tree);

    const before = await list();
    const beforeCount = countNodes(before);
    const beforeTop = topIds(before);

    const imp = await bridge.request({ kind: "emitters/import-from-file", params: { path, selected } });

    const after = await list();
    const afterCount = countNodes(after);
    // The new top-level (parent===null) node(s) the import added.
    const newTops = (after?.children ?? []).filter((c) => !beforeTop.has(c.id));
    const newRootChildRoles = (newTops[0]?.children ?? []).map((c) => c.role).sort();

    // Atomic single undo reverts the whole import (and cleans up for later specs).
    await bridge.request({ kind: "undo/perform", params: { direction: "undo" } });
    const undoneCount = countNodes(await list());

    return {
      selectedLen: selected.length,
      importOk: imp.ok === true,
      imported: imp.imported ?? null,
      beforeCount,
      afterCount,
      newTopCount: newTops.length,
      newRootChildCount: newTops[0]?.children?.length ?? 0,
      newRootChildRoles,
      undoneCount,
    };
  }, SOURCE_ALO);

  expect(result.selectedLen).toBe(3); // root + lifetime + death
  expect(result.importOk).toBe(true);
  expect(result.imported).toBe(result.selectedLen);
  expect(result.afterCount).toBe(result.beforeCount + result.selectedLen);
  // Shape: exactly ONE new top-level root appeared (not three loose roots),
  // and it re-parented its lifetime + death children. A broken spawn-rebind
  // would leave all three as roots — same count, different (wrong) shape.
  expect(result.newTopCount).toBe(1);
  expect(result.newRootChildCount).toBe(2);
  expect(result.newRootChildRoles).toEqual(["death", "lifetime"]);
  // Atomic single-undo reverts the entire import.
  expect(result.undoneCount).toBe(result.beforeCount);
});

test("partial import (root only) drops the non-picked child links", async () => {
  const result = await page.evaluate(async (path) => {
    type TreeNode = { id: number; name: string; role?: string; children?: TreeNode[] };
    type Resp = { ok?: boolean; imported?: number; tree?: TreeNode; root?: TreeNode };
    const topIds = (root: TreeNode | undefined): Set<number> =>
      new Set((root?.children ?? []).map((c) => c.id));
    const bridge = (window as unknown as {
      bridge: { request: (r: { kind: string; params: unknown }) => Promise<Resp> };
    }).bridge;
    const list = async () => (await bridge.request({ kind: "emitters/list", params: {} })).root;

    // Select ONLY the source root (first top-level node) — its children are
    // NOT picked, so the rebind must drop those links → a childless root.
    const preview = await bridge.request({ kind: "emitters/preview-from-file", params: { path } });
    const rootId = preview.tree?.children?.[0]?.id;

    const before = await list();
    const beforeTop = topIds(before);

    const imp = await bridge.request({
      kind: "emitters/import-from-file",
      params: { path, selected: [rootId] },
    });

    const after = await list();
    const newTops = (after?.children ?? []).filter((c) => !beforeTop.has(c.id));

    await bridge.request({ kind: "undo/perform", params: { direction: "undo" } });

    return {
      rootIdValid: typeof rootId === "number",
      imported: imp.imported ?? null,
      newTopCount: newTops.length,
      newRootChildCount: newTops[0]?.children?.length ?? -1,
    };
  }, SOURCE_ALO);

  expect(result.rootIdValid).toBe(true);
  expect(result.imported).toBe(1);
  expect(result.newTopCount).toBe(1);
  // Children weren't picked → spawn links drop → the imported root is childless.
  expect(result.newRootChildCount).toBe(0);
});

test("a failed import rejects, mutates nothing, and pushes no undo entry", async () => {
  const result = await page.evaluate(async ({ path, badPath }) => {
    type TreeNode = { id: number; children?: TreeNode[] };
    type Resp = { ok?: boolean; imported?: number; tree?: TreeNode; root?: TreeNode };
    const countNodes = (node: TreeNode | undefined): number =>
      (node?.children ?? []).reduce((n, c) => n + 1 + countNodes(c), 0);
    const collectIds = (node: TreeNode | undefined): number[] =>
      (node?.children ?? []).flatMap((c) => [c.id, ...collectIds(c)]);
    const bridge = (window as unknown as {
      bridge: { request: (r: { kind: string; params: unknown }) => Promise<Resp> };
    }).bridge;
    const count = async () => countNodes((await bridge.request({ kind: "emitters/list", params: {} })).root);

    const baseline = await count();

    // A valid import first (so there's a known undoable mutation on the stack).
    const preview = await bridge.request({ kind: "emitters/preview-from-file", params: { path } });
    const selected = collectIds(preview.tree);
    await bridge.request({ kind: "emitters/import-from-file", params: { path, selected } });
    const afterImport = await count();

    // Now a FAILED import (non-existent path) — must reject (sendErr) and mutate nothing.
    let threw = false;
    try {
      await bridge.request({ kind: "emitters/import-from-file", params: { path: badPath, selected: [0] } });
    } catch {
      threw = true;
    }
    const afterFail = await count();

    // ONE undo. If the failed import pushed NO undo entry, this reverts the
    // VALID import → back to baseline. A stray entry from the failure would
    // make this undo a no-op and leave the valid import in place.
    await bridge.request({ kind: "undo/perform", params: { direction: "undo" } });
    const afterUndo = await count();

    return { baseline, selectedLen: selected.length, afterImport, threw, afterFail, afterUndo };
  }, { path: SOURCE_ALO, badPath: BAD_ALO });

  expect(result.threw).toBe(true); // sendErr → envelope ok:false → promise rejects
  expect(result.afterImport).toBe(result.baseline + result.selectedLen);
  expect(result.afterFail).toBe(result.afterImport); // failed import mutated nothing
  expect(result.afterUndo).toBe(result.baseline); // one undo reverted the valid import → no stray entry
});
