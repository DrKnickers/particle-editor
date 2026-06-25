// Unit tests for the pure autoscroll-delta decision used by the
// emitter-tree reorder drag. jsdom can't exercise real layout/scroll, so the
// only logic worth unit-testing is this pure ramp — the live scrolling is
// verified in the browser preview.

import { describe, it, expect } from "vitest";
import { computeAutoscrollDelta } from "../drag-autoscroll";

// Container spans y ∈ [100, 500]; defaults zone=28, maxSpeed=12.
const RECT = { top: 100, bottom: 500 };

describe("computeAutoscrollDelta", () => {
  it("is zero when the pointer is well inside the list", () => {
    expect(computeAutoscrollDelta(300, RECT)).toBe(0);
  });

  it("is zero exactly at the zone boundary", () => {
    expect(computeAutoscrollDelta(100 + 28, RECT)).toBe(0); // top boundary
    expect(computeAutoscrollDelta(500 - 28, RECT)).toBe(0); // bottom boundary
  });

  it("scrolls up (negative) near the top edge, at full speed at the edge", () => {
    expect(computeAutoscrollDelta(100, RECT)).toBeCloseTo(-12);
  });

  it("scrolls down (positive) near the bottom edge, at full speed at the edge", () => {
    expect(computeAutoscrollDelta(500, RECT)).toBeCloseTo(12);
  });

  it("ramps proportionally with depth into the zone", () => {
    // 14px below the top = halfway through the 28px zone → half speed.
    expect(computeAutoscrollDelta(114, RECT)).toBeCloseTo(-6);
    // 14px above the bottom = halfway → half speed, downward.
    expect(computeAutoscrollDelta(486, RECT)).toBeCloseTo(6);
  });

  it("clamps to max speed past the edges (pointer outside the container)", () => {
    expect(computeAutoscrollDelta(50, RECT)).toBeCloseTo(-12);  // above top
    expect(computeAutoscrollDelta(600, RECT)).toBeCloseTo(12);  // below bottom
  });

  it("scrolls DOWN when the pointer is nearer the bottom in a viewport shorter than 2×zone", () => {
    // Viewport height 40 (< 2*28): the top and bottom edge zones overlap.
    // A pointer at y=25 is 15px from the bottom but 25px from the top — nearer
    // the bottom, so it must scroll DOWN. The old top-zone-first short-circuit
    // returned UP here, making the end-of-list gap unreachable on a short panel.
    const SHORT = { top: 0, bottom: 40 };
    expect(computeAutoscrollDelta(25, SHORT)).toBeGreaterThan(0);
    // And a pointer nearer the top in the same overlap still scrolls UP.
    expect(computeAutoscrollDelta(15, SHORT)).toBeLessThan(0);
  });

  it("respects custom zone and maxSpeed", () => {
    // rect [0,200], zone 20, maxSpeed 5; 10px in = halfway → -2.5.
    expect(
      computeAutoscrollDelta(10, { top: 0, bottom: 200 }, { zone: 20, maxSpeed: 5 }),
    ).toBeCloseTo(-2.5);
  });
});
