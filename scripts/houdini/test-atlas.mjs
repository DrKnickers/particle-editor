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

// A DXT1 or DXT5 DDS of a top-down RGBA buffer, as texconv lays it out: each
// block's darkest and brightest texels as the colour ends (four-colour mode),
// its alpha range as the alpha ends (eight-level mode), every texel to its
// nearest. Lower mips repeat mip 0's corner (they are never decoded); `mips`
// sets how many levels the header claims.
export function encodeDds(width, height, rgba, fourCC = "DXT5", { mips = Math.log2(Math.max(width, height)) + 1 } = {}) {
  const levels = [];
  for (let level = 0; level < mips; level++) {
    const w = Math.max(1, width >> level), h = Math.max(1, height >> level);
    for (let by = 0; by < h; by += 4) for (let bx = 0; bx < w; bx += 4) levels.push(block(bx, by));
  }
  const header = Buffer.alloc(128);
  header.write("DDS ", 0, "latin1");
  header.writeUInt32LE(124, 4); header.writeUInt32LE(0x000a1007, 8);
  header.writeUInt32LE(height, 12); header.writeUInt32LE(width, 16); header.writeUInt32LE(mips, 28);
  header.writeUInt32LE(32, 76); header.writeUInt32LE(4, 80); header.write(fourCC, 84, "latin1");
  header.writeUInt32LE(0x401008, 108);
  return Buffer.concat([header, ...levels]);

  function block(bx, by) {
    const texels = [];
    for (let t = 0; t < 16; t++) {
      const x = Math.min(width - 1, bx + (t & 3)), y = Math.min(height - 1, by + (t >> 2)), o = (y * width + x) * 4;
      texels.push([rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3]]);
    }
    const pack = ([r, g, b]) => (Math.round(r * 31 / 255) << 11) | (Math.round(g * 63 / 255) << 5) | Math.round(b * 31 / 255);
    const unpack = (v) => [((v >> 11) & 31) * 255 / 31, ((v >> 5) & 63) * 255 / 63, (v & 31) * 255 / 31];
    const light = (c) => c[0] + c[1] + c[2];
    let c0 = pack(texels.reduce((a, c) => (light(c) > light(a) ? c : a))), c1 = pack(texels.reduce((a, c) => (light(c) < light(a) ? c : a)));
    // DXT1 with a texel under half alpha: three colours and transparent black.
    const holes = fourCC === "DXT1" && texels.some((c) => c[3] < 128);
    if (holes ? c0 > c1 : c0 < c1) [c0, c1] = [c1, c0];
    const e = [unpack(c0), unpack(c1)];
    const palette = holes ? [e[0], e[1], e[0].map((v, k) => (v + e[1][k]) / 2)]
      : c0 === c1 ? [e[0], e[0], e[0], e[0]]
        : [e[0], e[1], e[0].map((v, k) => (2 * v + e[1][k]) / 3), e[0].map((v, k) => (v + 2 * e[1][k]) / 3)];
    const nearest = (list, distance) => list.reduce((best, _, i) => (distance(i) < distance(best) ? i : best), 0);
    let indices = 0;
    texels.forEach((c, t) => {
      const index = holes && c[3] < 128 ? 3 : nearest(palette, (i) => palette[i].reduce((s, v, k) => s + (v - c[k]) ** 2, 0));
      indices |= index << (2 * t);
    });
    const colour = Buffer.alloc(8);
    colour.writeUInt16LE(c0, 0); colour.writeUInt16LE(c1, 2); colour.writeUInt32LE(indices >>> 0, 4);
    if (fourCC === "DXT1") return colour;
    const a0 = Math.max(...texels.map((c) => c[3])), a1 = Math.min(...texels.map((c) => c[3]));
    const alphas = a0 === a1 ? [a0] : [a0, a1, ...[1, 2, 3, 4, 5, 6].map((i) => ((7 - i) * a0 + i * a1) / 7)];
    let bits = 0n;
    texels.forEach((c, t) => { bits |= BigInt(nearest(alphas, (i) => Math.abs(alphas[i] - c[3]))) << BigInt(3 * t); });
    const alpha = Buffer.alloc(8);
    alpha[0] = a0; alpha[1] = a0 === a1 ? a0 : a1;
    for (let i = 0; i < 6; i++) alpha[2 + i] = Number((bits >> BigInt(8 * i)) & 0xffn);
    return Buffer.concat([alpha, colour]);
  }
}

// What `hoiiotool --stats` prints for each frame.
export const frameStatsText = (count, size, { channels = 4, nan = 0 } = {}) => Array.from({ length: count }, (_, i) =>
  `frames/frame_${String(i + 1).padStart(4, "0")}.exr : ${String(size).padStart(4)} x ${String(size).padStart(4)}, ${channels} channel, half openexr\n` +
  `    Stats Min: 0.000000 0.000000 0.500000 1.000000 (float)\n` +
  `    Stats NanCount: ${nan} 0 0 0 \n    Stats InfCount: 0 0 0 0 \n    Stats FiniteCount: 784 784 784 784 \n`).join("");
