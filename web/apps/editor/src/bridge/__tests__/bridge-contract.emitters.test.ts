// Contract tests for MockBridge — the emitter tree: list/select/duplicate/delete/move/drop/clipboard/properties.
// Split by namespace from the former single bridge-contract.test.ts. Each
// request is exercised end-to-end: request → store mutation → event →
// follow-up read, keeping the schema and the MockBridge implementation honest.

import { describe, it, expect, beforeEach } from "vitest";
import { MockBridge } from "../mock";
import {
  useMockEmitterTree,
  useMockEngineState,
  makeDefaultEngineState,
} from "../mock-state";
import type {
  EmitterTreeDto,
  EmitterTreeNode,
  Request,
  ResponseFor,
} from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import { resetMockState } from "@/test/mock-state";

// Reset the shared mock stores between tests so state mutations don't leak.
beforeEach(resetMockState);

describe("MockBridge contract — emitters/*", () => {
  it("types emitters/import-from-file like the native success payload", () => {
    type ImportFromFileRequest = Extract<Request, { kind: "emitters/import-from-file" }>;
    const response = { ok: true, imported: 1 } satisfies ResponseFor<ImportFromFileRequest>;
    expect(response).toEqual({ ok: true, imported: 1 });
  });

  it("rejects unimplemented emitters/* requests (mutations) as not implemented", async () => {
    const b = new MockBridge();
    // emitters/import-from-file has a native handler (and the emitter-import
    // a11y spec) but needs the real FileManager, so browser mode rejects it.
    await expect(b.request({ kind: "emitters/import-from-file", params: { path: "x.alo", selected: [0] } }))
      .rejects.toThrow(/not implemented/);
  });

  it("emitters/preview-from-file returns a mock tree for any path", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/preview-from-file",
      params: { path: "/anywhere/foo.alo" },
    });
    expect(r.ok).toBe(true);
    if (r.ok) {
      expect(r.tree).toBeDefined();
      expect(r.tree.children.length).toBeGreaterThanOrEqual(3);
      // Names from the mock tree.
      const names = r.tree.children.map((c) => c.name);
      expect(names).toEqual(expect.arrayContaining(["Smoke", "Sparks", "Flash"]));
    }
  });

  // ─── emitter tree + selection ──────────────────
  it("emitters/list returns the fixture tree with the synthetic root + populated real roots", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    // Synthetic root has id=-1 and is the only level the rest descends from.
    expect(tree.root.id).toBe(-1);
    expect(tree.root.role).toBe("root");
    // Three real roots from the fixture.
    expect(tree.root.children).toHaveLength(3);
    const names = tree.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Sparks", "Flash"]);
    // Smoke has one lifetime child + one death child.
    const smoke = tree.root.children[0];
    expect(smoke.linkGroup).toBe(1);
    expect(smoke.children).toHaveLength(2);
    expect(smoke.children[0].role).toBe("lifetime");
    expect(smoke.children[1].role).toBe("death");
    // Sparks has just a lifetime child.
    const sparks = tree.root.children[1];
    expect(sparks.children).toHaveLength(1);
    expect(sparks.children[0].role).toBe("lifetime");
    // Flash is a bare leaf with no link group.
    const flash = tree.root.children[2];
    expect(flash.children).toHaveLength(0);
    expect(flash.linkGroup).toBe(0);
    // All emitters in the fixture are visible.
    expect(smoke.visible).toBe(true);
  });

  it("emitters/select updates snapshot.selectedEmitterId and fires emitters/selected", async () => {
    const b = new MockBridge();
    let lastEvent: { id: number | null } | null = null;
    const off = b.on("emitters/selected", (e) => { lastEvent = e.payload; });

    // Initial snapshot: the default root emitter (id 0) is selected on boot
    // (legacy parity — see makeDefaultEngineState in mock-state).
    const before = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(before.selectedEmitterId).toBe(0);

    // Select a different emitter (Smoke embers, id=1) — proves select changes
    // the scalar AND fires emitters/selected.
    await b.request({ kind: "emitters/select", params: { id: 1 } });
    expect(lastEvent).not.toBeNull();
    expect(lastEvent!.id).toBe(1);
    const after = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(after.selectedEmitterId).toBe(1);

    // Selecting null clears.
    await b.request({ kind: "emitters/select", params: { id: null } });
    expect(lastEvent!.id).toBeNull();
    const cleared = await b.request({ kind: "engine/state/snapshot", params: {} });
    expect(cleared.selectedEmitterId).toBeNull();
    off();
  });

  // ─── emitter mutations + dialogs ─────
  it("emitters/duplicate clones the emitter as a new root and returns ok:true + newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    const r = await b.request({ kind: "emitters/duplicate", params: { id: 0 } });
    if (!r.ok) throw new Error("expected ok:true");
    expect(r.newId).toBeGreaterThan(0);

    expect(lastTree).not.toBeNull();
    // Synthetic root now has 4 children (Smoke / Sparks / Flash / duplicate).
    expect(lastTree!.root.children).toHaveLength(4);
    // The duplicate inherits the source name with a `_<n>` suffix.
    const dup = lastTree!.root.children[3];
    expect(dup.name.startsWith("Smoke_")).toBe(true);
    off();
  });

  it("emitters/delete removes the emitter + subtree and clears selection if it was selected", async () => {
    const b = new MockBridge();
    // Pre-select the target so we observe the auto-clear.
    await b.request({ kind: "emitters/select", params: { id: 0 } });

    let lastTree: EmitterTreeDto | null = null;
    let lastSelected: number | null | "untouched" = "untouched";
    const offTree = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    const offSel  = b.on("emitters/selected",     (e) => { lastSelected = e.payload.id; });

    await b.request({ kind: "emitters/delete", params: { id: 0 } });

    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children).toHaveLength(2);  // Smoke + its 2 kids gone
    expect(lastTree!.root.children.map((c) => c.name)).toEqual(["Sparks", "Flash"]);
    expect(lastSelected).toBeNull();
    offTree();
    offSel();
  });

  it("emitters/rename updates the node name in the tree", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });

    await b.request({
      kind: "emitters/rename",
      params: { id: 0, name: "Smoke (renamed)" },
    });
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children[0].name).toBe("Smoke (renamed)");
    off();
  });

  it("emitters/duplicate-with-index-increment returns the new id and clones the subtree", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/duplicate-with-index-increment",
      params: { id: 0, delta: 5 },
    });
    expect(r.newId).toBeGreaterThan(0);
    // Snapshot the tree and find the duplicate; its name carries the
    // delta marker so we can assert the round-trip preserved `delta`.
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const dup = tree.root.children[tree.root.children.length - 1];
    expect(dup.name).toContain("(+5)");
  });

  it("emitters/duplicate-with-index-increment-many returns N chained ids in one call", async () => {
    const b = new MockBridge();
    const before = await b.request({ kind: "emitters/list", params: {} });
    const beforeCount = before.root.children.length;

    const r = await b.request({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: 0, delta: 2, count: 3 },
    });
    expect(Array.isArray(r.newIds)).toBe(true);
    expect(r.newIds).toHaveLength(3);
    for (const id of r.newIds) expect(id).toBeGreaterThan(0);
    expect(new Set(r.newIds).size).toBe(3); // three distinct copies

    // Three new emitters were added under the (root) source in one call.
    const after = await b.request({ kind: "emitters/list", params: {} });
    expect(after.root.children.length).toBe(beforeCount + 3);

    // Pin CHAINING (not 3 independent copies of the source): the mock tags each
    // duplicate's name with a `(+delta)` marker, and because each copy is made
    // from the PREVIOUS copy, the marker compounds — the 3 copies (in creation
    // order) carry 1, 2, then 3 markers. Copying the original 3× would give 1
    // marker each. This is the load-bearing #575 behavior (the index climbs).
    const marks = (name: string) => (name.match(/\(\+2\)/g) ?? []).length;
    const copies = after.root.children.slice(beforeCount); // the 3 new copies
    expect(copies.map((c) => marks(c.name))).toEqual([1, 2, 3]);
  });

  // ─── add-child + move + link-group membership ─

  it("emitters/add-lifetime-child adds a lifetime child under the parent and returns its newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Flash (id=5) starts as a leaf — no children. Adding a lifetime
    // child should succeed and produce a child with role: "lifetime".
    const r = await b.request({
      kind: "emitters/add-lifetime-child",
      params: { parentId: 5 },
    });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    const flash = lastTree!.root.children[2];
    expect(flash.children).toHaveLength(1);
    expect(flash.children[0].role).toBe("lifetime");
    expect(flash.children[0].id).toBe(r.newId);
    off();
  });

  it("emitters/add-root appends a new empty root emitter and returns its newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    const before = await b.request({ kind: "emitters/list", params: {} });
    const rootCountBefore = before.root.children.length;
    const r = await b.request({ kind: "emitters/add-root", params: {} });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children).toHaveLength(rootCountBefore + 1);
    // New root is appended at the end with role: "root", empty name,
    // no children, link group 0.
    const last = lastTree!.root.children[lastTree!.root.children.length - 1]!;
    expect(last.id).toBe(r.newId);
    expect(last.role).toBe("root");
    expect(last.children).toHaveLength(0);
    expect(last.linkGroup).toBe(0);
    off();
  });

  it("emitters/add-death-child adds a death child under the parent and returns its newId", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Sparks (id=3) has one lifetime child but no death child. Add a
    // death child and verify role + position (death child renders
    // after lifetime).
    const r = await b.request({
      kind: "emitters/add-death-child",
      params: { parentId: 3 },
    });
    expect(r.newId).toBeGreaterThan(0);
    expect(lastTree).not.toBeNull();
    const sparks = lastTree!.root.children[1];
    expect(sparks.children).toHaveLength(2);
    expect(sparks.children[1].role).toBe("death");
    expect(sparks.children[1].id).toBe(r.newId);
    off();
  });

  it("emitters/move swaps adjacent roots", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture roots in order: Smoke(0), Sparks(3), Flash(5). Move
    // Sparks (middle) up — should swap with Smoke.
    await b.request({
      kind: "emitters/move",
      params: { id: 3, direction: "up" },
    });
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Sparks", "Smoke", "Flash"]);
    off();
  });

  it("emitters/set-visible flips a single emitter's visible flag", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    await b.request({
      kind: "emitters/set-visible",
      params: { id: 0, visible: false },
    });
    expect(lastTree).not.toBeNull();
    const smoke = lastTree!.root.children.find((c) => c.id === 0)!;
    expect(smoke.visible).toBe(false);
    // Siblings unchanged.
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    expect(sparks.visible).toBe(true);
    off();
  });

  it("emitters/set-all-visible recurses through the tree", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    await b.request({
      kind: "emitters/set-all-visible",
      params: { visible: false },
    });
    expect(lastTree).not.toBeNull();
    // Every emitter — roots + children of every depth — should be hidden.
    const allHidden = (n: EmitterTreeNode): boolean =>
      n.visible === false && n.children.every(allHidden);
    expect(lastTree!.root.children.every(allHidden)).toBe(true);

    // Bulk-show puts them all back.
    await b.request({
      kind: "emitters/set-all-visible",
      params: { visible: true },
    });
    const allVisible = (n: EmitterTreeNode): boolean =>
      n.visible === true && n.children.every(allVisible);
    expect(lastTree!.root.children.every(allVisible)).toBe(true);
    off();
  });

  // ─── drag/drop reorder + reparent ────────────

  it("emitters/drop { mode: 'reorder' } reorders the fixture roots", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture roots: Smoke(0), Sparks(3), Flash(5). Drop Smoke at
    // gap 3 (after Flash) → expected post-order [Sparks, Flash, Smoke].
    const r = await b.request({
      kind: "emitters/drop",
      params: { mode: "reorder", id: 0, rootIndex: 3 },
    });
    expect(r.ok).toBe(true);
    expect(lastTree).not.toBeNull();
    const namesAfter = lastTree!.root.children.map((c) => c.name);
    expect(namesAfter).toEqual(["Sparks", "Flash", "Smoke"]);
    off();
  });

  it("emitters/drop { mode: 'reparent' } reparents under the target in the named slot", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Fixture: Flash (id=5) is an unlinked leaf with NO children;
    // Sparks (id=3) is a root with only a lifetime child. Reparent
    // Flash under Sparks in the death slot — Sparks should gain Flash
    // as a death child; the root list should shrink by one.
    const r = await b.request({
      kind: "emitters/drop",
      params: { mode: "reparent", id: 5, targetId: 3, slot: "death" },
    });
    expect(r.ok).toBe(true);
    expect(lastTree).not.toBeNull();
    // Flash should no longer be a root.
    const rootIds = lastTree!.root.children.map((c) => c.id);
    expect(rootIds).not.toContain(5);
    // Sparks should now have Flash as a death-role child.
    const sparks = lastTree!.root.children.find((c) => c.id === 3)!;
    const flashUnderSparks = sparks.children.find((c) => c.id === 5);
    expect(flashUnderSparks).toBeDefined();
    expect(flashUnderSparks!.role).toBe("death");
    off();
  });

  // ─── clipboard (copy / cut / paste) ───────────

  it("emitters/copy stashes the named subtrees and emits no tree change", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Copy Smoke (id=0) — should NOT emit tree/changed (read-only op).
    const r = await b.request({ kind: "emitters/copy", params: { ids: [0] } });
    expect(r).toEqual({});
    expect(lastTree).toBeNull();
    // Tree is untouched.
    const after = await b.request({ kind: "emitters/list", params: {} });
    expect(after.root.children.map((c) => c.name)).toEqual(["Smoke", "Sparks", "Flash"]);
    off();
  });

  it("emitters/cut serialises + deletes + a follow-up paste restores the subtree as a new root", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Cut Sparks (id=3) — has a lifetime child (id=4). After cut, the
    // root list should be [Smoke, Flash] (Sparks gone), with the
    // clipboard holding the Sparks subtree.
    await b.request({ kind: "emitters/cut", params: { ids: [3] } });
    expect(lastTree).not.toBeNull();
    expect(lastTree!.root.children.map((c) => c.name)).toEqual(["Smoke", "Flash"]);
    // Paste back — clipboard buffer is consumed by reference (deep
    // clone on every set), so a fresh paste should produce a Sparks
    // subtree with fresh ids appended at the end of roots.
    const r = await b.request({ kind: "emitters/paste", params: {} });
    expect(r.newIds).toHaveLength(1);
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Flash", "Sparks"]);
    // The pasted Sparks should carry the child (Spark trail).
    const pastedSparks = lastTree!.root.children.find((c) => c.name === "Sparks")!;
    expect(pastedSparks.children).toHaveLength(1);
    expect(pastedSparks.children[0]!.name).toBe("Spark trail");
    // Pasted ids are fresh (above the pre-cut max=5).
    expect(pastedSparks.id).toBeGreaterThan(5);
    off();
  });

  it("emitters/paste with afterId splices the pasted roots after the named root", async () => {
    const b = new MockBridge();
    let lastTree: EmitterTreeDto | null = null;
    const off = b.on("emitters/tree/changed", (e) => { lastTree = e.payload; });
    // Copy Flash (id=5), then paste with afterId=0 (Smoke). Expected
    // root order after paste: [Smoke, Flash-copy, Sparks, Flash].
    await b.request({ kind: "emitters/copy", params: { ids: [5] } });
    await b.request({ kind: "emitters/paste", params: { afterId: 0 } });
    expect(lastTree).not.toBeNull();
    const names = lastTree!.root.children.map((c) => c.name);
    expect(names).toEqual(["Smoke", "Flash", "Sparks", "Flash"]);
    off();
  });

  // ─── emitters/get-properties + set-properties

  it("emitters/get-properties returns a full EmitterPropertiesDto with Basic/Appearance/Physics fields", async () => {
    const b = new MockBridge();
    const r = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(r).toHaveProperty("properties");
    const p = r.properties;
    // Basic fields
    expect(p).toHaveProperty("name");
    expect(p).toHaveProperty("lifetime");
    expect(p).toHaveProperty("useBursts");
    expect(p).toHaveProperty("nParticlesPerSecond");
    expect(p).toHaveProperty("randomRotation");
    expect(p).toHaveProperty("parentLinkStrength");
    expect(p).toHaveProperty("index");
    // Appearance fields
    expect(p).toHaveProperty("colorTexture");
    expect(p).toHaveProperty("blendMode");
    expect(p).toHaveProperty("hasTail");
    expect(p).toHaveProperty("randomColors");
    expect(p.randomColors).toHaveLength(4);
    // Physics fields
    expect(p).toHaveProperty("acceleration");
    expect(p.acceleration).toHaveLength(3);
    expect(p).toHaveProperty("gravity");
    expect(p).toHaveProperty("groundBehavior");
    expect(p).toHaveProperty("weatherCubeSize");
    // Groups
    expect(p).toHaveProperty("groups");
    expect(p.groups).toHaveLength(3);
    expect(p.groups[0]).toHaveProperty("type");
    expect(p.groups[0]).toHaveProperty("min");
    expect(p.groups[0].min).toHaveLength(3);
  });

  it("emitters/set-properties applies a partial patch that is observable via get-properties", async () => {
    const b = new MockBridge();
    // Verify baseline differs from the patch values so the round-trip
    // assertion is meaningful.
    const before = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(before.properties.lifetime).not.toBe(5.0);
    expect(before.properties.useBursts).toBe(false);

    await b.request({
      kind: "emitters/set-properties",
      params: { id: 0, patch: { lifetime: 5.0, useBursts: true, nBursts: 3 } },
    });

    const after = await b.request({
      kind: "emitters/get-properties",
      params: { id: 0 },
    });
    expect(after.properties.lifetime).toBe(5.0);
    expect(after.properties.useBursts).toBe(true);
    expect(after.properties.nBursts).toBe(3);
    // Untouched fields stay at their fixture values.
    expect(after.properties.nParticlesPerSecond).toBe(before.properties.nParticlesPerSecond);
  });
});

