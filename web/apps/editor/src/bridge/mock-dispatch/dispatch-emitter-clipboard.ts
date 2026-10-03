// Mock counterpart of src/host/BridgeDispatch_EmitterClipboard.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import {
  copyEmittersToClipboard,
  deleteEmitter,
  pasteEmittersFromClipboard,
  pasteAsChildFromClipboard,
  useMockEmitterClipboard,
  useMockEmitterTree,
  useMockEngineState,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type EmitterClipboardKind =
  | "emitters/copy"
  | "emitters/cut"
  | "emitters/paste"
  | "emitters/paste-as-child";

type EmitterClipboardRequest = Extract<Request, { kind: EmitterClipboardKind }>;

export function dispatchEmitterClipboard(this: MockDispatchHost, req: EmitterClipboardRequest): unknown {
  switch (req.kind) {

    // ---------------- emitters/copy / cut / paste
    //
    // Process-local clipboard mirrors the native host's
    // `std::vector<std::vector<uint8_t>>`. `copy` snapshots subtrees
    // into the in-memory buffer; `cut` does the same then deletes
    // the originals in descending-id order (so prior indices stay
    // valid during the loop); `paste` deep-clones the buffer back
    // into the tree with fresh ids, splicing after `afterId` (when
    // present and matching a root) or appending at the end.
    case "emitters/copy": {
      const cur = useMockEmitterTree.getState().tree;
      const buf = copyEmittersToClipboard(cur, req.params.ids);
      useMockEmitterClipboard.getState().set(buf);
      return {};
    }

    case "emitters/cut": {
      const cur = useMockEmitterTree.getState().tree;
      const buf = copyEmittersToClipboard(cur, req.params.ids);
      useMockEmitterClipboard.getState().set(buf);
      // Delete in descending id order — keeps indices valid even if
      // a future implementation drops in-place id reuse. Single
      // tree-changed event at the end (atomic cut).
      let next: typeof cur = cur;
      const ids = [...req.params.ids].sort((a, b) => b - a);
      for (const id of ids) {
        const after = deleteEmitter(next, id);
        if (after !== null) next = after;
      }
      useMockEmitterTree.getState().setTree(next);
      // Clear selection if any cut id was selected.
      const snap = snapshotEngineState();
      if (snap.selectedEmitterId !== null && req.params.ids.includes(snap.selectedEmitterId)) {
        useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
        this.emit({ kind: "emitters/selected", payload: { id: null } });
      }
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    case "emitters/paste": {
      const cur = useMockEmitterTree.getState().tree;
      const buf = useMockEmitterClipboard.getState().buffer;
      const afterId = req.params.afterId ?? null;
      const result = pasteEmittersFromClipboard(cur, buf, afterId);
      if (result.newIds.length === 0) {
        // Empty clipboard or nothing pasted; emit nothing so dirty
        // doesn't flip pointlessly. Still return the empty newIds.
        return { newIds: [] };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newIds: result.newIds };
    }

    case "emitters/paste-as-child": {
      const cur = useMockEmitterTree.getState().tree;
      const buf = useMockEmitterClipboard.getState().buffer;
      const result = pasteAsChildFromClipboard(
        cur,
        buf,
        req.params.parentId,
        req.params.slot,
      );
      if (result === null) {
        // Empty clipboard or occupied slot — emit nothing, no dirty flip.
        return { newId: -1 };
      }
      useMockEmitterTree.getState().setTree(result.tree);
      this.emit({ kind: "emitters/tree/changed", payload: result.tree });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { newId: result.newId };
    }

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
