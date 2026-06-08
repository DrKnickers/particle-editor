// Vitest unit tests for the link-group bracket palette + range
// computation (Phase 3 Screen 4 Batch C).

import { describe, it, expect } from "vitest";
import {
  BRACKET_PALETTE_SIZE,
  colorForGroup,
  computeLinkGroupBrackets,
  laneCount,
  type LinkGroupBracket,
} from "../link-group-colors";

describe("link-group-colors", () => {
  it("colorForGroup returns null for unlinked (group 0) and cycles through 8 colours for non-zero groups", () => {
    expect(BRACKET_PALETTE_SIZE).toBe(8);
    expect(colorForGroup(0)).toBeNull();
    // Group 1 → palette[0]; group 9 wraps back to palette[0].
    const c1 = colorForGroup(1)!;
    const c9 = colorForGroup(9)!;
    expect(c1).toBeTruthy();
    expect(c1).toBe(c9);
    // Group 2..8 should each be distinct from group 1.
    const seen = new Set([c1]);
    for (let g = 2; g <= 8; g++) {
      const c = colorForGroup(g)!;
      expect(c).toBeTruthy();
      seen.add(c);
    }
    expect(seen.size).toBe(8);
  });

  it("computeLinkGroupBrackets bounds each group's first + last row index in the flat list", () => {
    const rows = [
      { linkGroup: 0 }, // 0  unlinked
      { linkGroup: 1 }, // 1  group 1 starts
      { linkGroup: 0 }, // 2  unlinked
      { linkGroup: 1 }, // 3  group 1 extends → 2-member group, KEPT
      { linkGroup: 2 }, // 4  group 2 starts
      { linkGroup: 2 }, // 5  group 2 extends → 2-member group, KEPT
      { linkGroup: 3 }, // 6  group 3, ONLY member → FILTERED OUT
      { linkGroup: 0 }, // 7  unlinked
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(2);  // group 3 absent
    const g1 = brackets.find((b) => b.groupId === 1)!;
    expect(g1.firstRowIndex).toBe(1);
    expect(g1.lastRowIndex).toBe(3);
    expect(g1.memberRowIndices).toEqual([1, 3]);  // every member, not just ends
    expect(g1.color).toBe(colorForGroup(1));
    const g2 = brackets.find((b) => b.groupId === 2)!;
    expect(g2.firstRowIndex).toBe(4);
    expect(g2.lastRowIndex).toBe(5);
    expect(g2.memberRowIndices).toEqual([4, 5]);
    expect(g2.color).toBe(colorForGroup(2));
    expect(brackets.find((b) => b.groupId === 3)).toBeUndefined();
  });

  it("filters single-member groups — they never appear in the result", () => {
    const rows = [
      { linkGroup: 5 },  // only member of group 5
      { linkGroup: 0 },
      { linkGroup: 7 },  // only member of group 7
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(0);
  });

  it("assigns lane 0 to a single group", () => {
    const rows = [
      { linkGroup: 0 },
      { linkGroup: 1 }, // row 1
      { linkGroup: 1 }, // row 2
      { linkGroup: 0 },
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(1);
    expect(brackets[0].lane).toBe(0);
  });

  it("gives each non-overlapping group its OWN dedicated lane (no reuse), ordered by groupId", () => {
    const rows = [
      { linkGroup: 1 }, { linkGroup: 1 }, // group 1: rows 0-1
      { linkGroup: 0 },
      { linkGroup: 2 }, { linkGroup: 2 }, // group 2: rows 3-4 (no overlap with g1)
      { linkGroup: 0 },
      { linkGroup: 3 }, { linkGroup: 3 }, // group 3: rows 6-7 (no overlap)
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(3);
    // Dedicated lanes: even though none overlap, each group keeps its own
    // lane so the gutter never bounces (one lane per group, by groupId).
    expect(brackets.find((b) => b.groupId === 1)!.lane).toBe(0);
    expect(brackets.find((b) => b.groupId === 2)!.lane).toBe(1);
    expect(brackets.find((b) => b.groupId === 3)!.lane).toBe(2);
  });

  it("orders lanes by groupId regardless of row order", () => {
    const rows = [
      { linkGroup: 2 }, // group 2 starts FIRST in row order
      { linkGroup: 1 },
      { linkGroup: 2 },
      { linkGroup: 1 },
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(2);
    // Lane follows groupId, not first-appearance: group 1 → lane 0.
    expect(brackets.find((b) => b.groupId === 1)!.lane).toBe(0);
    expect(brackets.find((b) => b.groupId === 2)!.lane).toBe(1);
  });

  it("collects every member row index per group (for per-member stubs)", () => {
    const rows = [
      { linkGroup: 1 }, // 0
      { linkGroup: 0 }, // 1 gap inside the group's span
      { linkGroup: 1 }, // 2
      { linkGroup: 1 }, // 3
    ];
    const brackets = computeLinkGroupBrackets(rows);
    expect(brackets).toHaveLength(1);
    expect(brackets[0].memberRowIndices).toEqual([0, 2, 3]);
    expect(brackets[0].firstRowIndex).toBe(0);
    expect(brackets[0].lastRowIndex).toBe(3);
  });

  it("laneCount returns 0 for an empty bracket array", () => {
    expect(laneCount([])).toBe(0);
  });

  it("laneCount returns 1 for a single group", () => {
    const rows = [
      { linkGroup: 1 }, { linkGroup: 1 },
      { linkGroup: 0 },
    ];
    expect(laneCount(computeLinkGroupBrackets(rows))).toBe(1);
  });

  it("laneCount equals the number of groups (one dedicated lane each)", () => {
    const rows = [
      { linkGroup: 1 }, { linkGroup: 1 },
      { linkGroup: 0 },
      { linkGroup: 2 }, { linkGroup: 2 }, // non-overlapping, but its own lane now
    ];
    expect(laneCount(computeLinkGroupBrackets(rows))).toBe(2);
  });

  it("laneCount returns 3 for a 3-lane bracket set", () => {
    // Build a bracket array directly with known lanes.
    const brackets: LinkGroupBracket[] = [
      { groupId: 1, color: "#000", firstRowIndex: 0, lastRowIndex: 5, memberRowIndices: [0, 5], lane: 0 },
      { groupId: 2, color: "#000", firstRowIndex: 1, lastRowIndex: 4, memberRowIndices: [1, 4], lane: 1 },
      { groupId: 3, color: "#000", firstRowIndex: 2, lastRowIndex: 3, memberRowIndices: [2, 3], lane: 2 },
    ];
    expect(laneCount(brackets)).toBe(3);
  });
});
