// Phase 3 Screen 4 Batch B3 Playwright contract specs.
//
// Verifies:
//   1. emitters/drop { mode: "reorder", id, rootIndex } via the bridge
//      reorders root emitters in the host's live ParticleSystem.
//   2. emitters/drop { mode: "reparent", id, targetId, slot } via the
//      bridge reparents a root under another root in the named slot.
//
// We drive the bridge call directly instead of synthesising HTML5 DnD
// over CDP. The React drag handlers are verified by Vitest
// (EmitterTree.test.tsx) — the host-side semantics + the wire contract
// are what the Playwright spec needs to cover. CDP's drag synthesis is
// notoriously flaky for sub-row drop-zone positioning; bridge-driven
// verification gives us a deterministic gate.

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

// ── 1. Reorder via the bridge ────────────────────────────────────────

test("emitters/drop reorder via the bridge swaps adjacent root positions", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Seed: duplicate the first root so we have two adjacent roots to
    // shuffle. Most host particle systems load with just one root.
    const before0 = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const firstId = before0.root.children[0]!.id;
    await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    });

    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const beforeNames = before.root.children.map((c) => c.name);
    // Identify the duplicate by its auto-suffixed `_<digits>` name.
    const dupIdx = beforeNames.findIndex((n) => /_\d+$/.test(n));
    if (dupIdx === -1) return { error: "no duplicate produced" };
    const dupId = before.root.children[dupIdx]!.id;

    // Reorder: drop the duplicate at gap 0 (before the first root).
    // moveEmitterToRootIndex no-op-detects gaps [sourceIdx,
    // sourceIdx+1], so the move to gap 0 is meaningful when the dup
    // is at idx 1 (or higher).
    const r = await bridge.request({
      kind: "emitters/drop",
      params: { mode: "reorder", id: dupId, rootIndex: 0 },
    }) as { ok: boolean };

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const afterNames = after.root.children.map((c) => c.name);
    const newDupIdx = afterNames.findIndex((n) => /_\d+$/.test(n));
    // Resolve the duplicate's new id for cleanup (a successful move
    // rewrites indices).
    const dupIdAfter = newDupIdx >= 0 ? after.root.children[newDupIdx]!.id : -1;

    // Cleanup the duplicate so the host state is left as we found it.
    if (dupIdAfter >= 0) {
      await bridge.request({
        kind: "emitters/delete",
        params: { id: dupIdAfter },
      });
    }
    return {
      ok: r.ok,
      dupIdxBefore: dupIdx,
      dupIdxAfter: newDupIdx,
      beforeNames,
      afterNames,
    };
  });

  if ("error" in result) {
    throw new Error(result.error);
  }
  // Drop at gap 0 moves the duplicate to position 0. The duplicate
  // was at dupIdxBefore (likely 1 for a single-root seed → after dup),
  // so after reorder it should be at index 0.
  expect(result.ok).toBe(true);
  expect(result.dupIdxAfter).toBe(0);
  expect(result.dupIdxAfter).toBeLessThan(result.dupIdxBefore);
});

// ── 2. Reparent via the bridge ───────────────────────────────────────

test("emitters/drop reparent via the bridge attaches the source as a child of target", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Seed two roots: duplicate the first root so we have a source +
    // target pair. The target needs at least one free slot — if the
    // host seed already has a packed root, the duplicate (a fresh
    // empty copy) gives us a clean target.
    const before0 = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string; children: { role: string }[] }[] } };
    const firstId = before0.root.children[0]!.id;
    const dupResult = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    }) as { ok: boolean; newId?: number };
    if (!dupResult.ok || typeof dupResult.newId !== "number") {
      return { error: "duplicate failed" };
    }
    const dupId = dupResult.newId;

    // Pick a target: the original first root. Find a free slot via
    // its current children's roles.
    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string; children: { role: string }[] }[] } };
    const target = after.root.children.find((c) => c.id === firstId);
    if (!target) return { error: "target not found post-duplicate" };
    const hasLifetime = target.children.some((c) => c.role === "lifetime");
    const hasDeath    = target.children.some((c) => c.role === "death");
    if (hasLifetime && hasDeath) return { skipped: true };
    const slot: "lifetime" | "death" = !hasLifetime ? "lifetime" : "death";

    // Reparent: drop the duplicate under the original in the named slot.
    const r = await bridge.request({
      kind: "emitters/drop",
      params: { mode: "reparent", id: dupId, targetId: firstId, slot },
    }) as { ok: boolean; error?: string };

    const final = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; children: { id: number; role: string }[] }[] } };

    const targetAfter = final.root.children.find((c) => c.id === firstId);
    const childUnderTarget = targetAfter?.children.find((c) => c.id === dupId);
    // The duplicate should no longer be a root.
    const stillRoot = final.root.children.some((c) => c.id === dupId);

    // Cleanup: delete the duplicate (it's now a child of target).
    await bridge.request({
      kind: "emitters/delete",
      params: { id: dupId },
    });

    return {
      ok: r.ok,
      childRole: childUnderTarget?.role,
      stillRoot,
      slotPicked: slot,
    };
  });

  if ("error" in result) {
    throw new Error(result.error);
  }
  if ("skipped" in result) {
    test.skip(true, "host seed has both slots filled on first root");
    return;
  }
  expect(result.ok).toBe(true);
  expect(result.stillRoot).toBe(false);
  expect(result.childRole).toBe(result.slotPicked);
});
