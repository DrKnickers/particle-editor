import { describe, it, expect } from "vitest";
import { computeFlipDeltas, type FlipPositions } from "@/lib/flip";

const m = (entries: Array<[number, number]>): FlipPositions => new Map(entries);

describe("computeFlipDeltas", () => {
  it("returns prev-minus-next for rows that moved", () => {
    // row 101 moved down 24 (was 0, now 24) → delta -? prev-next = -24?
    // delta is the INVERT transform: was-0 now-24 → translateY(-24) puts it
    // back at its old spot, then transitions to 0.
    const prev = m([[101, 0], [102, 24], [103, 48]]);
    const next = m([[101, 24], [102, 0], [103, 48]]);
    const d = computeFlipDeltas(prev, next);
    expect(d.get(101)).toBe(-24);
    expect(d.get(102)).toBe(24);
    expect(d.has(103)).toBe(false); // unmoved → no entry
  });
  it("ignores rows that only exist on one side (created / deleted)", () => {
    const d = computeFlipDeltas(m([[101, 0]]), m([[102, 0]]));
    expect(d.size).toBe(0);
  });
  it("empty maps → empty deltas", () => {
    expect(computeFlipDeltas(m([]), m([])).size).toBe(0);
  });
});
