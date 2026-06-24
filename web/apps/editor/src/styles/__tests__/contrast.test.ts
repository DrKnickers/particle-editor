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

    it(`${name}: white text on --danger-strong (destructive button)`, () => {
      expect(contrast(WHITE, t("danger-strong"))).toBeGreaterThanOrEqual(AA);
      expect(contrast(WHITE, t("danger-strong-hover"))).toBeGreaterThanOrEqual(AA);
    });

    it(`${name}: --text-3 (hints) on every surface it renders on`, () => {
      // text-3 carries hint / disabled / placeholder text on the base + panel
      // surfaces; all clear AA. `--panel-3` is deliberately NOT asserted: it is
      // dark-theme ~4.27:1 with text-3, but text-3 NEVER renders on it — every
      // panel-3 background (tooltip surface, .tb-btn / .panel-header .icon-btn
      // :active press, button hover) carries --text or --text-2 (both AA there).
      // Lightening text-3 to clear panel-3 would wash out tertiary text on every
      // real surface to satisfy a pairing that doesn't occur.
      for (const bg of ["bg", "bg-2", "panel", "panel-2"]) {
        expect(contrast(t("text-3"), t(bg))).toBeGreaterThanOrEqual(AA);
      }
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

    it(`${name}: semantic -fg text tokens also clear AA on --bg and --bg-2`, () => {
      // The semantic -fg tokens render inline status copy on the base canvas
      // surfaces too (e.g. SetLinkGroupDialog's warning note sits on --bg; the
      // texture-palette "broken" placeholder + status row sit on --bg-2). Per
      // the audit these already pass with the worst margin ~4.58:1; lock it so
      // a token edit can't silently drop a real pairing below AA.
      for (const fg of ["danger-fg", "success-fg", "warning-fg"]) {
        for (const bg of ["bg", "bg-2"]) {
          expect(contrast(t(fg), t(bg))).toBeGreaterThanOrEqual(AA);
        }
      }
    });
  }

  it("documents that bare --accent / --danger are too light for white text (why -strong exists)", () => {
    // Guards the rationale: if someone darkens the base token enough to pass
    // here, the -strong variant may be redundant — flag that to revisit.
    expect(contrast(WHITE, token(":root {", "accent"))).toBeLessThan(AA);
    expect(contrast(WHITE, token(":root {", "danger"))).toBeLessThan(AA);
  });
});

// --- Viewport pill light scrim: composited AA guard -------------------------
// The light scrim appears ONLY over a genuinely-dark render, so its glyphs must
// clear AA against the scrim composited over the darkest triggering backdrop
// (pure black) — NOT against pure white. Eyeballing it against white is what let
// the first draft ship a 3.1:1 lock accent. (Asserted at the interactive
// opacity-1 state; the idle 0.4 is a deliberate decorative de-emphasis.)
const componentsCss = readFileSync("src/styles/components.css", "utf8");

function lightScrimBlock(): string {
  const m = componentsCss.match(/\.vp-overlay\[data-scrim="light"\]\s*\{([^}]*)\}/);
  if (!m) throw new Error('.vp-overlay[data-scrim="light"] block not found');
  return m[1]!;
}
function scrimHex(block: string, name: string): string {
  const m = block.match(new RegExp(`--${name}:\\s*(#[0-9a-fA-F]{6})`));
  if (!m) throw new Error(`--${name} (#hex) not found in light-scrim block`);
  return m[1]!;
}
/** Composite an `rgba(r,g,b,a)` scrim token over a solid backdrop → #hex. */
function scrimCompositeOver(block: string, name: string, bd: [number, number, number]): string {
  const m = block.match(new RegExp(`--${name}:\\s*rgba\\((\\d+),\\s*(\\d+),\\s*(\\d+),\\s*([0-9.]+)\\)`));
  if (!m) throw new Error(`--${name} (rgba) not found in light-scrim block`);
  const [r, g, b, a] = [Number(m[1]), Number(m[2]), Number(m[3]), Number(m[4])];
  const mix = (fg: number, c: number) => Math.round(fg * a + c * (1 - a));
  const h = (n: number) => n.toString(16).padStart(2, "0");
  return `#${h(mix(r, bd[0]))}${h(mix(g, bd[1]))}${h(mix(b, bd[2]))}`;
}

describe("viewport pill light scrim (composited WCAG AA)", () => {
  it("off-icon + lock accent clear AA over the scrim composited on a dark render", () => {
    const block = lightScrimBlock();
    const surface = scrimCompositeOver(block, "vp-scrim-bg", [0, 0, 0]); // darkest triggering backdrop
    expect(contrast(scrimHex(block, "vp-icon-off"), surface)).toBeGreaterThanOrEqual(AA);
    expect(contrast(scrimHex(block, "vp-lock-accent"), surface)).toBeGreaterThanOrEqual(AA);
  });
});

// The DARK scrim now appears over backdrops up to bright (the raised flip
// threshold), so its glyphs must stay legible composited over WHITE (worst case)
// and NEUTRAL — not just over the dark backdrops #365 originally assumed.
function darkScrimBlock(): string {
  const blocks = [...componentsCss.matchAll(/\.vp-overlay\s*\{([^}]*)\}/g)];
  const m = blocks.find((b) => b[1]!.includes("--vp-scrim-bg"));
  if (!m) throw new Error(".vp-overlay base scrim block not found");
  return m[1]!;
}

describe("viewport pill dark scrim (composited WCAG AA over bright/neutral)", () => {
  const NON_TEXT = 3.0; // toggle icons are graphical objects (WCAG 1.4.11)
  it("keeps glyphs legible over a bright backdrop", () => {
    const block = darkScrimBlock();
    const surface = scrimCompositeOver(block, "vp-scrim-bg", [255, 255, 255]); // brightest backdrop
    expect(contrast(scrimHex(block, "vp-icon-off"), surface)).toBeGreaterThanOrEqual(NON_TEXT);
    expect(contrast(scrimHex(block, "vp-icon-on"), surface)).toBeGreaterThanOrEqual(AA);
    expect(contrast(scrimHex(block, "vp-lock-accent"), surface)).toBeGreaterThanOrEqual(AA);
  });
  it("keeps glyphs legible over a neutral backdrop", () => {
    const block = darkScrimBlock();
    const surface = scrimCompositeOver(block, "vp-scrim-bg", [128, 128, 128]);
    expect(contrast(scrimHex(block, "vp-icon-off"), surface)).toBeGreaterThanOrEqual(AA);
    expect(contrast(scrimHex(block, "vp-icon-on"), surface)).toBeGreaterThanOrEqual(AA);
  });
});
