import { describe, it, expect } from "vitest";
import { ATLAS_MAX_SIDE, gridSide, frameCount, isAtlasEligible, isAtlasTooLarge,
  resolveFrame, wrapFrame, cellRect, fitGridLayout } from "../atlas-grid";

describe("gridSide mirrors floor(sqrt(max(1,n)))", () => {
  it.each([[1,1],[2,1],[3,1],[4,2],[5,2],[9,3],[16,4],[20,4],[25,5],[0,1],[-7,1]])(
    "textureSize %i -> side %i", (n, side) => expect(gridSide(n)).toBe(side));
  it("NaN/Infinity -> 1", () => { expect(gridSide(NaN)).toBe(1); expect(gridSide(Infinity)).toBe(1); });
});
describe("frameCount / eligibility / too-large", () => {
  it("frameCount is side^2", () => { expect(frameCount(20)).toBe(16); expect(frameCount(16)).toBe(16); });
  it("eligible when side>=2", () => { expect(isAtlasEligible(3)).toBe(false); expect(isAtlasEligible(4)).toBe(true); });
  it("too large above cap", () => {
    expect(isAtlasTooLarge((ATLAS_MAX_SIDE+1)**2)).toBe(true);
    expect(isAtlasTooLarge(ATLAS_MAX_SIDE**2)).toBe(false);
    expect(isAtlasTooLarge(1_000_000)).toBe(true);
  });
});
describe("resolveFrame", () => {
  it("floors fractional", () => expect(resolveFrame(5.7, 4)).toBe(5));
  it("in range", () => { expect(resolveFrame(0,4)).toBe(0); expect(resolveFrame(15,4)).toBe(15); });
  it("out of range -> null", () => {
    expect(resolveFrame(16,4)).toBeNull(); expect(resolveFrame(-1,4)).toBeNull();
    expect(resolveFrame(NaN,4)).toBeNull(); expect(resolveFrame(Infinity,4)).toBeNull();
  });
});
describe("wrapFrame mirrors the engine's index % side² torus", () => {
  it("in range is unchanged (floored)", () => {
    expect(wrapFrame(0, 4)).toBe(0);
    expect(wrapFrame(3, 4)).toBe(3);
    expect(wrapFrame(2.7, 4)).toBe(2);
  });
  it("wraps index == count to 0 (the reported #563 case)", () => {
    expect(wrapFrame(4, 4)).toBe(0);   // 2×2 atlas, index 4 -> frame 0
    expect(wrapFrame(16, 16)).toBe(0); // 4×4 atlas, index 16 -> frame 0
  });
  it("wraps arbitrary overflow modulo count", () => {
    expect(wrapFrame(5, 4)).toBe(1);
    expect(wrapFrame(6, 4)).toBe(2);
    expect(wrapFrame(9, 4)).toBe(1);
  });
  it("wraps for non-power-of-two counts (3×3, 5×5 atlases)", () => {
    expect(wrapFrame(9, 9)).toBe(0);   // 3×3 atlas, index 9 -> frame 0
    expect(wrapFrame(10, 9)).toBe(1);
    expect(wrapFrame(8, 9)).toBe(8);   // in range, unchanged
    expect(wrapFrame(25, 25)).toBe(0); // 5×5 atlas, index 25 -> frame 0
    expect(wrapFrame(27, 25)).toBe(2);
    // NOTE: negative values on a non-power-of-two count intentionally diverge
    // from the engine's (unsigned int) cast (documented in wrapFrame); positive
    // overflow — the reported #563 case — matches exactly.
  });
  it("floored-modulo for negatives stays in [0,count)", () => {
    expect(wrapFrame(-1, 4)).toBe(3);
    expect(wrapFrame(-4, 4)).toBe(0);
  });
  it("degenerate inputs pass through", () => {
    expect(wrapFrame(NaN, 4)).toBeNaN();
    expect(wrapFrame(5, 0)).toBe(5);
  });
});
describe("cellRect per-axis source crop", () => {
  it("square", () => expect(cellRect(5,4,256,256)).toEqual({ left:64, top:64, width:64, height:64 }));
  it("non-square", () => expect(cellRect(5,4,256,512)).toEqual({ left:64, top:128, width:64, height:128 }));
  it("first/last", () => {
    expect(cellRect(0,4,400,400)).toEqual({ left:0, top:0, width:100, height:100 });
    expect(cellRect(15,4,400,400)).toEqual({ left:300, top:300, width:100, height:100 });
  });
});
describe("fitGridLayout (width-only, ~sqrt(n) columns, dense floor)", () => {
  const GAP = 4, MIN = 44, MAX = 160;
  it("4 frames -> 2x2 of big thumbnails (fills width)", () => {
    const { cols, cell } = fitGridLayout(4, 226, GAP, MIN, MAX);
    expect(cols).toBe(2);
    expect(cell).toBeGreaterThan(80);
    expect(cell).toBeLessThanOrEqual(MAX);
  });
  it("16 frames -> 4 columns", () => {
    expect(fitGridLayout(16, 226, GAP, MIN, MAX).cols).toBe(4);
  });
  it("64 frames -> columns capped to keep cells >= min (dense, scrolls)", () => {
    const { cols, cell } = fitGridLayout(64, 226, GAP, MIN, MAX);
    expect(cols).toBe(4); // sqrt=8, but the min-cell floor caps columns at this width
    expect(cell).toBeGreaterThanOrEqual(MIN);
  });
  it("a wider container gives a large atlas more columns", () => {
    expect(fitGridLayout(64, 420, GAP, MIN, MAX).cols).toBeGreaterThan(4);
  });
  it("caps the cell size for very few frames in a wide panel", () => {
    expect(fitGridLayout(4, 600, GAP, MIN, MAX).cell).toBe(MAX);
  });
  it("never overflows the available width", () => {
    for (const [n, w] of [[4, 226], [9, 226], [16, 226], [25, 300], [36, 180], [64, 213]] as const) {
      const { cols, cell } = fitGridLayout(n, w, GAP, MIN, MAX);
      expect(cols * cell + (cols - 1) * GAP).toBeLessThanOrEqual(w);
    }
  });
  it("degenerate (n<=0 or w<=0) -> 1 column, min cell", () => {
    expect(fitGridLayout(0, 226, GAP, MIN, MAX)).toEqual({ cols: 1, cell: MIN });
    expect(fitGridLayout(4, 0, GAP, MIN, MAX)).toEqual({ cols: 1, cell: MIN });
  });
  it("crosses a column-count boundary in a single clean step (no oscillation)", () => {
    // maxColsForMin ticks 4->5 at w = 5*(MIN+GAP)-GAP = 236
    expect(fitGridLayout(64, 235, GAP, MIN, MAX).cols).toBe(4);
    expect(fitGridLayout(64, 236, GAP, MIN, MAX).cols).toBe(5);
  });
  it("handles sub-pixel width: integer cell, no overflow", () => {
    const { cols, cell } = fitGridLayout(16, 225.6, GAP, MIN, MAX);
    expect(Number.isInteger(cell)).toBe(true);
    expect(cols * cell + (cols - 1) * GAP).toBeLessThanOrEqual(225.6);
  });
  it("non-square n still yields a valid, non-overflowing layout", () => {
    const { cols, cell } = fitGridLayout(20, 226, GAP, MIN, MAX);
    expect(cols).toBeGreaterThanOrEqual(1);
    expect(cell).toBeGreaterThanOrEqual(MIN);
    expect(cols * cell + (cols - 1) * GAP).toBeLessThanOrEqual(226);
  });
});
