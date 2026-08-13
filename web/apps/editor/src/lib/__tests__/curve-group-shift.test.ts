// Unit tests for the rigid group time-shift clamp (#619).
import { describe, it, expect } from "vitest";
import { clampGroupTimeShift } from "../curve-group-shift";

// Keys [0,25,50,100] → span 100 → eps 0.01.
const KEYS = [0, 25, 50, 100];
const BORDERS = new Set([0, 100]);

describe("clampGroupTimeShift", () => {
  it("leaves an in-range shift untouched", () => {
    // 25 → 35, well left of the unselected wall at 50.
    expect(clampGroupTimeShift(KEYS, new Set([25]), BORDERS, 10)).toBeCloseTo(10, 6);
  });

  it("stops a rightward shift eps before the nearest unselected key", () => {
    // 25 shifted +30 would reach 55, past the unselected 50 → clamp so 25 lands
    // at 50 - eps ⇒ dTime = 25 - 0.01 = 24.99.
    expect(clampGroupTimeShift(KEYS, new Set([25]), BORDERS, 30)).toBeCloseTo(24.99, 6);
  });

  it("stops a leftward shift eps after the nearest unselected key", () => {
    // 50 shifted -30 would reach 20, past the unselected 25 → clamp so 50 lands
    // at 25 + eps ⇒ dTime = -(25 - 0.01) = -24.99.
    expect(clampGroupTimeShift(KEYS, new Set([50]), BORDERS, -30)).toBeCloseTo(-24.99, 6);
  });

  it("bounds a contiguous run by its leading edge's wall", () => {
    // [0,25,30,50,100]; run {25,30} moves together, bounded by 30→50.
    const keys = [0, 25, 30, 50, 100];
    // eps = 100/10000 = 0.01. 30 stops at 50-eps ⇒ dTime = 20 - 0.01 = 19.99.
    expect(clampGroupTimeShift(keys, new Set([25, 30]), new Set([0, 100]), 30)).toBeCloseTo(19.99, 6);
  });

  it("bounds a NON-contiguous selection by the tightest key", () => {
    // [0,25,40,60,100]; select {25,60}. 25's right wall is the unselected 40
    // (tightest); 60's is 100. So the whole selection stops when 25 reaches 40.
    const keys = [0, 25, 40, 60, 100];
    // dTime = (40 - 25) - 0.01 = 14.99.
    expect(clampGroupTimeShift(keys, new Set([25, 60]), new Set([0, 100]), 30)).toBeCloseTo(14.99, 6);
  });

  it("treats a selected BORDER key as pinned (a wall), not movable", () => {
    // [0,25,100]; select {0,25} — 0 is a border (pinned). Only 25 moves; its
    // right wall is 100. dTime = (100 - 25) - 0.01 = 74.99.
    expect(clampGroupTimeShift([0, 25, 100], new Set([0, 25]), new Set([0, 100]), 80)).toBeCloseTo(74.99, 6);
  });

  it("returns 0 when the selection is all-border (nothing moves)", () => {
    expect(clampGroupTimeShift(KEYS, new Set([0, 100]), BORDERS, 30)).toBe(0);
  });

  it("returns 0 when a key already sits against its wall", () => {
    // 25 selected, but an unselected key at 26 leaves no room to the right.
    const keys = [0, 25, 26, 100];
    // eps ~0.01; right room = 26-25-0.01 = 0.99 > 0, so +5 clamps to ~0.99.
    expect(clampGroupTimeShift(keys, new Set([25]), new Set([0, 100]), 5)).toBeCloseTo(0.99, 6);
  });
});
