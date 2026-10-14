// [houdini] Test helper: a texture that passes its profile, built from the
// layout, so a test can damage one property at a time. Not used at runtime.
import { atlasLayout } from "./profiles.mjs";
import { encodeTga } from "./images.mjs";

// Additive: black guard bands, opaque, each cell a distinct colour.
// Transparent: each cell a soft disc of one colour, that colour bled into
// every alpha-0 texel (bands included), as the encode leaves it.
export function syntheticAtlas({ grid = 4, cell = 32, blend = "additive" } = {}) {
  const layout = atlasLayout(grid, cell);
  const { size, pad } = layout;
  const rgba = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const o = (y * size + x) * 4, index = Math.floor(y / cell) * grid + Math.floor(x / cell);
      const u = x % cell, v = y % cell, band = u < pad || v < pad || u >= cell - pad || v >= cell - pad;
      const colour = [40 + index * 12, 200 - index * 8, 120];
      if (blend === "additive") {
        rgba.set(band ? [0, 0, 0, 255] : [...colour, 255], o);
      } else {
        const r = Math.hypot(u - cell / 2 + 0.5, v - cell / 2 + 0.5) / (cell / 2 - pad);
        const alpha = band ? 0 : Math.round(255 * Math.min(1, Math.max(0, (1 - r) * 3)));
        rgba.set([...colour, alpha], o);
      }
    }
  }
  return { layout, rgba, tga: () => encodeTga(size, size, rgba) };
}

// What `hoiiotool --stats` prints for each frame.
export const frameStatsText = (count, size, { channels = 4, nan = 0 } = {}) => Array.from({ length: count }, (_, i) =>
  `frames/frame_${String(i + 1).padStart(4, "0")}.exr : ${String(size).padStart(4)} x ${String(size).padStart(4)}, ${channels} channel, half openexr\n` +
  `    Stats Min: 0.000000 0.000000 0.500000 1.000000 (float)\n` +
  `    Stats NanCount: ${nan} 0 0 0 \n    Stats InfCount: 0 0 0 0 \n    Stats FiniteCount: 784 784 784 784 \n`).join("");
