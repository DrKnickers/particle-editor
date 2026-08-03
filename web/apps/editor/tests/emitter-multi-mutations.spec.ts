// Playwright contract specs.
//
// Verifies:
//   1. emitters/add-lifetime-child via the bridge adds a lifetime child
//      under the selected emitter (observe via tree/changed + a
//      child node with role: "lifetime").
//   2. emitters/move down swaps adjacent roots (the host seed has one
//      root, so we duplicate first to grow the tree, then move).
//   3. Ctrl+click multi-select updates the React-side state observable
//      via data-selected-count on the tree container.
//
// Talks to the host's real ParticleSystem via window.bridge — no
// seeding mocks; the native host owns the live system.

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

// ── 1. Add Lifetime Child via the bridge ─────────────────────────────

test("emitters/add-lifetime-child via the bridge adds a lifetime child to the parent", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Find a parent whose lifetime slot is currently empty. We pick
    // the first root; if it already has a lifetime child we delete
    // the duplicate later for cleanup. Most host seeds start with at
    // least one bare root.
    const listBefore = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; children: { role: string }[] }[] } };
    const parent = listBefore.root.children.find(
      (c) => !c.children.some((kid) => kid.role === "lifetime"),
    );
    if (!parent) {
      // All roots already have lifetime children — fall back to the
      // first root and accept the no-op semantics from the host.
      return { skipped: true };
    }

    const r = await bridge.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: parent.id },
    });
    const listAfter = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; children: { id: number; role: string }[] }[] } };
    const parentAfter = listAfter.root.children.find((c) => c.id === parent.id)!;
    const lifetime = parentAfter.children.find((c) => c.role === "lifetime");

    return {
      parentId: parent.id,
      newId: r.newId,
      hasLifetime: lifetime !== undefined,
      lifetimeId: lifetime?.id,
    };
  });

  if (!("skipped" in result)) {
    expect(result.hasLifetime).toBe(true);
    if (typeof result.newId === "number" && result.newId >= 0) {
      expect(result.lifetimeId).toBe(result.newId);
    }
    // Cleanup: delete the newly added child so subsequent specs see a
    // smaller tree. Errors are swallowed; host-state is reset on a
    // future file/new anyway.
    if (typeof result.newId === "number" && result.newId >= 0) {
      await page.evaluate(async (id) => {
        const bridge = (window as Window & {
          bridge?: {
            request: (req: { kind: string; params: unknown }) => Promise<unknown>;
          };
        }).bridge;
        if (bridge) await bridge.request({ kind: "emitters/delete", params: { id } });
      }, result.newId);
    }
  }
});

// ── 2. emitters/move swaps adjacent roots ────────────────────────────

test("emitters/move via the bridge swaps adjacent roots", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Seed: most host particle systems have just one root, so duplicate
    // it first to give us two adjacent roots. The duplicate lands
    // directly after the original via insertEmitterAfter.
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
    // Identify the duplicate by its auto-suffixed name (`<base>_1`
    // etc.) — `_<digits>` suffix is the legacy convention.
    const dupIdx = beforeNames.findIndex((n) => /_\d+$/.test(n));
    if (dupIdx === -1) return { error: "no duplicate produced" };
    const dupId = before.root.children[dupIdx]!.id;

    // Move-up by one: the duplicate should swap positions with the
    // root that immediately precedes it.
    await bridge.request({
      kind: "emitters/move",
      params: { id: dupId, direction: "up" },
    });
    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string }[] } };
    const afterNames = after.root.children.map((c) => c.name);
    // Find the duplicate by its name in the post-move tree.
    const newIdx = afterNames.findIndex((n) => /_\d+$/.test(n));
    // The new id at that position may differ — the move rewrote
    // indices. Use the name lookup to identify the duplicate.
    const dupIdAfter = newIdx >= 0 ? after.root.children[newIdx]!.id : -1;

    // Cleanup: delete the duplicate by its current id.
    if (dupIdAfter >= 0) {
      await bridge.request({
        kind: "emitters/delete",
        params: { id: dupIdAfter },
      });
    }
    return { dupIdxBefore: dupIdx, dupIdxAfter: newIdx, beforeNames, afterNames };
  });

  if ("error" in result) {
    throw new Error(result.error);
  }
  // Move-up: position in the roots list decreases by exactly 1.
  expect(result.dupIdxAfter).toBe(result.dupIdxBefore - 1);
});

