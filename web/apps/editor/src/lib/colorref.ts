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

