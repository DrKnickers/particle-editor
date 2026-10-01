// Unit tests for the pure curve-domain helpers in lib/curve-model.ts:
// the per-track y-axis range and the multi-key group-move transform.
import { describe, it, expect } from "vitest";
import type { TrackDto, TrackName } from "@particle-editor/bridge-schema";
import { computeGroupMoves, valueRangeForTrack } from "../curve-model";

function track(name: TrackName, values: number[]): TrackDto {
  return {
    name,
    keys: values.map((value, i) => ({ time: i * 10, value })),
    interpolation: "linear",
    lockedTo: null,
  };
}

describe("valueRangeForTrack", () => {
  it.each(["red", "green", "blue", "alpha"] as const)(
    "%s is a fixed [0, 1] range regardless of key values",
    (name) => {
      expect(valueRangeForTrack(track(name, [5, -3]))).toEqual({ min: 0, max: 1 });
    },
  );

  it.each(["scale", "index"] as const)("%s grows its max to the largest key value", (name) => {
    expect(valueRangeForTrack(track(name, [0.5, 3, 2]))).toEqual({ min: 0, max: 3 });
  });

  it.each(["scale", "index"] as const)(
    "%s floors its max at 1 and keeps min at 0 (small, negative, or no keys)",
    (name) => {
      expect(valueRangeForTrack(track(name, [0.2, 0.4]))).toEqual({ min: 0, max: 1 });
      expect(valueRangeForTrack(track(name, [-5]))).toEqual({ min: 0, max: 1 });
      expect(valueRangeForTrack(track(name, []))).toEqual({ min: 0, max: 1 });
    },
  );

  it("rotationSpeed spans [0, 1] at minimum and extends both ways to fit the keys", () => {
    expect(valueRangeForTrack(track("rotationSpeed", []))).toEqual({ min: 0, max: 1 });
    expect(valueRangeForTrack(track("rotationSpeed", [0.25, 0.75]))).toEqual({ min: 0, max: 1 });
    expect(valueRangeForTrack(track("rotationSpeed", [-2, 0.5]))).toEqual({ min: -2, max: 1 });
    expect(valueRangeForTrack(track("rotationSpeed", [0.3, 4]))).toEqual({ min: 0, max: 4 });
    expect(valueRangeForTrack(track("rotationSpeed", [-1.5, 6]))).toEqual({ min: -1.5, max: 6 });
  });
});

// Moved verbatim from CurveEditorPanel.test.tsx (pure function, no component).
// ─── computeGroupMoves — group-drag clamp (#619 collision, #620 value bounds) ──
describe("computeGroupMoves (#619/#620)", () => {
  const keys = [
    { time: 0, value: 0 },
    { time: 25, value: 5 },
    { time: 50, value: 0 },
    { time: 100, value: 0 },
  ];
  const borders = new Set([0, 100]);

  it("bounds a rightward group shift so no selected key crosses an unselected key (#619)", () => {
    // Select {25}; +30 would reach 55, past the unselected key at 50. The rigid
    // clamp stops it eps before 50 — no colliding newTime.
    const moves = computeGroupMoves(keys, new Set([25]), borders, 30, 0, { min: 0, max: 1e6 });
    expect(moves).toHaveLength(1);
    expect(moves[0]!.newTime).toBeLessThan(50);
    expect(moves[0]!.newTime).toBeCloseTo(49.99, 2); // 50 - eps
    // The committed newTime must not equal any unselected key's time.
    expect(keys.some((k) => !new Set([25]).has(k.time) && k.time === moves[0]!.newTime)).toBe(false);
  });

  it("clamps the value to the PASSED bounds so canvas (display) and spinner (engine) differ (#620)", () => {
    // dValue +20 on value 5 → 25. With the display range (max 10) it clamps to 10
    // (what a canvas drag commits, matching the preview); with wide spinner bounds
    // it stays 25 (what a spinner group edit commits).
    const display = computeGroupMoves(keys, new Set([25]), borders, 0, 20, { min: 0, max: 10 });
    expect(display[0]!.newValue).toBe(10);
    const spinner = computeGroupMoves(keys, new Set([25]), borders, 0, 20, { min: 0, max: 1e6 });
    expect(spinner[0]!.newValue).toBe(25);
  });
});

describe("computeGroupMoves — edge cases", () => {
  // Keys [0,25,50,100] → span 100 → eps 0.01.
  const keys = [
    { time: 0, value: 1 },
    { time: 25, value: 5 },
    { time: 50, value: 3 },
    { time: 100, value: 2 },
  ];
  const borders = new Set([0, 100]);
  const wide = { min: -1e6, max: 1e6 };

  it("returns no moves for an empty track", () => {
    expect(computeGroupMoves([], new Set([0]), new Set(), 10, 1, wide)).toEqual([]);
  });

  it("emits only selected keys, in track order, with oldTime echoing the source key", () => {
    const moves = computeGroupMoves(keys, new Set([50, 25]), borders, 5, 0, wide);
    expect(moves.map((m) => m.oldTime)).toEqual([25, 50]);
    expect(moves.map((m) => m.newTime)).toEqual([30, 55]);
  });

  it("returns nothing when no selected time matches a key", () => {
    expect(computeGroupMoves(keys, new Set([12]), borders, 5, 1, wide)).toEqual([]);
  });

  it("pins a selected border key in time but still shifts its value", () => {
    const moves = computeGroupMoves(keys, new Set([0, 25]), borders, 10, 1, wide);
    expect(moves).toEqual([
      { oldTime: 0, newTime: 0, newValue: 2 },
      { oldTime: 25, newTime: 35, newValue: 6 },
    ]);
  });

  it("a border-only selection never moves in time (clamped shift is 0)", () => {
    const moves = computeGroupMoves(keys, new Set([0, 100]), borders, 40, 0, wide);
    expect(moves.map((m) => m.newTime)).toEqual([0, 100]);
  });

  it("bounds a leftward shift eps after the nearest wall", () => {
    // Select {50}; −40 would reach 10, past the unselected key at 25.
    const moves = computeGroupMoves(keys, new Set([50]), borders, -40, 0, wide);
    expect(moves[0]!.newTime).toBeCloseTo(25.01, 6);
  });

  it("clamps the value at the lower bound too", () => {
    const moves = computeGroupMoves(keys, new Set([25]), borders, 0, -100, { min: 0, max: 10 });
    expect(moves[0]!.newValue).toBe(0);
  });
});
