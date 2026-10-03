// Mock counterpart of src/host/BridgeDispatch_LinkGroups.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import {
  setLinkGroupMembership,
  useMockEmitterTree,
  useMockLinkGroupExempt,
  useMockLinkGroupConflicts,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type LinkGroupsKind =
  | "linkGroups/set-membership"
  | "linkGroups/list-exempt-fields"
  | "linkGroups/set-exempt-fields"
  | "linkGroups/reset-exempt-fields"
  | "linkGroups/diff-membership"
  | "linkGroups/diff-exempt-change";

type LinkGroupsRequest = Extract<Request, { kind: LinkGroupsKind }>;

export function dispatchLinkGroups(this: MockDispatchHost, req: LinkGroupsRequest): unknown {
  switch (req.kind) {

    case "linkGroups/set-membership": {
      const cur = useMockEmitterTree.getState().tree;
      const next = setLinkGroupMembership(
        cur,
        req.params.ids,
        req.params.groupId,
      );
      useMockEmitterTree.getState().setTree(next);
      this.emit({ kind: "emitters/tree/changed", payload: next });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return {};
    }

    // ---------------- linkGroups/* ------
    case "linkGroups/list-exempt-fields": {
      const fields = useMockLinkGroupExempt.getState().get(req.params.groupId);
      return { fields };
    }

    case "linkGroups/set-exempt-fields": {
      useMockLinkGroupExempt.getState().set(req.params.groupId, req.params.fields);
      this.emit({
        kind: "emitters/tree/changed",
        payload: useMockEmitterTree.getState().tree,
      });
      return {};
    }

    case "linkGroups/reset-exempt-fields": {
      useMockLinkGroupExempt.getState().reset(req.params.groupId);
      this.emit({
        kind: "emitters/tree/changed",
        payload: useMockEmitterTree.getState().tree,
      });
      return {};
    }

    // The native host diffs real emitter params; the mock has
    // none, so it echoes whatever conflicts the test seeded (default:
    // none). Read-only — no mutation, no events.
    case "linkGroups/diff-membership": {
      const groupId = req.params.groupId;
      // Leaving (0/null) never overwrites anything → no conflicts.
      if (groupId === null || groupId === 0) return { conflicts: [] };
      return { conflicts: useMockLinkGroupConflicts.getState().conflicts };
    }

    // Link settings surface: same stub — the real exempt→shared field diff
    // is a native concern; echo the seeded conflicts so a test/preview can
    // drive the inline settings warning.
    case "linkGroups/diff-exempt-change":
      return { conflicts: useMockLinkGroupConflicts.getState().conflicts };

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
