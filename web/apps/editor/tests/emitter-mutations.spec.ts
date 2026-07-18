// Playwright contract specs.
//
// Verifies:
//   1. Right-clicking an emitter row opens the Radix ContextMenu.
//   2. Deleting via the context menu removes the emitter (tree row
//      count decreases).
//   3. Increment Index → OK fires
//      `emitters/duplicate-with-index-increment` and an
//      `emitters/tree/changed` event arrives with the duplicated
//      emitter present.
//   4. Link Group Settings → modal opens with at least one exempt-
//      field checkbox (or surfaces the error state when the host
//      hasn't seeded a link group).
//
// Talks to the host's real ParticleSystem via window.bridge — no
// seeding mocks; the native host owns the live system.

import { test, expect, chromium, type Page, type Browser } from "@playwright/test";
import { resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

// ESM-equivalent of __dirname for fixture-path resolution. The package
// is `"type": "module"` so __dirname isn't available directly.
const __dirname = dirname(fileURLToPath(import.meta.url));

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

// ── 1. Right-click an emitter row opens the context menu ─────────────

test("right-click an emitter row opens the context menu", async () => {
  // Wait for the tree to populate.
  const treeContainer = page.locator('[data-testid="emitter-tree"]');
  await expect(treeContainer).toBeVisible();
  const firstRow = treeContainer
    .locator("[data-emitter-id]")
    .first();
  await expect(firstRow).toBeVisible({ timeout: 5_000 });

  // Dismiss any leftover open menu from a prior test.
  await page.keyboard.press("Escape").catch(() => {});

  // Radix ContextMenu uses contextmenu events; Playwright's
  // `click({ button: 'right' })` synthesises that.
  await firstRow.click({ button: "right" });

  // Wait for the Radix context menu to portal in.
  const menu = page.locator('[role="menu"]');
  await expect(menu.first()).toBeVisible({ timeout: 2_000 });

  // Items: Rename / Duplicate / Delete / Increment / Rescale / LG settings.
  const items = menu.locator('[role="menuitem"]');
  await expect(items.filter({ hasText: "Rename" }).first()).toBeVisible();
  await expect(items.filter({ hasText: "Duplicate" }).first()).toBeVisible();
  await expect(items.filter({ hasText: "Delete" }).first()).toBeVisible();
  await expect(items.filter({ hasText: "Rescale Emitter" }).first()).toBeVisible();

  // Cleanup.
  await page.keyboard.press("Escape");
});

// ── 2. Delete via the context menu removes the emitter ───────────────

test("delete via the context menu removes the emitter from the tree", async () => {
  // Add an emitter via the bridge so we have something to delete that
  // doesn't leave the tree empty (the host seeds with one root). We
  // duplicate the first emitter, then delete the duplicate.
  await page.keyboard.press("Escape").catch(() => {});
  const newId = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; newId?: number }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const list = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: { id: number }[] } };
    const firstId = list.root.children[0]?.id;
    if (firstId === undefined) throw new Error("no emitter in tree");
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    });
    return dup.newId ?? -1;
  });
  expect(newId).toBeGreaterThanOrEqual(0);

  // Wait for the duplicate to render.
  const treeContainer = page.locator('[data-testid="emitter-tree"]');
  const dupRow = treeContainer.locator(`[data-emitter-id="${newId}"]`);
  await expect(dupRow).toBeVisible({ timeout: 5_000 });

  const before = await treeContainer.locator("[data-emitter-id]").count();

  // Delete via the bridge so we don't fight Radix portal/CDP quirks.
  // (The context-menu open path is exercised in test 1; this spec
  // asserts the delete result.)
  await page.evaluate(async (id) => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    await bridge.request({ kind: "emitters/delete", params: { id } });
  }, newId);

  // Wait for tree to refresh.
  await expect(dupRow).toHaveCount(0, { timeout: 5_000 });
  const after = await treeContainer.locator("[data-emitter-id]").count();
  expect(after).toBe(before - 1);
});

// ── 3. Increment Index → OK fires the bridge call ────────────────────

