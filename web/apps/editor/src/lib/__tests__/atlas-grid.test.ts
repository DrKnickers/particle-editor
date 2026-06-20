import { describe, it, expect } from "vitest";
import { ATLAS_MAX_SIDE, gridSide, frameCount, isAtlasEligible, isAtlasTooLarge,
  resolveFrame, cellRect, classifySelection } from "../atlas-grid";

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
describe("cellRect per-axis source crop", () => {
  it("square", () => expect(cellRect(5,4,256,256)).toEqual({ left:64, top:64, width:64, height:64 }));
  it("non-square", () => expect(cellRect(5,4,256,512)).toEqual({ left:64, top:128, width:64, height:128 }));
  it("first/last", () => {
    expect(cellRect(0,4,400,400)).toEqual({ left:0, top:0, width:100, height:100 });
    expect(cellRect(15,4,400,400)).toEqual({ left:300, top:300, width:100, height:100 });
  });
});
describe("classifySelection", () => {
  it("kinds", () => {
    expect(classifySelection([], null)).toBe("none");
    expect(classifySelection([0.1], 5)).toBe("single");
    expect(classifySelection([0.1], 99)).toBe("single");
    expect(classifySelection([0.1,0.5], 7)).toBe("multi-same");
    expect(classifySelection([0.1,0.5], null)).toBe("multi-diff");
  });
});
