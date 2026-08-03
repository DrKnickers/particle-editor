// Vitest tests for lib/drop-zone.ts — the pure DnD math behind the
// EmitterTree drag/drop layer.
//
// Coverage:
//   1. computeDropZone   — y → above/onto/below thirds, with the exact
//      boundary semantics from the source: `y < third` is "above"
//      (strict), `y >= rowHeight - third` is "below" (inclusive), so
//      y === 1/3 lands "onto" and y === 2/3 lands "below". Degenerate
//      rowHeight (0 / negative) short-circuits to "onto".
//   2. isDescendant      — DFS cycle detection: self, direct child,
//      deep descendant, unrelated, leaf (empty subtree).
//   3. resolveReparentSlot — both-free → lifetime, one-free → the free
//      one, both-filled → null (refused drop).
//   4. computeRootGapIndex — above → gap K, below → gap K+1, including
//      the before-first and after-last edges.

import { describe, it, expect } from "vitest";
import type { EmitterRole, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { ZERO_SPAWN } from "@particle-editor/bridge-schema";
import {
  computeDropZone,
  isDescendant,
  resolveReparentSlot,
  computeRootGapIndex,
} from "../drop-zone";

/** Minimal EmitterTreeNode fixture. Only id / role / children matter to
 *  the helpers under test; the rest satisfies the DTO shape. */
function node(
  id: number,
  role: EmitterRole = "root",
  children: EmitterTreeNode[] = [],
): EmitterTreeNode {
  return {
    id,
    stableId: 1000 + id,
    name: `emitter-${id}`,
    role,
    linkGroup: 0,
    visible: true,
    spawn: ZERO_SPAWN,
    children,
  };
}

// ─── computeDropZone ─────────────────────────────────────────────────

describe("computeDropZone", () => {
  // rowHeight 30 → third = 10; bands: [0,10) above, [10,20) onto, [20,∞) below.
  const H = 30;

  it("top edge (y=0) is 'above'", () => {
    expect(computeDropZone(0, H)).toBe("above");
  });

  it("just under the 1/3 boundary is 'above'", () => {
    expect(computeDropZone(9.999, H)).toBe("above");
  });

  it("y exactly at 1/3 (strict-less upper band) is 'onto'", () => {
    // Source uses `y < third` — the boundary pixel belongs to the middle band.
    expect(computeDropZone(10, H)).toBe("onto");
  });

  it("middle of the row is 'onto'", () => {
    expect(computeDropZone(15, H)).toBe("onto");
  });

  it("just under the 2/3 boundary is 'onto'", () => {
    expect(computeDropZone(19.999, H)).toBe("onto");
  });

  it("y exactly at 2/3 (inclusive lower band) is 'below'", () => {
    // Source uses `y >= rowHeight - third` — the boundary pixel is "below".
    expect(computeDropZone(20, H)).toBe("below");
  });

  it("bottom edge (y=rowHeight) is 'below'", () => {
    expect(computeDropZone(30, H)).toBe("below");
  });

  it("handles a height that doesn't divide by 3 (banker's-free float math)", () => {
    // H=22 → third ≈ 7.333…; 7 is above, 7.34 is onto, 14.67 is below.
    expect(computeDropZone(7, 22)).toBe("above");
    expect(computeDropZone(7.34, 22)).toBe("onto");
    expect(computeDropZone(22 - 22 / 3, 22)).toBe("below");
  });

  it("degenerate rowHeight (0 or negative) collapses to 'onto'", () => {
    expect(computeDropZone(0, 0)).toBe("onto");
    expect(computeDropZone(5, -10)).toBe("onto");
  });
});

// ─── isDescendant ────────────────────────────────────────────────────

describe("isDescendant", () => {
  //        1
  //       / \
  //      2   3
  //          |
  //          4
  //          |
  //          5
  const deep = node(1, "root", [
    node(2, "lifetime"),
    node(3, "death", [node(4, "lifetime", [node(5, "lifetime")])]),
  ]);

  it("returns true for the source itself (self-cycle guard)", () => {
    expect(isDescendant(deep, 1)).toBe(true);
  });

  it("returns true for a direct child", () => {
    expect(isDescendant(deep, 2)).toBe(true);
  });

  it("returns true for a deep descendant (multi-level DFS)", () => {
    expect(isDescendant(deep, 5)).toBe(true);
  });

  it("returns false for an unrelated node id", () => {
    expect(isDescendant(deep, 99)).toBe(false);
  });

  it("returns false on a leaf source (empty subtree) for a foreign id", () => {
    expect(isDescendant(node(7), 8)).toBe(false);
  });

  it("a leaf source still matches itself", () => {
    expect(isDescendant(node(7), 7)).toBe(true);
  });
});

// ─── resolveReparentSlot ─────────────────────────────────────────────

describe("resolveReparentSlot", () => {
  it("both slots free → 'lifetime' (legacy auto-pick)", () => {
    expect(resolveReparentSlot(node(1, "root", []))).toBe("lifetime");
  });

  it("death occupied, lifetime free → 'lifetime'", () => {
    const target = node(1, "root", [node(2, "death")]);
    expect(resolveReparentSlot(target)).toBe("lifetime");
  });

  it("lifetime occupied, death free → 'death'", () => {
    const target = node(1, "root", [node(2, "lifetime")]);
    expect(resolveReparentSlot(target)).toBe("death");
  });

  it("both slots occupied → null (drop refused)", () => {
    const target = node(1, "root", [node(2, "lifetime"), node(3, "death")]);
    expect(resolveReparentSlot(target)).toBeNull();
  });

  it("occupancy is by role, not by child count", () => {
    // Two lifetime children still leave the death slot free.
    const target = node(1, "root", [node(2, "lifetime"), node(3, "lifetime")]);
    expect(resolveReparentSlot(target)).toBe("death");
  });
});

// ─── computeRootGapIndex ─────────────────────────────────────────────

describe("computeRootGapIndex", () => {
  it("dropping above the first root resolves to gap 0 (insert at front)", () => {
    expect(computeRootGapIndex(0, "above")).toBe(0);
  });

  it("dropping below the first root resolves to gap 1", () => {
    expect(computeRootGapIndex(0, "below")).toBe(1);
  });

  it("dropping above the row at index K resolves to gap K", () => {
    expect(computeRootGapIndex(3, "above")).toBe(3);
  });

  it("dropping below the last root (index N-1 of N=5) resolves to gap N", () => {
    expect(computeRootGapIndex(4, "below")).toBe(5);
  });
});
