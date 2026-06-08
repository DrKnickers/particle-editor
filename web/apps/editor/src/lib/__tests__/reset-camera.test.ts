// Locks the shared Reset-Camera constant to the legacy engine default so a
// stray edit to one source can't silently diverge the menu item and the
// Ctrl+Home accelerator from the legacy ID_VIEW_RESETCAMERA behaviour ().
//
// Legacy reference: src/main.cpp:1834 (ID_VIEW_RESETCAMERA) and the engine
// constructor default src/engine.cpp:2190-2192 — eye (0,-250,125), target
// origin, up +Z. These vectors are identical at both legacy sites.

import { describe, it, expect } from "vitest";
import { RESET_CAMERA } from "../reset-camera";

describe("RESET_CAMERA", () => {
  it("matches the legacy ID_VIEW_RESETCAMERA / engine-constructor default", () => {
    expect(RESET_CAMERA).toEqual({
      position: [0, -250, 125],
      target: [0, 0, 0],
      up: [0, 0, 1],
    });
  });
});
