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

// COLORREF ↔ Vec4 helpers.
//
// The engine's lights / ambient / shadow take linear-space floating-point
// colour in `Vec4 = [r, g, b, a]` with each channel in [0, 1]. The Win32
// UI persists those same colours as COLORREF 0xBBGGRR ints. These helpers
// bridge the two representations so the ColorButton (which speaks
// COLORREF via `hexToColorref` / `colorrefToHex`) can drive lighting
// controls that send Vec4 to the bridge.
//
// Alpha defaults to 1.0 because every existing call site treats colour
// alpha as opaque (see `MakeLight` at src/main.cpp:6196 which hard-codes
// the diffuse Vec4's w to 1.0f). A separate `intensity` multiplier is
// folded in by the lighting panel — it scales the RGB on the way to the
// bridge, mirroring legacy behaviour (R/255 * intensity, etc).

import type { Color, Vec4 } from "@particle-editor/bridge-schema";

export function colorrefToVec4(c: Color): Vec4 {
  const r = (c & 0xff) / 255;
  const g = ((c >> 8) & 0xff) / 255;
  const b = ((c >> 16) & 0xff) / 255;
  return [r, g, b, 1.0] as const;
}

export function vec4ToColorref(v: Vec4): Color {
  const clamp = (x: number) => Math.max(0, Math.min(255, Math.round(x * 255)));
  const r = clamp(v[0]);
  const g = clamp(v[1]);
  const b = clamp(v[2]);
  return (b << 16) | (g << 8) | r;
}