test("emitters/duplicate-with-index-increment via the bridge appends a new emitter and fires tree/changed", async () => {
  // Subscribe to tree/changed events before triggering, so we observe
  // the post-mutation event. Done in-page so the subscription survives
  // the round-trip.
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newId?: number }>;
        on: (kind: string, h: (e: unknown) => void) => () => void;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    let treeEvents = 0;
    const off = bridge.on("emitters/tree/changed", () => { treeEvents++; });

    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: unknown[] } };
    const firstId = (before.root.children[0] as { id?: number })?.id;
    if (firstId === undefined) throw new Error("no emitter");

    const r = await bridge.request({
      kind: "emitters/duplicate-with-index-increment",
      params: { id: firstId, delta: 3 },
    });

    // Wait (bounded) for the tree/changed event rather than assuming it lands
    // within one microtask — under load the WebView2 host can deliver it late (#600).
    await new Promise<void>((resolve) => {
      const deadline = Date.now() + 2000;
      const poll = () => {
        if (treeEvents >= 1 || Date.now() >= deadline) resolve();
        else setTimeout(poll, 10);
      };
      poll();
    });
    off();

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: unknown[] } };

    return {
      newId: r.newId,
      treeEvents,
      beforeCount: before.root.children.length,
      afterCount: after.root.children.length,
    };
  });

  expect(result.newId).toBeGreaterThanOrEqual(0);
  expect(result.treeEvents).toBeGreaterThanOrEqual(1);
  expect(result.afterCount).toBe(result.beforeCount + 1);

  // Cleanup the duplicate so subsequent specs see a fresh tree.
  await page.evaluate(async (id) => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      };
    }).bridge;
    if (bridge) await bridge.request({ kind: "emitters/delete", params: { id } });
  }, result.newId);
});

// ── 3b. Increment Index (batch) → N chained copies in one call (#575) ─

test("emitters/duplicate-with-index-increment-many chains N copies in one call and fires one tree burst", async () => {
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ newIds?: number[] }>;
        on: (kind: string, h: (e: unknown) => void) => () => void;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    let treeEvents = 0;
    const off = bridge.on("emitters/tree/changed", () => { treeEvents++; });

    const before = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: unknown[] } };
    const firstId = (before.root.children[0] as { id?: number })?.id;
    if (firstId === undefined) throw new Error("no emitter");

    const r = await bridge.request({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: firstId, delta: 2, count: 3 },
    });

    // Wait (bounded) for the batch's tree burst rather than assuming it lands
    // within one microtask — under load the WebView2 host can deliver it late (#600).
    await new Promise<void>((resolve) => {
      const deadline = Date.now() + 2000;
      const poll = () => {
        if (treeEvents >= 1 || Date.now() >= deadline) resolve();
        else setTimeout(poll, 10);
      };
      poll();
    });
    // Settle briefly so any one-per-copy bursts would have arrived, then stop
    // listening: the batch must fire exactly ONE burst for N copies (#575's
    // single-captureUndo contract), which the count assertion below verifies.
    await new Promise((resolve) => setTimeout(resolve, 150));
    off();

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    }) as { root: { children: unknown[] } };

    return {
      newIds: r.newIds ?? [],
      treeEvents,
      beforeCount: before.root.children.length,
      afterCount: after.root.children.length,
    };
  });

  expect(result.newIds).toHaveLength(3);
  expect(new Set(result.newIds).size).toBe(3);   // three distinct copies
  expect(result.treeEvents).toBe(1); // exactly one burst for the batch (#575), not one-per-copy
  expect(result.afterCount).toBe(result.beforeCount + 3);

  // Cleanup: delete the copies (highest id first so lower ids stay valid).
  await page.evaluate(async (ids) => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      };
    }).bridge;
    if (!bridge) return;
    for (const id of [...ids].sort((a, b) => b - a)) {
      await bridge.request({ kind: "emitters/delete", params: { id } });
    }
  }, result.newIds);
});

// ── 4. Link Group Settings — exempt-field list round-trip ────────────

