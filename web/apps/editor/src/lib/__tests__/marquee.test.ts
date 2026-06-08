import { describe, it, expect } from "vitest";
import {
  rectFromPoints,
  rectsIntersect,
  emittersInMarquee,
  mergeMarqueeSelection,
  type Rect,
} from "../marquee";

describe("marquee geometry", () => {
  it("rectFromPoints normalises any corner order", () => {
    expect(rectFromPoints(10, 20, 4, 6)).toEqual({ left: 4, top: 6, right: 10, bottom: 20 });
    expect(rectFromPoints(4, 6, 10, 20)).toEqual({ left: 4, top: 6, right: 10, bottom: 20 });
  });

  it("rectsIntersect detects overlap and gaps (edge-inclusive)", () => {
    const a: Rect = { left: 0, top: 0, right: 10, bottom: 10 };
    expect(rectsIntersect(a, { left: 5, top: 5, right: 15, bottom: 15 })).toBe(true); // overlap
    expect(rectsIntersect(a, { left: 10, top: 10, right: 20, bottom: 20 })).toBe(true); // touching edge
    expect(rectsIntersect(a, { left: 11, top: 0, right: 20, bottom: 10 })).toBe(false); // gap on x
    expect(rectsIntersect(a, { left: 0, top: 11, right: 10, bottom: 20 })).toBe(false); // gap on y
  });

  it("emittersInMarquee returns intersecting ids in row order", () => {
    // Three stacked rows 0..30, 30..60, 60..90 (x 0..100).
    const rows = [
      { id: 0, rect: { left: 0, top: 0, right: 100, bottom: 30 } },
      { id: 1, rect: { left: 0, top: 30, right: 100, bottom: 60 } },
      { id: 2, rect: { left: 0, top: 60, right: 100, bottom: 90 } },
    ];
    // A marquee covering rows 1 and 2 (top 40 → 80).
    expect(emittersInMarquee(rows, { left: 5, top: 40, right: 50, bottom: 80 })).toEqual([1, 2]);
    // A marquee in empty x-space hits nothing.
    expect(emittersInMarquee(rows, { left: 200, top: 0, right: 300, bottom: 90 })).toEqual([]);
  });

  it("mergeMarqueeSelection (non-additive) = swept, primary = last swept", () => {
    expect(mergeMarqueeSelection([], [1, 2])).toEqual({ ids: [1, 2], primary: 2 });
  });

  it("mergeMarqueeSelection (additive) unions base then swept, no dupes", () => {
    expect(mergeMarqueeSelection([5], [1, 5, 2])).toEqual({ ids: [5, 1, 2], primary: 2 });
  });

  it("mergeMarqueeSelection with nothing swept keeps base, primary = last base", () => {
    expect(mergeMarqueeSelection([5, 7], [])).toEqual({ ids: [5, 7], primary: 7 });
    expect(mergeMarqueeSelection([], [])).toEqual({ ids: [], primary: null });
  });
});
