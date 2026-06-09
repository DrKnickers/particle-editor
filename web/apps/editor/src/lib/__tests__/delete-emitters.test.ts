import { describe, it, expect, beforeEach } from "vitest";
import {
  computeDeleteImpact, performDelete, requestDeleteEmitters,
  readConfirmDelete, writeConfirmDelete, useDeleteConfirmStore,
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
    performDelete(bridge, [1, 3, 0]);
    expect(calls).toEqual([3, 1, 0]);
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