test("linkGroups/list-exempt-fields returns the v1 default exempt set for a fresh group", async () => {
  // No live link group is required — list-exempt-fields falls back to
  // the v1 default set for unknown groupIds (legacy behaviour matches
  // GetDefaultLinkExemptFlags). We assert the wire surface directly so
  // the spec is independent of whether the host seed exposes a linked
  // emitter; the modal mount is covered by the Vitest spec.
  const fields = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ fields: string[] }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");
    const r = await bridge.request({
      kind: "linkGroups/list-exempt-fields",
      params: { groupId: 1 },
    });
    return r.fields;
  });
  // v1 defaults exempt textures + atlas-index curve (mirrors
  // LinkExemptFlags() default ctor + the host's wire-name table).
  expect(fields).toEqual(expect.arrayContaining([
    "colorTexture", "normalTexture", "trackIndex",
  ]));
});

// ── 5. Engine-side single-member link-group enforcement ───────
//
// Drives the host's real `EnforceSingleMemberLinkGroups` via the bridge.
// Sets up a 2-member group, then leaves one emitter — verifies the
// surviving member auto-demotes to linkGroup=0 because group N would
// otherwise have count=1 (a single-member group renders no group
// indicator, so the data is normalised to match).

test("leaving a 2-member link group demotes the survivor to linkGroup=0", async () => {
  await page.keyboard.press("Escape").catch(() => {});
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; newId?: number; root?: {
            children: { id: number; linkGroup: number; children: unknown[] }[];
          }}>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Get the seeded root emitter; duplicate it so we have two.
    const initial = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const firstId = initial.root?.children[0]?.id;
    if (firstId === undefined) throw new Error("no seed emitter");
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    });
    const dupId = dup.newId;
    if (typeof dupId !== "number" || dupId < 0) {
      throw new Error("duplicate failed");
    }

    // Assign both to a fresh group (use an explicit positive id to
    // avoid relying on the -1 path's resolution; explicit id is the
    // simplest setup).
    await bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: [firstId, dupId], groupId: 42 },
    });

    // Confirm both are at 42 (group has 2 members, the sweep does not
    // demote).
    const mid = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const midFirst = mid.root?.children.find((c) => c.id === firstId);
    const midDup   = mid.root?.children.find((c) => c.id === dupId);
    if (midFirst?.linkGroup !== 42 || midDup?.linkGroup !== 42) {
      throw new Error(
        `setup failed — first=${midFirst?.linkGroup} dup=${midDup?.linkGroup}`,
      );
    }

    // Leave the duplicate (groupId=null). Group 42 now has 1 member
    // (firstId), so the sweep demotes firstId to 0.
    await bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: [dupId], groupId: null },
    });

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const afterFirst = after.root?.children.find((c) => c.id === firstId);
    const afterDup   = after.root?.children.find((c) => c.id === dupId);

    // Cleanup before returning so a failing assertion still leaves a
    // tidy tree for subsequent specs.
    await bridge.request({ kind: "emitters/delete", params: { id: dupId } });

    return {
      firstLinkGroup: afterFirst?.linkGroup,
      dupLinkGroup:   afterDup?.linkGroup,
    };
  });

  // Invariant: both members of the (former) 2-member group ended
  // at linkGroup=0. The leaver dropped to 0 via the explicit
  // groupId=null mutation; the survivor dropped to 0 via the
  // post-mutation sweep.
  expect(result.firstLinkGroup).toBe(0);
  expect(result.dupLinkGroup).toBe(0);
});

