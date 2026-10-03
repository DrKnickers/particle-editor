// Mock counterpart of src/host/BridgeDispatch_Emitters.cpp.

import type {
  Request,
  EmitterTreeDto,
  EmitterTreeNode,
  SpawnParamsDto,
} from "@particle-editor/bridge-schema";
import {
  addDeathChildEmitter,
  addLifetimeChildEmitter,
  addRootEmitterMock,
  deleteEmitter,
  duplicateEmitter,
  duplicateWithIndexIncrement,
  duplicateWithIndexIncrementMany,
  findEmitterNode,
  moveEmitterInTree,
  renameEmitter,
  reorderManyRoots,
  reorderRootEmitter,
  reparentEmitterInTree,
  setAllEmittersVisibleMock,
  setEmitterVisibleMock,
  useMockEmitterProperties,
  useMockEmitterTree,
  useMockEngineState,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type EmittersKind =
  | "emitters/preview-from-file"
  | "emitters/list"
  | "emitters/select"
  | "emitters/duplicate"
  | "emitters/duplicate-many"
  | "emitters/delete"
  | "emitters/delete-many"
  | "emitters/rename"
  | "emitters/duplicate-with-index-increment"
  | "emitters/duplicate-with-index-increment-many"
  | "emitters/add-lifetime-child"
  | "emitters/add-death-child"
  | "emitters/add-root"
  | "emitters/move"
  | "emitters/move-many"
  | "emitters/reorder-many"
  | "emitters/set-visible"
  | "emitters/set-all-visible"
  | "emitters/drop";

type EmittersRequest = Extract<Request, { kind: EmittersKind }>;

// live spawn values come from the properties overlay at emit time —
// ONE decoration point instead of mirroring into the tree store from every
// mutation handler. Tree-node literals carry ZERO_SPAWN purely to satisfy
// the type; this override is the source of truth.
export function pickSpawn(id: number): SpawnParamsDto {
  const p = useMockEmitterProperties.getState().read(id);
  return {
    lifetime: p.lifetime,
    useBursts: p.useBursts,
    nBursts: p.nBursts,
    burstDelay: p.burstDelay,
    nParticlesPerSecond: p.nParticlesPerSecond,
    nParticlesPerBurst: p.nParticlesPerBurst,
  };
}

export function decorateSpawn(node: EmitterTreeNode): EmitterTreeNode {
  return {
    ...node,
    // The synthetic root (id -1) keeps its stored ZERO_SPAWN — host parity
    // (the native synthetic roots serialize all-zeros, and the estimator
    // never reads the root's spawn).
    spawn: node.id === -1 ? node.spawn : pickSpawn(node.id),
    children: node.children.map(decorateSpawn),
  };
}

export function dispatchEmitters(this: MockDispatchHost, req: EmittersRequest): unknown {
  switch (req.kind) {

    // ---------------- emitters/preview-from-file
    //
    // Returns a fixed 3-emitter mock tree regardless of path. Lets the
    // Import Emitters modal exercise the checkbox tree in browser
    // mode + Vitest. The native host loads the real .alo into a
    // temporary ParticleSystem instead.
    case "emitters/preview-from-file":
      // stableId parity with the native preview tree (BuildEmitterTreeNode
      // emits it; synthetic root uses the reserved 0). The preview tree is
      // throwaway — fixed values are fine, they just must be present+unique.
      return {
        ok: true,
        tree: {
          id: 0,
          stableId: 0,
          name: "root",
          children: [
            { id: 1, stableId: 9001, name: "Smoke",  children: [
              { id: 4, stableId: 9004, name: "Smoke embers", children: [] },
            ] },
            { id: 2, stableId: 9002, name: "Sparks", children: [] },
            { id: 3, stableId: 9003, name: "Flash",  children: [] },
          ],
        },
      };

    // ---------------- emitters/list + emitters/select
    //
    // The fixture tree lives in `mock-state.useMockEmitterTree`. The
    // list response returns a fresh copy so React-side consumers can't
    // mutate the store. `emitters/select` updates the snapshot's
    // selectedEmitterId scalar and emits both `emitters/selected` and
    // `engine/state/changed` so subscribers picking up either channel
    // see the change. Selection of an unknown id resets to null.
    case "emitters/list": {
      const cloned = JSON.parse(
        JSON.stringify(useMockEmitterTree.getState().tree),
      ) as EmitterTreeDto;
      // decorate the clone with live spawn values (see decorateSpawn).
      return { root: decorateSpawn(cloned.root) };
    }

    case "emitters/select": {
      const reqId = req.params.id;
      const tree = useMockEmitterTree.getState().tree;
      const valid = reqId !== null && findEmitterNode(tree, reqId) !== null
        ? reqId
        : null;
      useMockEngineState.getState().applyPatch({ selectedEmitterId: valid });
      this.emit({ kind: "emitters/selected", payload: { id: valid } });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    // ---------------- emitters/* mutations -----
    //
    // The fixture tree is mutated in place via the helpers in
    // mock-state. Each handler emits `emitters/tree/changed` so the
    // React EmitterTree re-fetches via `emitters/list`. Selection
    // bookkeeping mirrors the native host: deleting the selected
    // emitter clears the selection scalar.
    case "emitters/duplicate": {
      const cur = useMockEmitterTree.getState().tree;
      const result = duplicateEmitter(cur, req.params.id);
      if (result === null) {
        return { ok: false, error: "emitter not found" };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { ok: true, newId: result.newId };
    }

    case "emitters/duplicate-many": {
      // Loop the single duplicate. The mock appends each copy with a fresh
      // high id and does NOT reindex existing emitters, so the collected
      // newIds stay valid across the loop. (The real host inserts after the
      // source + reindexes; both honour the {newIds} contract.)
      let tree = useMockEmitterTree.getState().tree;
      const newIds: number[] = [];
      for (const id of req.params.ids) {
        const r = duplicateEmitter(tree, id);
        if (r !== null) {
          tree = r.tree;
          newIds.push(r.newId);
        }
      }
      if (newIds.length === 0) {
        return { ok: false, error: "no emitters to duplicate" };
      }
      useMockEmitterTree.getState().setTree(tree);
      this.emit({ kind: "emitters/tree/changed", payload: tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { ok: true, newIds };
    }

    case "emitters/delete": {
      const cur = useMockEmitterTree.getState().tree;
      const next = deleteEmitter(cur, req.params.id);
      if (next === null) {
        // Nothing to delete — still emit so subscribers know we tried.
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return {};
      }
      useMockEmitterTree.getState().setTree(next);
      // If the deleted id was selected, clear the selection.
      const snap = snapshotEngineState();
      if (snap.selectedEmitterId === req.params.id) {
        useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
        this.emit({ kind: "emitters/selected", payload: { id: null } });
      }
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    // ---------------- emitters/delete-many --
    //
    // The multi-root delete gesture. Loops the single delete and emits
    // ONE tree/changed for the whole batch. Unlike the native host the
    // mock stores ids ON the nodes and never reindexes, so the incoming
    // descending order is irrelevant here — it matters on the real host,
    // where an id is a position. The single undo entry this batching
    // exists for is likewise native-only; the mock pins the wire shape.
    case "emitters/delete-many": {
      let tree = useMockEmitterTree.getState().tree;
      const snap = snapshotEngineState();
      let removed = 0;
      let clearedSelection = false;
      for (const id of req.params.ids) {
        const next = deleteEmitter(tree, id);
        if (next === null) continue;
        tree = next;
        removed++;
        if (snap.selectedEmitterId === id) clearedSelection = true;
      }
      if (removed === 0) {
        // Nothing matched — still emit so subscribers know we tried,
        // matching the single-delete handler above.
        this.emit({ kind: "emitters/tree/changed", payload: tree });
        return {};
      }
      useMockEmitterTree.getState().setTree(tree);
      if (clearedSelection) {
        useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
        this.emit({ kind: "emitters/selected", payload: { id: null } });
      }
      this.emit({ kind: "emitters/tree/changed", payload: tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    case "emitters/rename": {
      const cur = useMockEmitterTree.getState().tree;
      const next = renameEmitter(cur, req.params.id, req.params.name);
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      return {};
    }

    case "emitters/duplicate-with-index-increment": {
      const cur = useMockEmitterTree.getState().tree;
      const result = duplicateWithIndexIncrement(cur, req.params.id, req.params.delta);
      if (result === null) {
        // Shape says { newId: number } unconditionally. We surface
        // the failure by returning newId=-1; native handler errors
        // via the wire's ok:false path (the schema variant is the
        // single-arm `{ newId }`). Tests that assert success branch
        // pre-stage a valid id.
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return { newId: -1 };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newId: result.newId };
    }

    case "emitters/duplicate-with-index-increment-many": {
      const cur = useMockEmitterTree.getState().tree;
      const result = duplicateWithIndexIncrementMany(
        cur, req.params.id, req.params.delta, req.params.count);
      if (result === null) {
        // Mirror the host's error path (SendErr): no copies made.
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return { newIds: [] };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newIds: result.newIds };
    }

    // ---------------- emitters/add-* / move / set-membership -
    //
    // Each mutates the fixture tree via mock-state helpers, then
    // emits `emitters/tree/changed` + `engine/state/changed`. Refusal
    // semantics mirror the host: add-child returns `{ newId: -1 }`
    // when the parent's slot is already filled or the id is missing;
    // move returns `{}` regardless (a refused move is a silent no-op
    // because the React side disables the menu item at the edges).
    case "emitters/add-lifetime-child": {
      const cur = useMockEmitterTree.getState().tree;
      const result = addLifetimeChildEmitter(cur, req.params.parentId);
      if (result === null) {
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return { newId: -1 };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newId: result.newId };
    }

    case "emitters/add-death-child": {
      const cur = useMockEmitterTree.getState().tree;
      const result = addDeathChildEmitter(cur, req.params.parentId);
      if (result === null) {
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return { newId: -1 };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newId: result.newId };
    }

    // New top-level "New Root Emitter"
    // menu item. Always succeeds at the mock level (the engine has
    // no max-roots cap). Tree-changed + state-changed events match
    // the other add-child handlers.
    case "emitters/add-root": {
      const cur = useMockEmitterTree.getState().tree;
      const result = addRootEmitterMock(cur);
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newId: result.newId };
    }

    case "emitters/move": {
      const cur = useMockEmitterTree.getState().tree;
      const next = moveEmitterInTree(cur, req.params.id, req.params.direction);
      if (next === null) {
        // Refused (non-root or at edge). Still emit so subscribers
        // that re-fetch defensively don't get stuck.
        this.emit({ kind: "emitters/tree/changed", payload: cur });
        return {};
      }
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    case "emitters/move-many": {
      // Move the selected ROOTS as a UNIT, preserving order: if the edge-most
      // root in the move direction is selected, the block is pinned → nothing
      // moves (no compacting past non-selected roots). Otherwise shift every
      // selected root by one (ascending for up / descending for down). The
      // mock keeps node ids stable through a move, so newIds are the selected
      // ids still at root level. (The real host reindexes; both honour {newIds}.)
      let tree = useMockEmitterTree.getState().tree;
      const dir = req.params.direction;
      const sel = new Set(req.params.ids);
      const order = tree.root.children.map((c) => c.id);
      const movable: number[] = [];
      const edgePinned =
        order.length > 0 && sel.has(dir === "up" ? order[0]! : order[order.length - 1]!);
      if (!edgePinned) {
        if (dir === "up") {
          for (let i = 0; i < order.length; i++) if (sel.has(order[i]!)) movable.push(order[i]!);
        } else {
          for (let i = order.length - 1; i >= 0; i--) if (sel.has(order[i]!)) movable.push(order[i]!);
        }
      }
      let moved = false;
      for (const id of movable) {
        const next = moveEmitterInTree(tree, id, dir);
        if (next !== null) {
          tree = next;
          moved = true;
        }
      }
      useMockEmitterTree.getState().setTree(tree);
      this.emit({ kind: "emitters/tree/changed", payload: tree });
      if (moved) this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      const finalRootIds = new Set(tree.root.children.map((c) => c.id));
      return { newIds: req.params.ids.filter((id) => finalRootIds.has(id)) };
    }

    case "emitters/reorder-many": {
      const cur = useMockEmitterTree.getState().tree;
      const next = reorderManyRoots(cur, req.params.ids, req.params.rootIndex);
      if (next === null) return { ok: false, error: "reorder refused" };
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      // Mock ids are stable across a reorder; newIds = the selected ids still
      // at root level, in input order (aligned for applyNewSelection).
      const rootIds = new Set(next.root.children.map((c) => c.id));
      return { ok: true, newIds: req.params.ids.filter((id) => rootIds.has(id)) };
    }

    case "emitters/set-visible": {
      const cur = useMockEmitterTree.getState().tree;
      const next = setEmitterVisibleMock(cur, req.params.id, req.params.visible);
      if (next === null) return {};
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    case "emitters/set-all-visible": {
      const cur = useMockEmitterTree.getState().tree;
      const next = setAllEmittersVisibleMock(cur, req.params.visible);
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    // ---------------- emitters/drop ------------
    //
    // Tagged-union: { mode: "reorder", id, rootIndex } reorders a
    // root via `reorderRootEmitter`; { mode: "reparent", id,
    // targetId, slot } moves the source under target in the named
    // slot via `reparentEmitterInTree`. Both helpers refuse cleanly
    // (return null) on cycle / slot-full / non-root / no-op — the
    // mock surfaces refusal as `{ ok: false, error: "..." }` to
    // match the native dispatcher's contract.
    case "emitters/drop": {
      const cur = useMockEmitterTree.getState().tree;
      // After a successful drop the highlight FOLLOWS the moved emitter
      // (re-select it + emit emitters/selected), mirroring the native host.
      // Mock ids are stable across a drop, so the moved id is req.params.id.
      const followSelection = () => {
        const id = req.params.id;
        useMockEngineState.getState().applyPatch({ selectedEmitterId: id });
        this.emit({ kind: "emitters/selected", payload: { id } });
      };
      if (req.params.mode === "reorder") {
        const next = reorderRootEmitter(cur, req.params.id, req.params.rootIndex);
        if (next === null) {
          return { ok: false, error: "reorder refused" };
        }
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        followSelection();
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true };
      }
      // mode === "reparent"
      const next = reparentEmitterInTree(
        cur,
        req.params.id,
        req.params.targetId,
        req.params.slot,
      );
      if (next === null) {
        return { ok: false, error: "reparent refused" };
      }
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      followSelection();
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { ok: true };
    }

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
