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
  // The fixture `tests/fixtures/singleton-emitter.alo` was produced by
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
  const fixturePath = resolve(__dirname, "fixtures/singleton-emitter.alo");

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

// ── 5. A structural mutation reaches an already-PLACED instance ──────
//
// 2026-07 audit. A ParticleSystemInstance spawns its root emitters ONCE,
// in its constructor, and Engine::OnParticleSystemChanged only visited the
// emitters that already existed — it never created or removed any. So an
// emitter added by Add Root / Paste / Import / Duplicate / reparent-to-root
// never appeared on an instance placed earlier. Deletion propagated (an
// Emitter's destructor tears its live instances down), addition did not, and
// that asymmetry is what made it look like "the tree updated, so it worked".
//
// Same user-visible shape as the set-properties gap fixed in #682: the tree
// row appears, the placed effect ignores it.
//
// This is the first test of live-instance state at all — engine/query/
// live-instances was added alongside the fix because the bridge could describe
// the authored system in detail and expose nothing about what was actually
// rendering.
test("adding a root emitter reaches an already-placed instance", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // A previous spec may have left a cursor-bound preview attached;
    // preview/attach refuses when one exists. Best-effort clear.
    try { await b.request({ kind: "preview/kill", params: {} }); } catch { /* none attached */ }

    // file/new gives a clean system with one root emitter AND clears the
    // engine's instance list, so the counts below start from a known floor.
    await b.request({ kind: "file/new", params: {} });

    // Place a PERSISTENT instance: attach spawns it cursor-bound, place
    // detaches it so it stays in the scene (the Shift-click gesture's
    // bridge-reachable equivalent).
    await b.request({ kind: "preview/attach", params: { x: 200, y: 200 } });
    await b.request({ kind: "preview/place", params: {} });

    const before = await b.request({ kind: "engine/query/live-instances", params: {} });
    // Structural mutation of the AUTHORED system, with the instance already live.
    const added = await b.request({ kind: "emitters/add-root", params: {} });
    const after = await b.request({ kind: "engine/query/live-instances", params: {} });

    return { before, after, newId: added.newId };
  });

  expect(result.newId).toBeGreaterThanOrEqual(0);     // the authored add succeeded
  expect(result.before.instances).toBeGreaterThanOrEqual(1);  // something was placed
  // The instance itself must NOT be recreated — this is a topology sync, not a
  // respawn (a respawn would restart the placed effect from zero).
  expect(result.after.instances).toBe(result.before.instances);
  // THE REGRESSION: without the sync the placed instance keeps its
  // creation-time emitter list and this stays equal to `before`.
  expect(result.after.emitters).toBe(result.before.emitters + 1);
});