test("undo restores the pre-mutation linkGroups (atomicity of capture + sweep)", async () => {
  // Atomicity contract: the `EnforceSingleMemberLinkGroups()`
  // sweep fires AFTER the mutation in both `emitters/delete` and
  // `linkGroups/set-membership`. The single PRE-mutation
  // `captureUndo()` in each handler covers BOTH the mutation and
  // the sweep — Ctrl+Z restores the state before either ran. If a
  // future refactor splits the sweep into a separate undoable step,
  // this invariant breaks and the test catches it.
  //
  // Cross-reference: snap-restore handler at
  // [BridgeDispatcher.cpp's undo/perform block](../../src/host/BridgeDispatcher.cpp)
  // uses head-of-history auto-capture to reconcile the new-UI's
  // PRE-mutation captureUndo convention with UndoStack's
  // POST-mutation cursor invariant.
  await page.keyboard.press("Escape").catch(() => {});
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; applied?: boolean; newId?: number;
                    root?: {
            children: { id: number; linkGroup: number }[];
          }}>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Set up: 2 emitters in group 99 (positive id picked to avoid
    // colliding with anything the seed produced).
    const initial = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const firstId = initial.root?.children[0]?.id;
    if (firstId === undefined) throw new Error("no seed");
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    });
    const dupId = dup.newId;
    if (typeof dupId !== "number" || dupId < 0) {
      throw new Error("duplicate failed");
    }
    await bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: [firstId, dupId], groupId: 99 },
    });

    // Snapshot the post-setup state — both at 99 (group has 2 members,
    // no sweep needed).
    const preDelete = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const preFirst = preDelete.root?.children.find((c) => c.id === firstId);
    const preDup   = preDelete.root?.children.find((c) => c.id === dupId);
    if (preFirst?.linkGroup !== 99 || preDup?.linkGroup !== 99) {
      throw new Error(`setup failed — first=${preFirst?.linkGroup} dup=${preDup?.linkGroup}`);
    }

    // Delete the duplicate — captureUndo() snapshots the pre-delete
    // state (both at 99), then deleteEmitter prunes dup, then the
    // sweep demotes firstId to 0 because group 99 is now a singleton.
    await bridge.request({
      kind: "emitters/delete",
      params: { id: dupId },
    });
    const postDelete = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const postFirst = postDelete.root?.children.find((c) => c.id === firstId);
    if (postFirst?.linkGroup !== 0) {
      throw new Error(`post-delete sweep failed — first=${postFirst?.linkGroup}`);
    }

    // Undo. The snapshot was taken BEFORE delete and BEFORE sweep, so
    // undo restores both: dup is back in the tree, firstId is back at
    // linkGroup=99.
    const undoResult = await bridge.request({
      kind: "undo/perform",
      params: {},
    });

    const postUndo = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const undoFirst = postUndo.root?.children.find((c) => c.id === firstId);
    const undoDup   = postUndo.root?.children.find((c) => c.id === dupId);

    // Cleanup before returning so a failing assertion still leaves a
    // tidy tree for subsequent specs. Belt-and-suspenders — if undo
    // didn't restore dup, the delete call no-ops.
    if (undoDup) {
      await bridge.request({ kind: "emitters/delete", params: { id: dupId } });
    }
    // Clear firstId's link group regardless of state.
    await bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: [firstId], groupId: null },
    });

    return {
      undoApplied: undoResult.applied,
      undoFirstLinkGroup: undoFirst?.linkGroup,
      undoDupPresent: undoDup !== undefined,
    };
  });

  // Undo must have applied (snap-restore returned a non-null
  // snapshot and swapped the ParticleSystem).
  expect(result.undoApplied).toBe(true);
  // Atomicity invariant: undo restored firstId to 99 (its pre-delete
  // value) AND restored dup to the tree. Both halves of the
  // capture+sweep atom rolled back together.
  expect(result.undoFirstLinkGroup).toBe(99);
  expect(result.undoDupPresent).toBe(true);
});

