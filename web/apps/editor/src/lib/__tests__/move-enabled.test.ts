import { describe, it, expect } from "vitest";
import { canMoveSelection } from "@/lib/move-enabled";

const ROOTS = [10, 11, 12, 13]; // ids top-to-bottom

describe("canMoveSelection", () => {
  it("is false with no roots", () => {
    expect(canMoveSelection([10], [], "up")).toBe(false);
  });

  it("is false when the target has no root (e.g. only children selected)", () => {
    expect(canMoveSelection([99], ROOTS, "up")).toBe(false);
  });

  it("single root not at the edge can move", () => {
    expect(canMoveSelection([11], ROOTS, "up")).toBe(true);
    expect(canMoveSelection([11], ROOTS, "down")).toBe(true);
  });

  it("single root at the top can't move up; at the bottom can't move down", () => {
    expect(canMoveSelection([10], ROOTS, "up")).toBe(false);
    expect(canMoveSelection([10], ROOTS, "down")).toBe(true);
    expect(canMoveSelection([13], ROOTS, "down")).toBe(false);
    expect(canMoveSelection([13], ROOTS, "up")).toBe(true);
  });

  it("contiguous block away from the edge can move both ways", () => {
    expect(canMoveSelection([11, 12], ROOTS, "up")).toBe(true);
    expect(canMoveSelection([11, 12], ROOTS, "down")).toBe(true);
  });

  it("block pinned at the top freezes up but can move down", () => {
    expect(canMoveSelection([10, 11], ROOTS, "up")).toBe(false);
    expect(canMoveSelection([10, 11], ROOTS, "down")).toBe(true);
  });

  it("non-contiguous selection: pinned-at-top blocks up, free at bottom allows down (symmetric)", () => {
    // {top, middle} — top pinned: up frozen, down free.
    expect(canMoveSelection([10, 12], ROOTS, "up")).toBe(false);
    expect(canMoveSelection([10, 12], ROOTS, "down")).toBe(true);
    // {middle, bottom} — bottom pinned: down frozen, up free.
    expect(canMoveSelection([11, 13], ROOTS, "down")).toBe(false);
    expect(canMoveSelection([11, 13], ROOTS, "up")).toBe(true);
  });

  it("selection pinned at BOTH edges freezes both directions", () => {
    expect(canMoveSelection([10, 13], ROOTS, "up")).toBe(false);
    expect(canMoveSelection([10, 13], ROOTS, "down")).toBe(false);
  });
});