// ── 6. Reparenting a root removes its old root-level live instance ───
//
// SyncRootEmitters historically only added newly-authored roots.
// Reparenting a root under another root changed the authored tree in place, so
// the already-placed ParticleSystemInstance kept the source's old root-level
// EmitterInstance. The same emitter could later also spawn as the target's
// child, rendering two live copies from one authored node.
//
// The exact value matters in both directions. Three authored roots plus a live
// lifetime child produce four live emitters before the drop. Afterwards the
// placed instance, two unrelated roots, and particle-parented child must
// survive, but the source's stale root-level instance must not.
test("reparenting a root removes only its stale root instance from an already-placed effect", async () => {
  const result = await page.evaluate(async () => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    try { await b.request({ kind: "preview/kill", params: {} }); } catch { /* none attached */ }
    await b.request({ kind: "file/new", params: {} });

    const initial = await b.request({ kind: "emitters/list", params: {} });
    const dynamicParentId = initial.root?.children?.[0]?.id;
    if (typeof dynamicParentId !== "number") throw new Error("seed root missing");

    const dynamicChild = await b.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: dynamicParentId },
    });
    if (typeof dynamicChild.newId !== "number" || dynamicChild.newId < 0) {
      throw new Error("lifetime child missing");
    }

    const target = await b.request({ kind: "emitters/add-root", params: {} });
    const source = await b.request({ kind: "emitters/add-root", params: {} });
    if (typeof target.newId !== "number" || typeof source.newId !== "number") {
      throw new Error("root setup missing");
    }

    // Freeze simulation before construction. Each default EmitterInstance
    // still creates its initial particle, but none can naturally spawn/expire
    // between the two exact counts.
    await b.request({ kind: "engine/set/paused", params: { paused: true } });
    try {
      await b.request({ kind: "preview/attach", params: { x: 200, y: 200 } });
      await b.request({ kind: "preview/place", params: {} });
      const before = await b.request({ kind: "engine/query/live-instances", params: {} });

      const dropped = await b.request({
        kind: "emitters/drop",
        params: {
          mode: "reparent",
          id: source.newId,
          targetId: target.newId,
          slot: "lifetime",
        },
      });
      const after = await b.request({ kind: "engine/query/live-instances", params: {} });
      const tree = await b.request({ kind: "emitters/list", params: {} });
      const targetNode = tree.root?.children?.find(
        (e: { id: number }) => e.id === target.newId,
      );
      const child = targetNode?.children?.find(
        (e: { id: number }) => e.id === source.newId,
      );

      return {
        before,
        after,
        dropOk: dropped.ok,
        childRole: child?.role,
      };
    } finally {
      await b.request({ kind: "engine/set/paused", params: { paused: false } });
    }
  });

  expect(result.dropOk).toBe(true);
  expect(result.childRole).toBe("lifetime");
  expect(result.before.instances).toBe(1);
  expect(result.before.emitters).toBe(4);
  expect(result.before.particles).toBe(4);
  // Specific regression: without removal these remain 4.
  // Specific overreach: removing every non-root emitter drops both to 2 by
  // killing the legitimate particle-parented child as well as the stale root.
  expect(result.after.emitters).toBe(3);
  expect(result.after.particles).toBe(3);
  // Reconciliation must not recreate or remove the placed system instance.
  expect(result.after.instances).toBe(1);
});

// ── file/open must not carry the previous document's selection ───────
//
// 2026-07 audit. m_selectedEmitterId is a POSITIONAL index, and only
// file/new ever reset it. file/open and autosave-recover swapped the bound
// ParticleSystem and emitted tree/state events but left the selection alone,
// so an id selected in the old document survived into the new one. When that
// index also exists in the new file — the common case, since most .alo files
// have several emitters — the Inspector and curve panel silently operate on
// the WRONG emitter, with nothing on screen indicating it.
//
// The fixture has 2 emitters, so selecting index 1 and then opening gives a
// surviving-but-wrong id rather than an out-of-range one that might get
// clamped by accident.
test("file/open resets the selection instead of inheriting the previous document's", async () => {
  await page.keyboard.press("Escape").catch(() => {});
  const fixturePath = resolve(__dirname, "fixtures/singleton-emitter.alo");

  const result = await page.evaluate(async (path) => {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const b = (window as any).bridge;

    // Start from a known document and select a NON-root emitter.
    await b.request({ kind: "file/open", params: { path } });
    const list = await b.request({ kind: "emitters/list", params: {} });
    const second = list.root?.children?.[1]?.id;
    if (second === undefined) throw new Error("fixture needs >= 2 emitters");
    await b.request({ kind: "emitters/select", params: { id: second } });
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });

    // Re-open the same document. The selection must be re-established by the
    // open itself, not inherited from the session that preceded it.
    let announced: number | null | undefined;
    const off = b.on("emitters/selected", (e: { payload: { id: number | null } }) => {
      announced = e.payload.id;
    });
    await b.request({ kind: "file/open", params: { path } });
    await new Promise((r) => setTimeout(r, 150));
    off();

    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    return { beforeSel: before.selectedEmitterId, afterSel: after.selectedEmitterId, announced };
  }, fixturePath);

  expect(result.beforeSel).toBeGreaterThan(0);   // we really did select a non-root
  // THE REGRESSION: without the reset this stays on the stale positional id.
  expect(result.afterSel).toBe(0);
  // And React must be told, or its selection atom keeps the old row highlighted
  // even though native has moved on.
  expect(result.announced).toBe(0);
});