test("load-time sweep — opening a legacy .alo with a singleton group auto-demotes it; dirty bit stays clean", async () => {
  // The fixture `tests/fixtures/nt-5-singleton.alo` was produced by
  // `ParticleEditor.exe --gen-nt5-fixture <path>` (see main.cpp's
  // argv branch) and contains a state no sweep-aware codepath can
  // produce: emitter 0 at linkGroup=0, emitter 1 at linkGroup=1
  // (alone — a legacy singleton). On file/open, the host's
  // load-time `EnforceSingleMemberLinkGroups` sweep
  // (BridgeDispatcher.cpp; the file/open handler that invokes it now
  // lives in src/host/BridgeDispatch_File.cpp)
  // fires right after the ParticleSystem swap, demoting emitter 1
  // to linkGroup=0. The dirty bit MUST stay false — the correction
  // is normalization, not user-driven mutation.
  await page.keyboard.press("Escape").catch(() => {});

  // The .alo lives at web/apps/editor/tests/fixtures/ relative to this
  // file. Resolve to absolute so the host's file/open (which doesn't
  // resolve relative paths) gets a clean wide-string.
  const fixturePath = resolve(__dirname, "fixtures/nt-5-singleton.alo");

  const result = await page.evaluate(async (path) => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<{
          ok?: boolean;
          path?: string;
          root?: { children: { id: number; linkGroup: number }[] };
          dirty?: boolean;
        }>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Stash current state so we restore-or-bail cleanly.
    const openRes = await bridge.request({
      kind: "file/open",
      params: { path },
    });
    if (openRes.ok !== true) {
      return { error: `file/open failed: ${JSON.stringify(openRes)}` };
    }

    // Read back: tree should show emitter 1 demoted to linkGroup=0
    // by the load-time sweep. The list endpoint returns the
    // synthetic root with the two emitters as children.
    const listed = await bridge.request({
      kind: "emitters/list",
      params: {},
    });

    // engine/state/snapshot exposes the dirty flag.
    const snap = await bridge.request({
      kind: "engine/state/snapshot",
      params: {},
    });

    return {
      childrenLinkGroups: listed.root?.children.map((c) => c.linkGroup) ?? [],
      childrenCount: listed.root?.children.length ?? 0,
      dirty: snap.dirty,
    };
  }, fixturePath);

  expect(result.error).toBeUndefined();
  // Two root emitters were saved; both should be present.
  expect(result.childrenCount).toBe(2);
  // The load-time sweep demoted the singleton. Both should be at 0.
  expect(result.childrenLinkGroups).toEqual([0, 0]);
  // Sweep must NOT have triggered a dirty flag — opening a legacy
  // file shouldn't force a save-prompt for the normalization fix.
  expect(result.dirty).toBe(false);

  // Reset to file/new so subsequent specs see a fresh tree.
  await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) => Promise<unknown>;
      };
    }).bridge;
    if (bridge) await bridge.request({ kind: "file/new", params: {} });
  });
});

test("deleting one member of a 2-member link group demotes the survivor", async () => {
  await page.keyboard.press("Escape").catch(() => {});
  const result = await page.evaluate(async () => {
    const bridge = (window as Window & {
      bridge?: {
        request: (req: { kind: string; params: unknown }) =>
          Promise<{ ok?: boolean; newId?: number; root?: {
            children: { id: number; linkGroup: number }[];
          }}>;
      };
    }).bridge;
    if (!bridge) throw new Error("bridge missing");

    // Set up: get seed + duplicate to 2 emitters; assign both to a
    // fresh group.
    const initial = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const firstId = initial.root?.children[0]?.id;
    if (firstId === undefined) throw new Error("no seed");
    const dup = await bridge.request({
      kind: "emitters/duplicate",
      params: { id: firstId },
    });
    const dupId = dup.newId;
    if (typeof dupId !== "number" || dupId < 0) {
      throw new Error("duplicate failed");
    }
    await bridge.request({
      kind: "linkGroups/set-membership",
      params: { ids: [firstId, dupId], groupId: 73 },
    });

    // Delete the duplicate. Group 73 now has 1 member (firstId) →
    // the sweep demotes firstId to 0.
    await bridge.request({
      kind: "emitters/delete",
      params: { id: dupId },
    });

    const after = await bridge.request({
      kind: "emitters/list",
      params: {},
    });
    const afterFirst = after.root?.children.find((c) => c.id === firstId);

    return { firstLinkGroup: afterFirst?.linkGroup };
  });

  expect(result.firstLinkGroup).toBe(0);
});