describe("MockBridge emitters/move-many (preserve order at the edge)", () => {
  function rootsTree(names: string[]): EmitterTreeDto {
    return {
      root: {
        id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN,
        children: names.map((name, i) => ({
          id: i, stableId: 100 + i, name, role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [],
        })),
      },
    };
  }
  const rootNames = () =>
    useMockEmitterTree.getState().tree.root.children.map((c) => c.name);

  it("moves a contiguous block together", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/move-many", params: { ids: [1, 2], direction: "up" } });
    expect(rootNames()).toEqual(["B", "C", "A", "D"]);
    expect(r.newIds).toEqual([1, 2]); // mock keeps ids stable through a move
  });

  it("freezes a contiguous selection pinned at the top edge", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [0, 1], direction: "up" } });
    expect(rootNames()).toEqual(["A", "B", "C", "D"]); // nothing moved
  });

  it("freezes a NON-contiguous selection whose lead is pinned (preserve, no compacting)", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [0, 2], direction: "up" } });
    // A is pinned at the top → the whole selection freezes. (Compacting would
    // have produced ["A", "C", "B", "D"]; order is preserved instead.)
    expect(rootNames()).toEqual(["A", "B", "C", "D"]);
  });

  it("translates a non-contiguous selection that is not edge-pinned", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [1, 3], direction: "up" } });
    expect(rootNames()).toEqual(["B", "A", "D", "C"]);
  });

  it("freezes at the bottom edge for move down", async () => {
    useMockEmitterTree.setState({ tree: rootsTree(["A", "B", "C", "D"]) });
    const b = new MockBridge();
    await b.request({ kind: "emitters/move-many", params: { ids: [2, 3], direction: "down" } });
    expect(rootNames()).toEqual(["A", "B", "C", "D"]);
  });
});