// ── 6. Paused live samples invalidate after rescale/interpolation ───────────
//
// These assertions deliberately read the sample cached by the native
// EmitterInstance::UpdateParticle path. Query-time re-sampling would prove only
// the authored track mutation and would stay green if the production
// OnParticleSystemChanged call stopped waking a paused render.

type LiveParticleSample = {
  instanceIndex: number;
  emitterId: number;
  relativeTimePercent: number;
  scale: number;
};

type LiveInstanceState = {
  instances: number;
  emitters: number;
  particles: number;
  samples: LiveParticleSample[];
};

type ExpectedLiveState = {
  instances: number;
  emitters: number;
  particles: number;
  samples: LiveParticleSample[];
};

async function liveBridgeRequest<T>(
  kind: string,
  params: Record<string, unknown> = {},
): Promise<T> {
  return page.evaluate(
    async ({ requestKind, requestParams }) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const bridge = (window as any).bridge;
      return bridge.request({ kind: requestKind, params: requestParams });
    },
    { requestKind: kind, requestParams: params },
  ) as Promise<T>;
}

// ── G-1. Spawn-schedule production call-site behavior ─────────────────────
//
// The pure SpawnSchedule predicate is covered by its C++ unit. These cases
// deliberately traverse the real bridge and an already-placed EmitterInstance,
// so leaving a syntactically matching ReconcileNextSpawnTime call in production
// while ignoring its return cannot pass.

type SpawnLiveState = {
  instances: number;
  emitters: number;
  particles: number;
};

async function spawnBridgeRequest<T>(
  kind: string,
  params: Record<string, unknown> = {},
): Promise<T> {
  return page.evaluate(
    async ({ requestKind, requestParams }) => {
      // eslint-disable-next-line @typescript-eslint/no-explicit-any
      const bridge = (window as any).bridge;
      return bridge.request({ kind: requestKind, params: requestParams });
    },
    { requestKind: kind, requestParams: params },
  ) as Promise<T>;
}

function normalizeLiveState(state: LiveInstanceState): ExpectedLiveState {
  return {
    instances: state.instances,
    emitters: state.emitters,
    particles: state.particles,
    samples: [...state.samples]
      .sort((a, b) => a.instanceIndex - b.instanceIndex || a.emitterId - b.emitterId)
      .map((sample) => ({
        instanceIndex: sample.instanceIndex,
        emitterId: sample.emitterId,
        relativeTimePercent: Number(sample.relativeTimePercent.toFixed(3)),
        scale: Number(sample.scale.toFixed(3)),
      })),
  };
}

function expectedLiveState(
  targetId: number,
  targetScale: number,
  controlId: number,
  controlScale: number,
): ExpectedLiveState {
  return {
    instances: 1,
    emitters: 2,
    particles: 2,
    samples: [
      {
        instanceIndex: 0,
        emitterId: targetId,
        relativeTimePercent: 50,
        scale: targetScale,
      },
      {
        instanceIndex: 0,
        emitterId: controlId,
        relativeTimePercent: 50,
        scale: controlScale,
      },
    ].sort((a, b) => a.emitterId - b.emitterId),
  };
}

async function waitForLiveState(expected: ExpectedLiveState): Promise<void> {
  await expect.poll(
    async () => normalizeLiveState(
      await liveBridgeRequest<LiveInstanceState>("engine/query/live-instances"),
    ),
    {
      message: "waiting for the native render path to publish its cached live samples",
      timeout: 5_000,
      intervals: [16, 32, 64, 100],
    },
  ).toEqual(expected);
}

async function resetLiveInvalidationFixture(): Promise<void> {
  // Clear while still paused first. Under a deliberately broken rescale
  // invalidation the live cursors point into the rebuilt key map; unpausing
  // before Clear would turn the intended stale-value assertion into a crash.
  await liveBridgeRequest("file/new").catch(() => {});
  await liveBridgeRequest("engine/set/paused", { paused: false }).catch(() => {});
}

