import { describe, it, expect, beforeEach } from "vitest";
import { getEmitterTree, useEmitterTreeStore } from "@/lib/emitter-tree";
import type { Bridge, EmitterTreeDto } from "@particle-editor/bridge-schema";

beforeEach(() => useEmitterTreeStore.setState({ tree: null, bridge: null }));

describe("useEmitterTreeStore", () => {
  const bridge = {} as Bridge;
  const tree = { root: { id: -1, name: "root", role: "root", visible: true, children: [] } } as unknown as EmitterTreeDto;

  it("holds and replaces the tree", () => {
    expect(getEmitterTree(bridge)).toBeNull();
    useEmitterTreeStore.getState().setTree(tree, bridge);
    expect(getEmitterTree(bridge)).toBe(tree);
  });

  it("hides a tree another bridge served (ids belong to that session)", () => {
    useEmitterTreeStore.getState().setTree(tree, bridge);
    expect(getEmitterTree({} as Bridge)).toBeNull();
  });
});
