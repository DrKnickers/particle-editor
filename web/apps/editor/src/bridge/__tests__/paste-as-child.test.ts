// Tests for Paste As ▸ Child (/): the pure mock-state tree
// helper and the MockBridge `emitters/paste-as-child` round-trip.

import { describe, it, expect } from "vitest";
import type { EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import {
  pasteAsChildFromClipboard,
  useMockEmitterTree,
  useMockEmitterClipboard,
} from "../mock-state";
import { MockBridge } from "../mock";

function node(
  id: number,
  name: string,
  role: EmitterTreeNode["role"],
  children: EmitterTreeNode[] = [],
): EmitterTreeNode {
  return { id, name, role, linkGroup: 0, visible: true, children };
}

// A two-root tree; root id 1 ("Alpha") has no children (both slots free).
function freshTree(): EmitterTreeDto {
  return {
    root: node(0, "", "root", [node(1, "Alpha", "root"), node(2, "Beta", "root")]),
  };
}

const clip: EmitterTreeNode[] = [
  node(99, "Copied", "root", [node(100, "Copied kid", "lifetime")]),
];

describe("pasteAsChildFromClipboard", () => {
  it("attaches clipboard[0] as a lifetime child of the target", () => {
    const r = pasteAsChildFromClipboard(freshTree(), clip, 1, "lifetime");
    expect(r).not.toBeNull();
    const alpha = r!.tree.root.children.find((c) => c.id === 1)!;
    const lifetime = alpha.children.find((c) => c.role === "lifetime")!;
    expect(lifetime).toBeTruthy();
    expect(lifetime.name).toBe("Copied");
    expect(lifetime.id).toBe(r!.newId);
    // Seeded from the buffer: the copied subtree comes along.
    expect(lifetime.children.length).toBe(1);
  });

  it("attaches as a death child when slot=death", () => {
    const r = pasteAsChildFromClipboard(freshTree(), clip, 1, "death");
    expect(r).not.toBeNull();
    const alpha = r!.tree.root.children.find((c) => c.id === 1)!;
    expect(alpha.children.find((c) => c.role === "death")!.role).toBe("death");
  });

  it("returns null when the buffer is empty", () => {
    expect(pasteAsChildFromClipboard(freshTree(), [], 1, "lifetime")).toBeNull();
  });

  it("returns null when the lifetime slot is already occupied", () => {
    const tree: EmitterTreeDto = {
      root: node(0, "", "root", [
        node(1, "Alpha", "root", [node(5, "existing", "lifetime")]),
      ]),
    };
    expect(pasteAsChildFromClipboard(tree, clip, 1, "lifetime")).toBeNull();
  });

  it("returns null for an unknown parent", () => {
    expect(pasteAsChildFromClipboard(freshTree(), clip, 999, "lifetime")).toBeNull();
  });

  it("re-ids the WHOLE pasted subtree so no id collides with the tree", () => {
    // Clipboard subtree whose descendant ids (1, 2) collide with the
    // fresh tree's existing ids — the pasted nodes must all be re-id'd,
    // or React renders duplicate keys.
    const colliding: EmitterTreeNode[] = [
      node(0, "Src", "root", [
        node(1, "Src life", "lifetime"),
        node(2, "Src death", "death"),
      ]),
    ];
    const r = pasteAsChildFromClipboard(freshTree(), colliding, 1, "lifetime");
    expect(r).not.toBeNull();
    const ids: number[] = [];
    const walk = (n: EmitterTreeNode) => { ids.push(n.id); n.children.forEach(walk); };
    walk(r!.tree.root);
    expect(new Set(ids).size).toBe(ids.length); // all unique
  });
});

describe("MockBridge emitters/paste-as-child", () => {
  it("pastes the clipboard into a free lifetime slot and returns a real newId", async () => {
    useMockEmitterTree.getState().reset();
    useMockEmitterClipboard.getState().reset();
    const bridge = new MockBridge();
    const roots = useMockEmitterTree.getState().tree.root.children;
    const copyId = roots[0].id; // "Smoke"
    // "Flash" (last root) has no children — both child slots free.
    const targetId = roots[roots.length - 1].id;
    await bridge.request({ kind: "emitters/copy", params: { ids: [copyId] } });

    const res = await bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: targetId, slot: "lifetime" },
    });
    expect(res.newId).toBeGreaterThan(0);
    const after = useMockEmitterTree.getState().tree;
    const target = after.root.children.find((c) => c.id === targetId)!;
    expect(target.children.some((c) => c.role === "lifetime")).toBe(true);
  });

  it("returns newId -1 when the clipboard is empty", async () => {
    useMockEmitterTree.getState().reset();
    useMockEmitterClipboard.getState().reset();
    const bridge = new MockBridge();
    const id = useMockEmitterTree.getState().tree.root.children[0].id;
    const res = await bridge.request({
      kind: "emitters/paste-as-child",
      params: { parentId: id, slot: "death" },
    });
    expect(res.newId).toBe(-1);
  });
});
