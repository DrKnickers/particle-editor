import { describe, it, expect } from "vitest";
import { reorderManyRoots } from "@/bridge/mock-state";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import type { EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";

const LETTERS = ["A", "B", "C", "D", "E", "F"];
function tree(): EmitterTreeDto {
  const children: EmitterTreeNode[] = LETTERS.map((name, id) => ({
    id, stableId: 100 + id, name, role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children: [],
  }));
  return { root: { id: -1, stableId: 0, name: "", role: "root", linkGroup: 0, visible: true, spawn: ZERO_SPAWN, children } };
}
const order = (t: EmitterTreeDto | null) =>
  t === null ? null : t.root.children.map((c) => c.name).join("");

describe("reorderManyRoots", () => {
  it("collapses a non-contiguous selection to a contiguous block, in tree order", () => {
    expect(order(reorderManyRoots(tree(), [1, 3], 5))).toBe("ACEBDF");
  });
  it("moves a contiguous block to the top (gap 0)", () => {
    expect(order(reorderManyRoots(tree(), [2, 3], 0))).toBe("CDABEF");
  });
  it("moves a contiguous block to the bottom (gap N)", () => {
    expect(order(reorderManyRoots(tree(), [1, 2], 6))).toBe("ADEFBC");
  });
  it("refuses a no-op on the block's own footprint (edges AND interior)", () => {
    for (const gap of [1, 2, 3, 4]) {
      expect(reorderManyRoots(tree(), [1, 2, 3], gap)).toBeNull();
    }
    // ...and DOES move for gaps outside its own footprint:
    expect(order(reorderManyRoots(tree(), [1, 2, 3], 0))).toBe("BCDAEF"); // to top
    expect(order(reorderManyRoots(tree(), [1, 2, 3], 6))).toBe("AEFBCD"); // to bottom
  });
  it("treats a single-id selection like a single reorder", () => {
    expect(order(reorderManyRoots(tree(), [0], 3))).toBe("BCADEF");
    expect(reorderManyRoots(tree(), [0], 1)).toBeNull();
  });
  it("refuses out-of-range gap, empty selection, and non-root ids", () => {
    expect(reorderManyRoots(tree(), [1], 99)).toBeNull();
    expect(reorderManyRoots(tree(), [], 2)).toBeNull();
    expect(reorderManyRoots(tree(), [42], 2)).toBeNull();
  });
  it("ignores duplicate ids and input order (uses tree order)", () => {
    expect(order(reorderManyRoots(tree(), [3, 1, 3], 5))).toBe("ACEBDF");
  });
});
