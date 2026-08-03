// COLORREF ↔ hex helpers.
//
// Win32 COLORREF byte order: low byte = R, then G, then B
// (i.e. `RGB(r,g,b) = r | (g<<8) | (b<<16)`). The high byte is reserved
// and stays zero. Mirrors the engine's `Color` typedef from
// `bridge-schema/src/index.ts`.

export function colorrefToHex(c: number): string {
  const r = c & 0xff;
  const g = (c >> 8) & 0xff;
  const b = (c >> 16) & 0xff;
  return `#${r.toString(16).padStart(2, "0")}${g.toString(16).padStart(2, "0")}${b.toString(16).padStart(2, "0")}`;
}

export function hexToColorref(hex: string): number {
  const m = hex.replace("#", "");
  const r = parseInt(m.slice(0, 2), 16);
  const g = parseInt(m.slice(2, 4), 16);
  const b = parseInt(m.slice(4, 6), 16);
  return (b << 16) | (g << 8) | r;
}

/** Extract R/G/B from a Win32 COLORREF (low byte = R), matching the hex helpers
 *  above and the `Color` type in bridge-schema. */
export function colorrefToRgb(c: number): { r: number; g: number; b: number } {
  return { r: c & 0xff, g: (c >> 8) & 0xff, b: (c >> 16) & 0xff };
}

/** Relative-luminance flip point: backdrops BELOW it get the LIGHT scrim, at/above
 *  it the DARK scrim. Set near mid-grey (0x808080 ≈ 0.216) so the pill flips on
 *  neutral backgrounds — dark/neutral scenes get a light chip (pops on the dark
 *  render), bright scenes a dark chip. Single threshold, theme-independent
 *  (2026-06-24 live-review decision: "always flip by backdrop"). One number to
 *  tune. e.g. neutral grey 0x6E6E6E (0.156) → light; mid-grey 0x808080 → dark. */
export const PILL_DARK_THRESHOLD = 0.2;

function srgbRelativeLuminance(r: number, g: number, b: number): number {
  const lin = (c: number) => {
    const s = c / 255;
    return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

/** The colour actually behind the bottom-left pill: the effective GROUND colour
 *  when the ground plane is shown (the floor sits under the pill), else the
 *  viewport background. `groundColor` is host-computed — the solid colour for the
 *  solid slot, the loaded texture's average for a textured floor — so the pill
 *  adapts to any floor, including custom textures. */
export function pillBackdropColor(state: {
  ground: boolean;
  groundColor: number;
  background: number;
}): number {
  return state.ground ? state.groundColor : state.background;
}

/** Pick the pill scrim from the effective BACKDROP colour (resolve it first with
 *  pillBackdropColor — the ground colour when the floor is shown, else the
 *  viewport background): a LIGHT chip over a dark/neutral backdrop (it pops on the
 *  render), a DARK chip over a bright one. */
export function pillScrimMode(backdrop: number): "light" | "dark" {
  const { r, g, b } = colorrefToRgb(backdrop);
  return srgbRelativeLuminance(r, g, b) < PILL_DARK_THRESHOLD ? "light" : "dark";
}

