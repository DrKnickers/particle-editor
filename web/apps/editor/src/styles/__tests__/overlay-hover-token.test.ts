// Intent guard for the atlas-cell hover tint (--overlay-hover). The className
// test guards only the scale lift, so without this a silent revert of the tint
// (or a wrong-block edit hitting the light override) would pass green.
//
// We do NOT pin the exact alphas: they're eyeballed, live-tuned aesthetics
// (dark 0.22, light 0.18) and may be re-tuned again — freezing the literals
// would force a double-edit on every future tweak and train maintainers to
// ignore the test. Instead we guard intent + channel identity per theme:
//   - dark is a WHITE tint, brighter than the old near-invisible 0.14 (>= 0.18
//     floor) — catches a silent revert and a wrong-block edit that leaves dark low;
//   - light is a BLACK tint, darker than the old faint 0.12 (>= 0.14 floor) —
//     same protection, and the per-channel checks catch a dark (white) value
//     leaking into the light block (or vice-versa) in both directions.
// Bounds each match to its theme block (ends at the next `\n}`) — the same
// readFileSync(tokens.css) approach as contrast.test.ts.
import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";

// Vitest runs from the package dir (web/apps/editor).
const css = readFileSync("src/styles/tokens.css", "utf8");

/** Parse `--overlay-hover: rgba(r,g,b,a)` from the block starting at `marker`,
 *  bounded to that block. Returns the normalized "r,g,b" channel + numeric alpha. */
function overlayHover(marker: string): { channel: string; alpha: number } {
  const from = css.indexOf(marker);
  if (from < 0) throw new Error(`block '${marker}' not found`);
  const rest = css.slice(from + marker.length);
  const end = rest.indexOf("\n}");
  const block = end >= 0 ? rest.slice(0, end) : rest;
  const m = block.match(/--overlay-hover:\s*rgba\(([^)]*)\)/);
  if (!m) throw new Error(`--overlay-hover not found in '${marker}' block`);
  const parts = m[1]!.split(",").map((s) => s.trim());
  return { channel: parts.slice(0, 3).join(","), alpha: Number(parts[3]) };
}

describe("--overlay-hover (atlas-cell hover tint)", () => {
  it("dark (:root) is a white tint brighter than the old 0.14", () => {
    const { channel, alpha } = overlayHover(":root {");
    expect(channel).toBe("255,255,255");
    expect(alpha).toBeGreaterThanOrEqual(0.18);
  });

  it("light override is a black tint darker than the old 0.12", () => {
    const { channel, alpha } = overlayHover('[data-theme="light"] {');
    expect(channel).toBe("0,0,0");
    expect(alpha).toBeGreaterThanOrEqual(0.14);
  });
});
