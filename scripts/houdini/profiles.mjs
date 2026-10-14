// [houdini] Output profiles: pack the frames into one texture, encode it for
// its blend mode, and check the result (plan §3.4, §3.5). No aesthetics.
//
// Layout: the texture is grid x cell texels on a side. Frame i sits in column
// i % grid, row i / grid from the top-left, inset by PAD texels, so every cell
// keeps a guard band of the profile's border value: the particle pass samples
// mip 0 only, with bilinear filtering, so 2 texels hold neighbours apart.
// A single texture (grid 1) is laid out the same way, so its border is
// guaranteed too.
//
// Encoding. Frames arrive premultiplied, as renderers write them, and this
// build's TGA writer divides colour by alpha on the way out. So the colour
// handed to the writer stays premultiplied wherever alpha > 0:
//   additive    - RGB as rendered (ONE/ONE adds it on black), alpha 255, as the
//                 dedicated vanilla additive flipbooks are (step-0 correction 5).
//   transparent - premultiplied colour where covered, and where alpha is 0 a
//                 colour pulled in from the coverage (--fillholes), so bilinear
//                 filtering at an edge blends towards the sprite's own colour,
//                 never towards black. Straight alpha, as PrimAlpha blends it.
//                 "Covered" is decided on the alpha the file will hold: alpha
//                 below COVERED is zeroed first (8-bit rounding would zero it
//                 anyway), so no texel ends up alpha 0 with its own near-black
//                 premultiplied colour instead of the fill.
// Every call runs with --evaloff --wildcardoff, and the TGA is written with
// its attributes erased and no compression, so equal pixels give equal bytes.
export const PAD = 2;
export const BLENDS = ["additive", "transparent"];
export const MAX_TEXTURE_BYTES = 64 * 1024 * 1024;   // src/common/ResourceLimits.h
// An alpha-0 texel beside coverage is a dark fringe when its colour is under
// FRINGE_RATIO of its covered neighbours' (alpha-weighted) colour, unless that
// colour is itself near black (FRINGE_FLOOR), where a fringe can't show.
// Only neighbours at FRINGE_MIN_ALPHA or more count: the file stores colour to
// one premultiplied step, 255 / alpha, so a faint texel's colour is noise (at
// alpha 2 a grey of 60 reads 127), and filtering takes almost nothing from it.
const FRINGE_RATIO = 0.5, FRINGE_FLOOR = 16, FRINGE_MIN_ALPHA = 16;
// 0.51 / 255: the least alpha that rounds to 1 in the 8-bit file, with margin.
const COVERED = "0.002";

const fail = (message, exitCode = 4) => Object.assign(new Error(message), { exitCode });

export const profileName = (blend, grid) => (grid > 1 ? "eaw-flipbook" : `eaw-${blend}`);

// Names are content-addressed: equal pixels, equal name; never two files for one name.
export const textureName = (id, textureSha256) => `pe_gen_${id}_${textureSha256.slice(0, 8)}.tga`;

export function atlasLayout(grid, cell, pad = PAD) {
  const frame = cell - 2 * pad;
  if (frame < 1) throw fail(`a ${cell}-texel cell has no room inside a ${pad}-texel guard band`, 1);
  const cells = Array.from({ length: grid * grid }, (_, index) => {
    const x = (index % grid) * cell, y = Math.floor(index / grid) * cell;
    return { index, x, y, frameX: x + pad, frameY: y + pad };
  });
  return { grid, cell, pad, frame, size: grid * cell, cells };
}

const SAFE = ["--evaloff", "--wildcardoff"];
const WRITE = ["--clamp:min=0:max=1", "--nosoftwareattrib", "--eraseattrib", ".*", "-d", "uint8", "--compression", "none", "-o"];

// hoiiotool needs the canvas on top of the stack to paste onto it (step-0
// correction 1): push the frame, swap, paste.
export function packArgs(frames, layout, out) {
  if (frames.length !== layout.cells.length) throw fail(`${frames.length} frames for ${layout.cells.length} cells`, 1);
  const args = [...SAFE, "--pattern", "constant:color=0,0,0,0", `${layout.size}x${layout.size}`, "4"];
  layout.cells.forEach((cell, i) => args.push(frames[i], "--swap", "--paste", `+${cell.frameX}+${cell.frameY}`));
  return [...args, "-d", "float", "-o", out];
}

export function encodeArgs(blend, packed, out) {
  if (blend === "additive") return [...SAFE, packed, "--ch", "R,G,B,A=1.0", ...WRITE, out];
  if (blend !== "transparent") throw fail(`unknown blend ${blend}`, 1);
  const coverage = [packed, "--ch", "A", "--subc", COVERED, "--mulc", "1e9", "--clamp:min=0:max=1"];
  return [...SAFE,
    // (premultiplied colour, coverage 0 or 1) ...
    packed, "--ch", "R,G,B", ...coverage, "--chappend",
    // ... over (the coverage's colour pulled into the holes, alpha 0) ...
    packed, "--fillholes", "--ch", "R,G,B,A=0.0", "--over",
    // ... then the alpha back, zeroed wherever it isn't covered.
    "--ch", "R,G,B", packed, "--ch", "A", ...coverage, "--mul", "--chappend", ...WRITE, out];
}

