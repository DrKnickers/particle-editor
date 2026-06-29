import { describe, it, expect, beforeEach } from "vitest";
import {
  computeDeleteImpact, performDelete, requestDeleteEmitters,
  readConfirmDelete, writeConfirmDelete, useDeleteConfirmStore,
  collapseToRoots, confirmPendingDelete,
} from "@/lib/delete-emitters";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";

// helper to build a node; role is irrelevant to impact logic.
const node = (id: number, name: string, children: EmitterTreeNode[] = []): EmitterTreeNode =>
  ({ id, name, role: "root", visible: true, children } as unknown as EmitterTreeNode);

// tree: root -> a(0) -> [a1(1), a2(2)] ; b(3)
const tree = { root: node(-1, "root", [node(0, "a", [node(1, "a1"), node(2, "a2")]), node(3, "b")]) } as unknown as EmitterTreeDto;

function recordingBridge() {
  const calls: number[] = [];
  const bridge = {
    request: (req: { kind: string; params: { id?: number } }) => {
      if (req.kind === "emitters/delete" && typeof req.params.id === "number") calls.push(req.params.id);
      return Promise.resolve({});
    },
    on: () => () => {},
  } as unknown as Bridge;
  return { bridge, calls };
}

beforeEach(() => {
  useEmitterTreeStore.setState({ tree });
  useDeleteConfirmStore.setState({ pending: null });
  localStorage.clear();
});

describe("computeDeleteImpact", () => {
  it("single childless leaf is non-destructive", () => {
    expect(computeDeleteImpact([3], tree)).toMatchObject({ affectedCount: 1, isDestructive: false, primaryName: "b" });
  });
  it("parent with children is destructive and counts the subtree", () => {
    expect(computeDeleteImpact([0], tree)).toMatchObject({ affectedCount: 3, isDestructive: true, primaryName: "a" });
  });
  it("multi-select of leaves is destructive", () => {
    expect(computeDeleteImpact([3, 1], tree).isDestructive).toBe(true);
  });
  it("dedups parent + its own child both selected", () => {
    expect(computeDeleteImpact([0, 1], tree).affectedCount).toBe(3); // a,a1,a2 — not 4
  });
  it("empty selection", () => {
    expect(computeDeleteImpact([], tree)).toMatchObject({ affectedCount: 0, isDestructive: false });
  });
});

describe("confirm-delete setting", () => {
  it("defaults to true when unset", () => { expect(readConfirmDelete()).toBe(true); });
  it("round-trips false", () => { writeConfirmDelete(false); expect(readConfirmDelete()).toBe(false); });
  it("treats garbage as default true", () => { localStorage.setItem("alo:confirm-delete", "wat"); expect(readConfirmDelete()).toBe(true); });
});

describe("performDelete", () => {
  it("emits emitters/delete in descending id order", () => {
    const { bridge, calls } = recordingBridge();
    performDelete(bridge, [1, 3, 0], null);
    expect(calls).toEqual([3, 1, 0]);
  });
});

// Release-audit #8: parent+descendant selections must collapse to roots so a
// shifting positional id never deletes the wrong node.
describe("collapseToRoots (#8)", () => {
  it("drops a descendant whose ancestor is also selected", () => {
    expect(collapseToRoots([0, 1], tree)).toEqual([0]);        // a + a1 -> a
  });
  it("drops all descendants of a selected ancestor", () => {
    expect(collapseToRoots([0, 1, 2], tree)).toEqual([0]);     // a + a1 + a2 -> a
  });
  it("keeps disjoint subtrees (no shared ancestor)", () => {
    expect(collapseToRoots([1, 3], tree)).toEqual([1, 3]);     // a1 + b -> both
  });
  it("is a no-op for a single id or null tree", () => {
    expect(collapseToRoots([1], tree)).toEqual([1]);
    expect(collapseToRoots([0, 1], null)).toEqual([0, 1]);
  });
  it("performDelete collapses then deletes only the root", () => {
    const { bridge, calls } = recordingBridge();
    performDelete(bridge, [0, 1, 2], tree);
    expect(calls).toEqual([0]);                                // not [2,1,0]
  });
});

describe("confirmPendingDelete (#8 confirm-time revalidation)", () => {
  it("deletes (collapsed) when the tree is unchanged since the confirm opened", () => {
    const { bridge, calls } = recordingBridge();
    const live = useEmitterTreeStore.getState().tree;
    useDeleteConfirmStore.setState({
      pending: { ids: [0, 1], impact: computeDeleteImpact([0, 1], live), tree: live },
    });
    confirmPendingDelete(bridge);
    expect(calls).toEqual([0]);
    expect(useDeleteConfirmStore.getState().pending).toBeNull();
  });
  it("ABORTS (deletes nothing) when the tree changed since the confirm opened", () => {
    const { bridge, calls } = recordingBridge();
    const stale = useEmitterTreeStore.getState().tree;
    useDeleteConfirmStore.setState({
      pending: { ids: [0], impact: computeDeleteImpact([0], stale), tree: stale },
    });
    // A new emitters/list landed — different tree object reference.
    useEmitterTreeStore.setState({ tree: { root: node(-1, "root", [node(0, "a")]) } as unknown as EmitterTreeDto });
    confirmPendingDelete(bridge);
    expect(calls).toEqual([]);                                 // stale -> no delete
    expect(useDeleteConfirmStore.getState().pending).toBeNull();
  });
});

describe("requestDeleteEmitters", () => {
  it("deletes a leaf immediately, no confirm", () => {
    const { bridge, calls } = recordingBridge();
    requestDeleteEmitters(bridge, [3]);
    expect(calls).toEqual([3]);
    expect(useDeleteConfirmStore.getState().pending).toBeNull();
  });
  it("opens the confirm for a destructive delete and deletes nothing yet", () => {
    const { bridge, calls } = recordingBridge();
    requestDeleteEmitters(bridge, [0]);
    expect(calls).toEqual([]);
    expect(useDeleteConfirmStore.getState().pending?.ids).toEqual([0]);
  });
  it("with the toggle off, deletes immediately even when destructive", () => {
    writeConfirmDelete(false);
    const { bridge, calls } = recordingBridge();
    requestDeleteEmitters(bridge, [0]);
    expect(calls).toEqual([0]);
    expect(useDeleteConfirmStore.getState().pending).toBeNull();
  });
  it("ignores an empty selection", () => {
    const { bridge, calls } = recordingBridge();
    requestDeleteEmitters(bridge, []);
    expect(calls).toEqual([]);
  });
});
