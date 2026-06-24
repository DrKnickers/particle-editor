// WCAG AA contrast guard for the design tokens.
//
// Parses tokens.css for the dark (:root) and light ([data-theme="light"])
// values and asserts that the foreground/background pairs the UI actually uses
// clear WCAG 2.1 AA for normal text (>= 4.5:1). This is the durable rail that
// catches a token edit silently dropping a pair below AA — there was no such
// guard before the accent-button contrast pass, and the comments in tokens.css
// (e.g. "~2.6:1") were the only record of the problem.
//
// Scope note: it asserts the pairs this pass fixed (white-on-accent-strong,
// text-3) plus the always-on text pairs as regression coverage. Tertiary text
// is checked against the canonical bg/bg-2 surfaces (not the rare text-on-
// panel-3 case).

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";

// Vitest runs from the package dir (web/apps/editor), so this relative path
// resolves to the token source. (import.meta.url / ?raw are unreliable under
// the vitest transform — see the prior attempts in git history.)
const css = readFileSync("src/styles/tokens.css", "utf8");

/** Pull `--name: #hex;` from the declaration block starting at `marker`.
 *  Bounded to the block (ends at the next `\n}`) so a token deleted from the
 *  dark `:root` block can't silently read the light override and pass. */
function token(marker: string, name: string): string {
  const from = css.indexOf(marker);
  if (from < 0) throw new Error(`block '${marker}' not found`);
  const rest = css.slice(from + marker.length);
  const end = rest.indexOf("\n}");
  const block = end >= 0 ? rest.slice(0, end) : rest;
  const m = block.match(new RegExp(`--${name}:\\s*(#[0-9a-fA-F]{6})`));
  if (!m) throw new Error(`token --${name} not found in '${marker}' block`);
  return m[1]!;
}

function srgbToLinear(c: number): number {
  const s = c / 255;
  return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
}

function luminance(hex: string): number {
  const n = parseInt(hex.slice(1), 16);
  const r = srgbToLinear((n >> 16) & 0xff);
  const g = srgbToLinear((n >> 8) & 0xff);
  const b = srgbToLinear(n & 0xff);
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

/** WCAG contrast ratio between two hex colours (order-independent). */
function contrast(a: string, b: string): number {
  const la = luminance(a);
  const lb = luminance(b);
  const [hi, lo] = la >= lb ? [la, lb] : [lb, la];
  return (hi + 0.05) / (lo + 0.05);
}

const AA = 4.5;
const WHITE = "#ffffff";

const THEMES = [
  { name: "dark", marker: ":root {" },
  { name: "light", marker: '[data-theme="light"] {' },
] as const;

describe("token contrast (WCAG AA, normal text >= 4.5:1)", () => {
  for (const { name, marker } of THEMES) {
    const t = (n: string) => token(marker, n);

    it(`${name}: white text on --accent-strong (primary buttons / fills)`, () => {
      expect(contrast(WHITE, t("accent-strong"))).toBeGreaterThanOrEqual(AA);
      // hover stays >= AA too (darken-on-hover must not regress)
      expect(contrast(WHITE, t("accent-strong-hover"))).toBeGreaterThanOrEqual(AA);
    });

    it(`${name}: --text-3 (hints) on --bg and --bg-2`, () => {
      expect(contrast(t("text-3"), t("bg"))).toBeGreaterThanOrEqual(AA);
      expect(contrast(t("text-3"), t("bg-2"))).toBeGreaterThanOrEqual(AA);
    });

    it(`${name}: --text and --text-2 on --bg (regression guard)`, () => {
      expect(contrast(t("text"), t("bg"))).toBeGreaterThanOrEqual(AA);
      expect(contrast(t("text-2"), t("bg"))).toBeGreaterThanOrEqual(AA);
    });

    it(`${name}: semantic -fg text tokens on --panel (regression guard)`, () => {
      // The -fg variants are explicitly tuned for the --panel surface (see the
      // tokens.css comment), not --bg — assert against that documented surface.
      for (const fg of ["danger-fg", "success-fg", "warning-fg"]) {
        expect(contrast(t(fg), t("panel"))).toBeGreaterThanOrEqual(AA);
      }
    });
  }

  it("documents that bare --accent is too light for white text (why -strong exists)", () => {
    // Guards the rationale: if someone darkens --accent enough to pass here,
    // --accent-strong may be redundant — this test will flag that to revisit.
    expect(contrast(WHITE, token(":root {", "accent"))).toBeLessThan(AA);
  });
});