describe("MockBridge stableId semantics (reorder-glide identity contract)", () => {
  // The glide keys React rows + FLIP maps by stableId. Three invariants the
  // host (ParticleSystem.cpp ctors/copySharedParamsFrom) and the mock must
  // both uphold; the mock is the only automatable side, so pin it here:
  //   presence+uniqueness, stability across reorders, freshness on copies.
  const flatten = (n: EmitterTreeNode): Array<[string, number]> =>
    [[n.name, n.stableId] as [string, number], ...n.children.flatMap(flatten)];
  const stableIdsByName = () =>
    new Map(useMockEmitterTree.getState().tree.root.children.flatMap(flatten));
  const allStableIds = () =>
    flatten(useMockEmitterTree.getState().tree.root).map(([, s]) => s);

  it("emitters/list: synthetic root has stableId 0; every node carries a unique positive stableId", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    expect(tree.root.stableId).toBe(0);
    const ids = tree.root.children.flatMap(flatten).map(([, s]) => s);
    expect(ids.every((s) => s > 0)).toBe(true);
    expect(new Set(ids).size).toBe(ids.length);
  });

  it("stableIds survive reorder-many and move-many (same emitter, same id)", async () => {
    const b = new MockBridge();
    const before = stableIdsByName();
    await b.request({ kind: "emitters/reorder-many", params: { ids: [5], rootIndex: 0 } });
    await b.request({ kind: "emitters/move-many", params: { ids: [3], direction: "down" } });
    const after = stableIdsByName();
    for (const [name, sid] of before) expect(after.get(name), name).toBe(sid);
  });

  it("duplicate-many yields FRESH stableIds; the tree stays unique", async () => {
    const b = new MockBridge();
    const before = new Set(allStableIds());
    const r = await b.request({ kind: "emitters/duplicate-many", params: { ids: [0] } });
    expect(r.ok).toBe(true);
    const after = allStableIds();
    expect(new Set(after).size).toBe(after.length); // still unique
    const fresh = after.filter((s) => !before.has(s));
    expect(fresh.length).toBeGreaterThan(0); // the copies got new identities
  });
});