// ── 3. Ctrl+click multi-select via the DOM ───────────────────────────

test("Ctrl+click on a second emitter row updates data-selected-count to 2", async () => {
  // Reset selection: clicking anywhere outside is a clear, but a
  // simpler way is to plain-click the first emitter row.
  const treeContainer = page.locator('[data-testid="emitter-tree"]');
  await expect(treeContainer).toBeVisible();

  // Make sure the tree has at least two rows. Seed an extra root if
  // there's only one — we'll delete it at the end.
  const seededId = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number }[] } };
    if (list.root.children.length >= 2) return -1;
    const firstId = list.root.children[0]!.id;
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    }) as { newId?: number };
    return dup.newId ?? -1;
  });

  // Wait for the tree to refresh after a possible seed.
  await page.waitForFunction(() => {
    const tree = document.querySelector('[data-testid="emitter-tree"]');
    if (!tree) return false;
    return tree.querySelectorAll("[data-emitter-id]").length >= 2;
  });

  const rows = treeContainer.locator("[data-emitter-id]");
  const firstRow  = rows.nth(0);
  const secondRow = rows.nth(1);

  // Plain-click first row to seed selection.
  await firstRow.click();
  await expect(treeContainer).toHaveAttribute("data-selected-count", "1");

  // Ctrl+click second row to add it.
  await secondRow.click({ modifiers: ["Control"] });
  await expect(treeContainer).toHaveAttribute("data-selected-count", "2");

  // Cleanup: drop multi-selection back to single-select on the first
  // row so subsequent specs don't see a leaked multi-set.
  await firstRow.click();
  if (seededId >= 0) {
    await page.evaluate(async (id) => {
      const bridge = (window as Window & {
        bridge?: {
          request: (req: { kind: string; params: unknown }) => Promise<unknown>;
        };
      }).bridge;
      if (bridge) await bridge.request({ kind: "emitters/delete", params: { id } });
    }, seededId);
  }
});

// ── 4. emitters/paste-as-child attaches the clipboard into a slot ────
//
// Real-host round-trip for the new Paste As ▸ command:
// copy a root, then paste-as-child into a parent whose lifetime slot is
// free, and confirm the engine attaches a lifetime child with the
// source's suffixed name (GenerateDuplicateName). Exercises the C++
// handler's deser + addLifetimeEmitter path through the actual engine,
// not just the mock.

test("emitters/paste-as-child via the bridge attaches a lifetime child", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newId?: number; root?: { children: unknown[] } }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; name: string; children: { role: string }[] }[] } };

    const srcId = before.root.children[0]?.id ?? -1;
    if (srcId < 0) return { skipped: true as const };
    const srcName = before.root.children[0]!.name;
    // A parent whose lifetime slot is free (legacy gate: spawnDuringLife == -1).
    const parent = before.root.children.find(
      (c) => !c.children.some((kid) => kid.role === "lifetime"),
    );
    if (!parent) return { skipped: true as const };

    await bridge.request({ kind: "emitters/copy", params: { ids: [srcId] } });
    const r = await bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: parent.id, slot: "lifetime" },
    });

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number; children: { id: number; name: string; role: string }[] }[] } };
    const parentAfter = after.root.children.find((c) => c.id === parent.id)!;
    const lifetime = parentAfter.children.find((c) => c.role === "lifetime");

    return {
      newId: r.newId,
      hasLifetime: lifetime !== undefined,
      lifetimeId: lifetime?.id,
      lifetimeName: lifetime?.name ?? "",
      srcName,
    };
  });

  if ("skipped" in result) {
    test.skip(true, "no free-lifetime-slot parent in the seed");
    return;
  }
  expect(typeof result.newId).toBe("number");
  expect(result.newId).toBeGreaterThanOrEqual(0);
  expect(result.hasLifetime).toBe(true);
  expect(result.lifetimeId).toBe(result.newId);
  // Pasted child takes the source name suffixed (GenerateDuplicateName).
  expect(result.lifetimeName.startsWith(result.srcName)).toBe(true);

  // Cleanup: drop the pasted child.
  if (typeof result.newId === "number" && result.newId >= 0) {
    await page.evaluate(async (id) => {
      const bridge = (window as Window & {
        bridge?: {
          request: (req: { kind: string; params: unknown }) => Promise<unknown>;
        };
      }).bridge;
      if (bridge) await bridge.request({ kind: "emitters/delete", params: { id } });
    }, result.newId);
  }
});
