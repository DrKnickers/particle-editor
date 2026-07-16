// Unit tests for the pure snap-to-grid math (#618).
import { describe, it, expect } from "vitest";
import {
  snapToGrid,
  GRID_CELLS,
  GRID_SUBDIVISIONS,
  GRID_MINOR_CELLS,
} from "../curve-snap";

describe("snapToGrid", () => {
  it("snaps a mid-cell value to the nearest stop", () => {
    // 50 stops over [0,100] → step 2. 5.4 → nearest stop is 6.
    expect(snapToGrid(5.4, 0, 100)).toBeCloseTo(6, 10);
    // 4.9 → nearest stop is 4.
    expect(snapToGrid(4.9, 0, 100)).toBeCloseTo(4, 10);
  });

  it("leaves a value already on a stop unchanged", () => {
    expect(snapToGrid(6, 0, 100)).toBeCloseTo(6, 10);
    expect(snapToGrid(50, 0, 100)).toBeCloseTo(50, 10);
  });

  it("keeps the endpoints exact", () => {
    expect(snapToGrid(0, 0, 100)).toBeCloseTo(0, 10);
    expect(snapToGrid(100, 0, 100)).toBeCloseTo(100, 10);
  });

  it("returns the value unchanged for a degenerate axis (min === max)", () => {
    expect(snapToGrid(42, 7, 7)).toBe(42);
  });

  it("returns the value unchanged for non-finite input or non-positive cells", () => {
    expect(snapToGrid(Number.NaN, 0, 100)).toBeNaN();
    expect(snapToGrid(5.4, 0, 100, 0)).toBe(5.4);
    expect(snapToGrid(5.4, 0, 100, -1)).toBe(5.4);
  });

  it("handles a non-zero min and negative ranges", () => {
    // [-10, 10] over 50 stops → step 0.4. -3.1 → nearest stop -3.2.
    expect(snapToGrid(-3.1, -10, 10)).toBeCloseTo(-3.2, 10);
  });

  it("respects a custom cell count", () => {
    // 10 stops over [0,100] → step 10. 46 → 50.
    expect(snapToGrid(46, 0, 100, 10)).toBeCloseTo(50, 10);
  });

  it("keeps in-range values in-range after snapping", () => {
    for (const v of [0.01, 12.3, 55.5, 99.99]) {
      const s = snapToGrid(v, 0, 100);
      expect(s).toBeGreaterThanOrEqual(0);
      expect(s).toBeLessThanOrEqual(100);
    }
  });

  it("exposes grid constants consistent with the renderer", () => {
    expect(GRID_CELLS).toBe(10);
    expect(GRID_SUBDIVISIONS).toBe(5);
    expect(GRID_MINOR_CELLS).toBe(50);
  });
});
