// Mock counterpart of src/host/BridgeDispatch_EmitterProperties.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import {
  findEmitterNode,
  makeFixtureProperties,
  renameEmitter,
  useMockEmitterProperties,
  useMockEmitterTree,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type EmitterPropertiesKind =
  | "emitters/get-properties"
  | "emitters/set-properties";

type EmitterPropertiesRequest = Extract<Request, { kind: EmitterPropertiesKind }>;

export function dispatchEmitterProperties(this: MockDispatchHost, req: EmitterPropertiesRequest): unknown {
  switch (req.kind) {

    // ---------------- emitters/get-properties ----
    //
    // Returns the merged fixture+overlay DTO for `id`. Unknown ids
    // (including the synthetic root id=-1) return default-shaped
    // properties so the React form can render a disabled placeholder
    // rather than special-casing the failure. The native host returns
    // ok:false on unknown id; the contract test asserts the success
    // path against a known id.
    case "emitters/get-properties": {
      const cur = useMockEmitterTree.getState().tree;
      const node = findEmitterNode(cur, req.params.id);
      if (node === null || node.id === -1) {
        return {
          properties: useMockEmitterProperties.getState().read(-1),
        };
      }
      return {
        properties: useMockEmitterProperties.getState().read(node.id),
      };
    }

    // ---------------- emitters/set-properties ----
    //
    // Batch patch: apply known keys in `patch` to the overlay, emit
    // tree/changed + state/changed once so the React form re-fetches
    // and any downstream consumers (selection-aware components) see
    // the mutation. Missing ids are a silent no-op (the React side
    // disables the form when no emitter is selected).
    // Known values are not type-checked like native. The mock also
    // accepts the derived, read-only blendAlphaGated field, which native
    // skips; preserve this leniency for test fixtures.
    case "emitters/set-properties": {
      const cur = useMockEmitterTree.getState().tree;
      const node = findEmitterNode(cur, req.params.id);
      if (node === null || node.id === -1) {
        return { applied: [], skipped: [] };
      }
      const defaults = makeFixtureProperties(node.id);
      const applied: string[] = [];
      const skipped: string[] = [];
      const patch = Object.fromEntries(
        Object.entries(req.params.patch).filter(([key]) => {
          if (Object.prototype.hasOwnProperty.call(defaults, key)) {
            applied.push(key);
            return true;
          }
          skipped.push(key);
          return false;
        }),
      );
      useMockEmitterProperties.getState().patch(node.id, patch);
      // If the patch includes `name`, mirror it onto the tree node so
      // the EmitterTree label updates without an extra `emitters/rename`
      // round-trip.
      if (typeof req.params.patch.name === "string") {
        useMockEmitterTree.getState().setTree(
          renameEmitter(cur, node.id, req.params.patch.name),
        );
      }
      this.emit({
        kind: "emitters/tree/changed",
        payload: useMockEmitterTree.getState().tree,
      });
      this.emit({
        kind: "engine/state/changed",
        payload: snapshotEngineState(),
      });
      return { applied, skipped };
    }

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