// One `hoiiotool --stats` over every frame: size, channels and non-finite counts.
export const statsArgs = (frames) => [...SAFE, "--stats", ...frames];
export function parseFrameStats(stdout) {
  const frames = [];
  for (const line of stdout.split(/\r?\n/)) {
    const head = / : +(\d+) x +(\d+), (\d+) channel/.exec(line);
    if (head) { frames.push({ width: Number(head[1]), height: Number(head[2]), channels: Number(head[3]), nan: null, inf: null }); continue; }
    const count = /^\s+Stats (NanCount|InfCount):((?: \d+)+)/.exec(line);
    if (count && frames.length) frames.at(-1)[count[1] === "NanCount" ? "nan" : "inf"] = count[2].trim().split(/ +/).map(Number).reduce((a, b) => a + b, 0);
  }
  return frames;
}

export function checkFrames(stats, layout) {
  const errors = [];
  if (stats.length !== layout.cells.length) return [`hoiiotool reported ${stats.length} frames, not ${layout.cells.length}`];
  stats.forEach((s, i) => {
    if (s.width !== layout.frame || s.height !== layout.frame) errors.push(`frame ${i} is ${s.width}x${s.height}, not ${layout.frame}x${layout.frame}`);
    if (s.channels !== 4) errors.push(`frame ${i} has ${s.channels} channels, not RGBA`);
    if (s.nan !== 0 || s.inf !== 0) errors.push(`frame ${i} has ${s.nan ?? "?"} NaN and ${s.inf ?? "?"} Inf values`);
  });
  return errors;
}

// The validators (§3.5) over the written texture. `image` is decodeTga()'s result.
export function validateTexture(image, bytes, { blend, layout }) {
  const errors = [];
  const { size, cell, pad, grid } = layout;
  if (image.type !== 2 || image.bpp !== 32 || image.alphaBits !== 8 || !image.rgba)
    return [`texture is not an uncompressed 32-bit TGA with 8 alpha bits (type ${image.type}, ${image.bpp} bpp, ${image.alphaBits} alpha bits)`];
  if (image.width !== size || image.height !== size) errors.push(`texture is ${image.width}x${image.height}, not ${size}x${size}`);
  if (size & (size - 1)) errors.push(`texture side ${size} is not a power of two`);
  if (bytes > MAX_TEXTURE_BYTES) errors.push(`texture is ${bytes} bytes, over the editor's 64 MiB limit`);
  if (errors.length) return errors;

  const at = (x, y) => (y * size + x) * 4;
  const px = image.rgba;
  const inBand = (x, y) => { const u = x % cell, v = y % cell; return u < pad || v < pad || u >= cell - pad || v >= cell - pad; };
  let bandBad = 0, opaqueBad = 0;
  const filled = new Array(grid * grid).fill(false);
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const o = at(x, y), maxRgb = Math.max(px[o], px[o + 1], px[o + 2]), alpha = px[o + 3];
      if (blend === "additive" && alpha !== 255) opaqueBad++;
      if (inBand(x, y)) {
        if (blend === "additive" ? maxRgb > 1 : alpha !== 0) bandBad++;
      } else if (blend === "additive" ? maxRgb > 1 : alpha > 0) {
        filled[Math.floor(y / cell) * grid + Math.floor(x / cell)] = true;
      }
    }
  }
  if (opaqueBad) errors.push(`${opaqueBad} texels have alpha below 255; additive textures are opaque`);
  if (bandBad) errors.push(`${bandBad} guard-band texels are not the ${blend === "additive" ? "black" : "fully transparent"} border`);
  const empty = filled.flatMap((v, i) => (v ? [] : [i]));
  if (empty.length) errors.push(`cells ${empty.join(", ")} are empty`);

  // Transparent: an alpha-0 texel beside coverage must carry the coverage's
  // colour, or filtering darkens the sprite's edge.
  if (blend === "transparent") {
    let fringe = 0;
    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const o = at(x, y);
        if (px[o + 3] !== 0) continue;
        let weight = 0;
        const sum = [0, 0, 0];
        for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
          const nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= size || ny >= size) continue;
          const n = at(nx, ny), a = px[n + 3];
          if (a < FRINGE_MIN_ALPHA) continue;
          weight += a; for (let c = 0; c < 3; c++) sum[c] += px[n + c] * a;
        }
        const around = weight ? Math.max(...sum.map((v) => v / weight)) : 0;
        if (around > FRINGE_FLOOR && Math.max(px[o], px[o + 1], px[o + 2]) < around * FRINGE_RATIO) fringe++;
      }
    }
    if (fringe) errors.push(`${fringe} transparent texels beside coverage are much darker than it: a dark fringe under filtering`);
  }
  return errors;
}