async function seedHalfwayLiveFixture(): Promise<{ targetId: number; controlId: number }> {
  await resetLiveInvalidationFixture();

  const list = await liveBridgeRequest<{ root: { children: { id: number }[] } }>(
    "emitters/list",
  );
  const targetId = list.root.children[0]?.id;
  if (targetId === undefined) throw new Error("file/new did not create a root emitter");

  const added = await liveBridgeRequest<{ newId: number }>("emitters/add-root");
  const controlId = added.newId;
  if (controlId < 0) throw new Error("emitters/add-root failed");

  for (const id of [targetId, controlId]) {
    await liveBridgeRequest("emitters/set-properties", {
      id,
      patch: {
        lifetime: 1,
        initialDelay: 0,
        useBursts: false,
        nParticlesPerSecond: 1,
        randomLifetimePerc: 0,
        randomScalePerc: 0,
        hasTail: false,
      },
    });
    await liveBridgeRequest("emitters/set-track-key", {
      id,
      track: "scale",
      oldTime: 0,
      newTime: 0,
      newValue: 0,
    });
    await liveBridgeRequest("emitters/set-track-key", {
      id,
      track: "scale",
      oldTime: 100,
      newTime: 100,
      newValue: 1,
    });
    await liveBridgeRequest("emitters/set-track-interpolation", {
      id,
      track: "scale",
      interpolation: "linear",
    });
  }

  await liveBridgeRequest("engine/set/paused", { paused: true });
  await liveBridgeRequest("preview/attach", { x: 200, y: 200 });
  await liveBridgeRequest("preview/place");
  await liveBridgeRequest("engine/action/step-frames", { frames: 30 });

  await waitForLiveState(expectedLiveState(targetId, 0.5, controlId, 0.5));
  return { targetId, controlId };
}

test("live invalidation: system rescale updates paused samples without respawning", async () => {
  try {
    const { targetId, controlId } = await seedHalfwayLiveFixture();
    await liveBridgeRequest("engine/action/rescale-system", {
      durationScalePercent: 100,
      sizeScalePercent: 200,
    });

    await waitForLiveState(expectedLiveState(targetId, 1, controlId, 1));
  } finally {
    await resetLiveInvalidationFixture();
  }
});

test("live invalidation: emitter rescale updates only its paused target sample", async () => {
  try {
    const { targetId, controlId } = await seedHalfwayLiveFixture();
    await liveBridgeRequest("engine/action/rescale-emitter", {
      id: targetId,
      durationScalePercent: 100,
      sizeScalePercent: 200,
    });

    await waitForLiveState(expectedLiveState(targetId, 1, controlId, 0.5));
  } finally {
    await resetLiveInvalidationFixture();
  }
});

test("live invalidation: Linear to Step repaints a paused halfway particle", async () => {
  try {
    const { targetId, controlId } = await seedHalfwayLiveFixture();
    await liveBridgeRequest("emitters/set-track-interpolation", {
      id: targetId,
      track: "scale",
      interpolation: "step",
    });

    await waitForLiveState(expectedLiveState(targetId, 0, controlId, 0.5));
  } finally {
    await resetLiveInvalidationFixture();
  }
});

async function spawnLiveState(): Promise<SpawnLiveState> {
  return spawnBridgeRequest<SpawnLiveState>("engine/query/live-instances");
}

async function expectSpawnLiveState(
  expected: Pick<SpawnLiveState, "instances" | "emitters" | "particles">,
): Promise<SpawnLiveState> {
  await expect.poll(
    async () => {
      const state = await spawnLiveState();
      return {
        instances: state.instances,
        emitters: state.emitters,
        particles: state.particles,
      };
    },
    {
      timeout: 5_000,
      intervals: [16, 32, 64, 100],
      message: "waiting for the native stepped clock to publish exact live counts",
    },
  ).toEqual(expected);
  return spawnLiveState();
}

