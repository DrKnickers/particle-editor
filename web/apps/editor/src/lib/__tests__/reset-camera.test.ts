// Locks the shared Reset-Camera constant to the legacy engine default so a
// stray edit to one source can't silently diverge the menu item and the
// Ctrl+Home accelerator from the original editor's Reset Camera behaviour.
//
// Legacy reference: the original editor's Reset Camera command and the
// Engine constructor default in src/rendering/engine.cpp — eye (0,-250,125), target
// origin, up +Z. These vectors are identical at both legacy sites.

import { describe, it, expect } from "vitest";
import { RESET_CAMERA } from "../reset-camera";

describe("RESET_CAMERA", () => {
  it("matches the original editor's Reset Camera / engine-constructor default", () => {
    expect(RESET_CAMERA).toEqual({
      position: [0, -250, 125],
      target: [0, 0, 0],
      up: [0, 0, 1],
    });
  });
});
