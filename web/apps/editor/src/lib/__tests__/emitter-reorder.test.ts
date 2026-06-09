import { describe, it, expect, beforeEach } from "vitest";
import { moveEmitters, duplicateEmitters } from "@/lib/emitter-reorder";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import type { Bridge } from "@particle-editor/bridge-schema";

function fakeBridge(responses: Record<string, unknown>) {
  const calls: { kind: string; params: unknown }[] = [];
  const bridge = {
    request: (req: { kind: string; params: unknown }) => {
      calls.push(req);
      return Promise.resolve(responses[req.kind] ?? {});
    },
    on: () => () => {},
  } as unknown as Bridge;
  return { bridge, calls };
}

beforeEach(() => useEmitterSelectionStore.getState().clear());

describe("moveEmitters", () => {
  it("calls move-many and re-selects the returned newIds (highlight follows)", async () => {
    useEmitterSelectionStore.getState().setIds([3, 5], 5);
    const { bridge, calls } = fakeBridge({ "emitters/move-many": { newIds: [2, 4] } });
    await moveEmitters(bridge, [3, 5], "up");
    expect(calls[0]).toEqual({ kind: "emitters/move-many", params: { ids: [3, 5], direction: "up" } });
    const sel = useEmitterSelectionStore.getState();
    expect(sel.ids).toEqual([2, 4]);
    expect(sel.primary).toBe(4); // old primary 5 was at input index 1 → newIds[1] = 4
    // host single-selection synced to the new primary
    expect(calls).toContainEqual({ kind: "emitters/select", params: { id: 4 } });
  });

  it("no-ops on an empty selection", async () => {
    const { bridge, calls } = fakeBridge({});
    await moveEmitters(bridge, [], "up");
    expect(calls).toEqual([]);
  });
});

describe("duplicateEmitters", () => {
  it("calls duplicate-many and moves the selection to the new copies on ok", async () => {
    useEmitterSelectionStore.getState().setIds([0, 1], 0);
    const { bridge, calls } = fakeBridge({ "emitters/duplicate-many": { ok: true, newIds: [6, 7] } });
    await duplicateEmitters(bridge, [0, 1]);
    expect(calls[0]).toEqual({ kind: "emitters/duplicate-many", params: { ids: [0, 1] } });
    const sel = useEmitterSelectionStore.getState();
    expect(sel.ids).toEqual([6, 7]);
    expect(sel.primary).toBe(6); // old primary 0 at index 0 → newIds[0] = 6
  });

  it("leaves the selection untouched on failure", async () => {
    useEmitterSelectionStore.getState().setIds([0], 0);
    const { bridge } = fakeBridge({ "emitters/duplicate-many": { ok: false, error: "x" } });
    await duplicateEmitters(bridge, [0]);
    expect(useEmitterSelectionStore.getState().ids).toEqual([0]);
  });
});
