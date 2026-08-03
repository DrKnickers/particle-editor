// Vitest unit tests for the link-group colour palette.

import { describe, it, expect } from "vitest";
import {
  BRACKET_PALETTE_SIZE,
  colorForGroup,
} from "../link-group-colors";

describe("link-group-colors", () => {
  it("colorForGroup returns null for unlinked (group 0) and cycles through 8 colours for non-zero groups", () => {
    expect(BRACKET_PALETTE_SIZE).toBe(8);
    expect(colorForGroup(0)).toBeNull();
    // Group 1 → palette[0]; group 9 wraps back to palette[0].
    const c1 = colorForGroup(1)!;
    const c9 = colorForGroup(9)!;
    expect(c1).toBeTruthy();
    expect(c1).toBe(c9);
    // Group 2..8 should each be distinct from group 1.
    const seen = new Set([c1]);
    for (let g = 2; g <= 8; g++) {
      const c = colorForGroup(g)!;
      expect(c).toBeTruthy();
      seen.add(c);
    }
    expect(seen.size).toBe(8);
  });
});