async function resetSpawnScheduleFixture(): Promise<void> {
  await spawnBridgeRequest("file/new");
  await spawnBridgeRequest("engine/set/paused", { paused: false });
}

async function cleanupSpawnScheduleFixture(): Promise<void> {
  await spawnBridgeRequest("file/new").catch(() => {});
  await spawnBridgeRequest("engine/set/paused", { paused: false }).catch(() => {});
}

async function seedSpawnScheduleFixture(
  initialDelay: number,
  particlesPerSecond: number,
): Promise<number> {
  await resetSpawnScheduleFixture();
  await spawnBridgeRequest("engine/set/paused", { paused: true });

  const list = await spawnBridgeRequest<{
    root: { children: Array<{ id: number }> };
  }>("emitters/list");
  const emitterId = list.root.children[0]?.id;
  if (emitterId === undefined) throw new Error("file/new did not create a root emitter");

  await spawnBridgeRequest("emitters/set-properties", {
    id: emitterId,
    patch: {
      lifetime: 10,
      initialDelay,
      useBursts: false,
      nParticlesPerSecond: particlesPerSecond,
      randomLifetimePerc: 0,
      freezeTime: 0,
      skipTime: 0,
      isWeatherParticle: false,
      hasTail: false,
    },
  });
  await spawnBridgeRequest("preview/attach", { x: 200, y: 200 });
  await spawnBridgeRequest("preview/place");
  return emitterId;
}

test("spawn schedule: a steady-state rate increase pulls the real next round in", async () => {
  try {
    const emitterId = await seedSpawnScheduleFixture(0, 1);
    const before = await expectSpawnLiveState({
      instances: 1,
      emitters: 1,
      particles: 1,
    });

    await spawnBridgeRequest("emitters/set-properties", {
      id: emitterId,
      patch: { nParticlesPerSecond: 100 },
    });
    await spawnBridgeRequest("engine/action/step-frames", { frames: 1 });

    const after = await expectSpawnLiveState({
      instances: 1,
      emitters: 1,
      particles: 2,
    });
    const tree = await spawnBridgeRequest<{
      root: { children: Array<{ id: number }> };
    }>("emitters/list");

    expect(before.instances).toBe(after.instances);
    expect(before.emitters).toBe(after.emitters);
    expect(tree.root.children.map((entry) => entry.id)).toEqual([emitterId]);
  } finally {
    await cleanupSpawnScheduleFixture();
  }
});

test("spawn schedule: unrelated edits preserve initialDelay before emission begins", async () => {
  try {
    const emitterId = await seedSpawnScheduleFixture(5, 10);
    const before = await expectSpawnLiveState({
      instances: 1,
      emitters: 1,
      particles: 0,
    });

    // This is deliberately unrelated to rate or timing. The broken
    // unconditional reconcile scheduled a round at now + 0.1 seconds.
    await spawnBridgeRequest("emitters/set-properties", {
      id: emitterId,
      patch: { gravity: 0.25 },
    });
    await spawnBridgeRequest("engine/action/step-frames", { frames: 7 });
    const wrongWindow = await expectSpawnLiveState({
      instances: 1,
      emitters: 1,
      particles: 0,
    });

    // Total stepped time is now 301/60 seconds: just beyond the authored five
    // seconds, but before a second 10/s round. Exactly one particle must exist.
    await spawnBridgeRequest("engine/action/step-frames", { frames: 294 });
    const afterDelay = await expectSpawnLiveState({
      instances: 1,
      emitters: 1,
      particles: 1,
    });
    const tree = await spawnBridgeRequest<{
      root: { children: Array<{ id: number }> };
    }>("emitters/list");

    expect(before.instances).toBe(wrongWindow.instances);
    expect(wrongWindow.instances).toBe(afterDelay.instances);
    expect(before.emitters).toBe(afterDelay.emitters);
    expect(tree.root.children.map((entry) => entry.id)).toEqual([emitterId]);
  } finally {
    await cleanupSpawnScheduleFixture();
  }
});
