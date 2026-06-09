import { describe, it, expect, beforeEach } from "vitest";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import type { EmitterTreeDto } from "@particle-editor/bridge-schema";

beforeEach(() => useEmitterTreeStore.setState({ tree: null }));

describe("useEmitterTreeStore", () => {
  it("holds and replaces the tree", () => {
    expect(useEmitterTreeStore.getState().tree).toBeNull();
    const tree = { root: { id: -1, name: "root", role: "root", visible: true, children: [] } } as unknown as EmitterTreeDto;
    useEmitterTreeStore.getState().setTree(tree);
    expect(useEmitterTreeStore.getState().tree).toBe(tree);
  });
});
