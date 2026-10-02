import { describe, it, expect } from "vitest";
import { deadCellsFromAlpha, DEAD_ALPHA_MAX } from "../atlas-dead-cells";

// Build an RGBA buffer (imgW×imgH) where a per-pixel alpha is chosen by (x,y).
// RGB is fixed opaque-looking (255) so the test proves detection keys on ALPHA,
// not luminance — the whole point (dead cells have real RGB but zero alpha).
function rgba(imgW: number, imgH: number, alphaAt: (x: number, y: number) => number): Uint8ClampedArray {
  const buf = new Uint8ClampedArray(imgW * imgH * 4);
  for (let y = 0; y < imgH; y++) {
    for (let x = 0; x < imgW; x++) {
      const i = (y * imgW + x) * 4;
      buf[i] = 255; buf[i + 1] = 255; buf[i + 2] = 255;
      buf[i + 3] = alphaAt(x, y);
    }
  }
  return buf;
}

describe("deadCellsFromAlpha", () => {
  it("flags cells whose max alpha is ~0, keying on ALPHA not RGB (4×4, side 2)", () => {
    // 4 cells of 2×2. Right column (cells 1,3) transparent; left column opaque.
    const buf = rgba(4, 4, (x) => (x >= 2 ? 0 : 255));
    expect(deadCellsFromAlpha(buf, 4, 4, 2)).toEqual(new Set([1, 3]));
  });

  it("returns empty for an all-opaque texture (nothing dimmed)", () => {
    const buf = rgba(8, 8, () => 255);
    expect(deadCellsFromAlpha(buf, 8, 8, 2)).toEqual(new Set());
  });

  it("flags every cell for an all-transparent texture", () => {
    const buf = rgba(8, 8, () => 0);
    expect(deadCellsFromAlpha(buf, 8, 8, 2)).toEqual(new Set([0, 1, 2, 3]));
  });

  it("uses frame index k = row*side + col (matches the picker grid)", () => {
    // Only the cell at row 1, col 0 (k = 2) is transparent.
    const buf = rgba(4, 4, (x, y) => (x < 2 && y >= 2 ? 0 : 255));
    expect(deadCellsFromAlpha(buf, 4, 4, 2)).toEqual(new Set([2]));
  });

  it("respects the threshold boundary (< DEAD_ALPHA_MAX is dead)", () => {
    // Single cell, uniform alpha just below vs at the threshold.
    const below = rgba(2, 2, () => DEAD_ALPHA_MAX - 1);
    const at = rgba(2, 2, () => DEAD_ALPHA_MAX);
    expect(deadCellsFromAlpha(below, 2, 2, 1)).toEqual(new Set([0]));
    expect(deadCellsFromAlpha(at, 2, 2, 1)).toEqual(new Set());
  });

  it("a single opaque pixel keeps a cell alive (max, not mean)", () => {
    // One opaque pixel in an otherwise-transparent cell → NOT dead.
    const buf = rgba(4, 4, (x, y) => (x === 3 && y === 3 ? 255 : 0));
    const dead = deadCellsFromAlpha(buf, 4, 4, 2);
    expect(dead.has(3)).toBe(false); // bottom-right cell has the one opaque pixel
    expect(dead).toEqual(new Set([0, 1, 2])); // the other three are fully transparent
  });

  it("handles non-divisible dimensions via proportional cell bounds (5×5, side 2)", () => {
    // Left ~half transparent, right ~half opaque; proportional split still classifies.
    const buf = rgba(5, 5, (x) => (x < 2 ? 0 : 255));
    const dead = deadCellsFromAlpha(buf, 5, 5, 2);
    expect(dead.has(0)).toBe(true);  // top-left (x 0..1) transparent
    expect(dead.has(1)).toBe(false); // top-right (x 2..4) opaque
  });

  it("side 1 → the whole texture is one cell", () => {
    expect(deadCellsFromAlpha(rgba(4, 4, () => 0), 4, 4, 1)).toEqual(new Set([0]));
    expect(deadCellsFromAlpha(rgba(4, 4, () => 255), 4, 4, 1)).toEqual(new Set());
  });

  it("degenerate side > image dim: unsamplable (zero-area) cells are NOT flagged dead", () => {
    // side 4 over a 2×2 image → many cells have zero pixel area; fail OPEN (don't dim
    // what we can't verify) rather than falsely dimming.
    const dead = deadCellsFromAlpha(rgba(2, 2, () => 255), 2, 2, 4);
    expect(dead.size).toBe(0);
  });

  it("guards invalid inputs (empty set, no throw)", () => {
    expect(deadCellsFromAlpha(new Uint8ClampedArray(0), 0, 0, 8)).toEqual(new Set());
    expect(deadCellsFromAlpha(rgba(4, 4, () => 0), 4, 4, 0)).toEqual(new Set());
  });
});