describe("MockBridge dirty-bit for batch structural mutations", () => {
  // isMutating must flag emitters/move-many and emitters/duplicate-many so the
  // mock's dirty-bit / save-prompt gate matches the native host's markDirty
  // rule. Both shipped earlier but were missing from the allowlist, so a
  // multi-select move/duplicate left the document falsely clean in the mock.
  it("emitters/move-many marks the document dirty", async () => {
    const b = new MockBridge();
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
    // Default fixture roots: Smoke(0), Sparks(3), Flash(5). Moving Smoke down
    // is a real structural mutation.
    await b.request({ kind: "emitters/move-many", params: { ids: [0], direction: "down" } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(true);
  });

  it("emitters/duplicate-many marks the document dirty", async () => {
    const b = new MockBridge();
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
    await b.request({ kind: "emitters/duplicate-many", params: { ids: [0] } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(true);
  });

  // The mock previously fired markDirty UNCONDITIONALLY for any mutating
  // kind, so a REFUSED or NO-OP drag-commit (which the native host leaves
  // clean, marking dirty only on a real mutation) falsely dirtied the doc.
  it("a REFUSED emitters/drop (own-footprint reorder) leaves the document clean", async () => {
    const b = new MockBridge();
    // Smoke is root 0; dropping it at root index 0 is its own position → refused.
    const r = await b.request({ kind: "emitters/drop", params: { mode: "reorder", id: 0, rootIndex: 0 } });
    expect((r as { ok: boolean }).ok).toBe(false);
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });

  it("a REFUSED emitters/reorder-many (own-footprint) leaves the document clean", async () => {
    const b = new MockBridge();
    // Selecting root 0 and landing at gap 0 is the block's own footprint → refused.
    const r = await b.request({ kind: "emitters/reorder-many", params: { ids: [0], rootIndex: 0 } });
    expect((r as { ok: boolean }).ok).toBe(false);
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });

  it("a NO-OP emitters/move-many (edge-pinned) leaves the document clean", async () => {
    const b = new MockBridge();
    // Smoke is the TOP root; moving it up is pinned → nothing moves.
    await b.request({ kind: "emitters/move-many", params: { ids: [0], direction: "up" } });
    expect((await b.request({ kind: "engine/state/snapshot", params: {} })).dirty).toBe(false);
  });

  // Refused / no-op clipboard + structural mutations (WX7): each returns its
  // refusal shape and must leave the document clean, as its handler comments
  // promise and as the native host does (markDirty only on the success branch).
  const isDirty = async (b: MockBridge) =>
    (await b.request({ kind: "engine/state/snapshot", params: {} })).dirty;

  it("emitters/paste with an EMPTY clipboard leaves the document clean", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/paste", params: {} });
    expect(r.newIds).toEqual([]);
    expect(await isDirty(b)).toBe(false);
  });

  it("a REFUSED emitters/paste-as-child (occupied slot) leaves the document clean", async () => {
    const b = new MockBridge();
    await b.request({ kind: "emitters/copy", params: { ids: [5] } });
    // Smoke (0) already has a lifetime child (1).
    const r = await b.request({ kind: "emitters/paste-as-child", params: { parentId: 0, slot: "lifetime" } });
    expect(r.newId).toBe(-1);
    expect(await isDirty(b)).toBe(false);
  });

  it("a REFUSED emitters/paste-as-child (empty clipboard) leaves the document clean", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/paste-as-child", params: { parentId: 5, slot: "lifetime" } });
    expect(r.newId).toBe(-1);
    expect(await isDirty(b)).toBe(false);
  });

  it("a REFUSED emitters/add-lifetime-child (slot filled) leaves the document clean", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/add-lifetime-child", params: { parentId: 0 } });
    expect(r.newId).toBe(-1);
    expect(await isDirty(b)).toBe(false);
  });

  it("a REFUSED emitters/add-death-child (slot filled) leaves the document clean", async () => {
    const b = new MockBridge();
    const r = await b.request({ kind: "emitters/add-death-child", params: { parentId: 0 } });
    expect(r.newId).toBe(-1);
    expect(await isDirty(b)).toBe(false);
  });

  it("emitters/delete of an UNKNOWN id leaves the document clean", async () => {
    const b = new MockBridge();
    await b.request({ kind: "emitters/delete", params: { id: 999 } });
    expect(await isDirty(b)).toBe(false);
  });

  it("emitters/delete-many of only UNKNOWN ids leaves the document clean", async () => {
    const b = new MockBridge();
    await b.request({ kind: "emitters/delete-many", params: { ids: [998, 999] } });
    expect(await isDirty(b)).toBe(false);
  });

  it("the successful counterparts still mark the document dirty", async () => {
    const b = new MockBridge();
    await b.request({ kind: "emitters/add-lifetime-child", params: { parentId: 5 } });
    expect(await isDirty(b)).toBe(true);
    const c = new MockBridge();
    useMockEngineState.setState(makeDefaultEngineState());
    await c.request({ kind: "emitters/delete", params: { id: 5 } });
    expect(await isDirty(c)).toBe(true);
  });
});

describe("emitter tree spawn params", () => {
  // tree nodes must mirror the LIVE spawn values from the
  // properties overlay (fixture defaults + set-properties patches), not
  // the frozen ZERO_SPAWN placeholder the tree-store literals carry for
  // type satisfaction. Decoration happens at the mock's emit/return
  // choke points.
  it("emitters/list nodes mirror the properties overlay's spawn fields", async () => {
    const b = new MockBridge();
    const tree = await b.request({ kind: "emitters/list", params: {} });
    const first = tree.root.children[0]!;
    const { properties } = await b.request({
      kind: "emitters/get-properties", params: { id: first.id },
    });
    expect(first.spawn).toEqual({
      lifetime: properties.lifetime,
      useBursts: properties.useBursts,
      nBursts: properties.nBursts,
      burstDelay: properties.burstDelay,
      nParticlesPerSecond: properties.nParticlesPerSecond,
      nParticlesPerBurst: properties.nParticlesPerBurst,
    });
  });

  it("a set-properties spawn patch is reflected in the next tree/changed payload", async () => {
    const b = new MockBridge();
    const events: EmitterTreeDto[] = [];
    const off = b.on("emitters/tree/changed", (e) => { events.push(e.payload); });
    await b.request({
      kind: "emitters/set-properties",
      params: { id: 0, patch: { nParticlesPerSecond: 4242 } },
    });
    off();
    expect(events.length).toBeGreaterThan(0);
    const node = events.at(-1)!.root.children.find((n) => n.id === 0);
    expect(node?.spawn.nParticlesPerSecond).toBe(4242);
  });
});
