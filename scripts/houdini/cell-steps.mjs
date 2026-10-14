// [houdini] How far a flipbook's picture jumps from one cell to the next.
//   node scripts/houdini/cell-steps.mjs --entry <cache entry>
//   node scripts/houdini/cell-steps.mjs --texture <file.tga|.dds> --grid <n> --blend additive|transparent [--pad <texels>]
//   node scripts/houdini/cell-steps.mjs --stock <path under Data, e.g. ART\TEXTURES\P_PARTICLE_EXPLOSION1.DDS> --grid <n> --blend ...
//
// A particle shows one cell at a time, with no blending between cells
// (src/simulation/EmitterInstance.cpp: floor of the index track), while its
// position, scale and colour move every frame. So a flipbook reads as
// stepped where consecutive cells differ a lot. This measures that, the same
// way for a generated sheet and the game's own:
//   - each cell's frame (the cell less its guard band) is averaged into a
//     grid of at most BINS x BINS, so sheets of any cell size compare fairly;
//   - a bin holds RGB for an additive sheet, premultiplied RGB and alpha for
//     a transparent one;
//   - step k is the mean absolute channel difference between cells k and
//     k + 1, in 8-bit levels, over the bins covered (brightest RGB, or alpha,
//     at least COVERED) in either cell.
// generate.mjs records it in provenance as `cellSteps`. Prints one
// `[houdini-cell-steps]` JSON line. Exit 0 ok, 2 bad input.
import * as fs from "node:fs";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { readStockFile, resolveReplayGameRoots } from "../replay-proof.mjs";
import { fail } from "./rules.mjs";
import { decodeDds, decodeTga } from "./images.mjs";
import { PAD } from "./profiles.mjs";

export const BINS = 64;
const COVERED = 4;
const round = (value) => Math.round(value * 100) / 100;

// `image` is a decoded TGA or DDS (top-down RGBA bytes).
export function cellSteps(image, { grid, blend, pad = 0 }) {
  if (![2, 4, 8, 16].includes(grid)) throw fail(`a flipbook grid is 2, 4, 8 or 16, not ${grid}`, 2);
  if (blend !== "additive" && blend !== "transparent") throw fail(`unknown blend ${blend}`, 2);
  if (image.width !== image.height || image.width % grid) throw fail(`a ${image.width}x${image.height} texture is no ${grid}x${grid} atlas`, 2);
  const cell = image.width / grid, frame = cell - 2 * pad, bins = Math.min(BINS, frame);
  if (frame < 1) throw fail(`a ${cell}-texel cell has no frame inside a ${pad}-texel guard band`, 2);
  const additive = blend === "additive", channels = additive ? 3 : 4;
  const binned = Array.from({ length: grid * grid }, (_, index) => {
    const sum = new Float64Array(bins * bins * 4), count = new Float64Array(bins * bins);
    const x0 = (index % grid) * cell + pad, y0 = Math.floor(index / grid) * cell + pad;
    for (let v = 0; v < frame; v++) {
      const row = Math.floor(v * bins / frame) * bins;
      for (let u = 0; u < frame; u++) {
        const o = ((y0 + v) * image.width + x0 + u) * 4, b = row + Math.floor(u * bins / frame), a = image.rgba[o + 3];
        for (let c = 0; c < 3; c++) sum[b * 4 + c] += additive ? image.rgba[o + c] : image.rgba[o + c] * a / 255;
        if (!additive) sum[b * 4 + 3] += a;
        count[b]++;
      }
    }
    for (let b = 0; b < bins * bins; b++) for (let c = 0; c < 4; c++) sum[b * 4 + c] /= count[b];
    return sum;
  });
  const covered = (bin, b) => (additive ? Math.max(bin[b * 4], bin[b * 4 + 1], bin[b * 4 + 2]) : bin[b * 4 + 3]) >= COVERED;
  const steps = [];
  for (let k = 0; k + 1 < binned.length; k++) {
    const [p, q] = [binned[k], binned[k + 1]];
    let total = 0, n = 0;
    for (let b = 0; b < bins * bins; b++) {
      if (!covered(p, b) && !covered(q, b)) continue;
      for (let c = 0; c < channels; c++) total += Math.abs(p[b * 4 + c] - q[b * 4 + c]);
      n += channels;
    }
    steps.push(round(n ? total / n : 0));
  }
  return { mean: round(steps.reduce((a, b) => a + b, 0) / steps.length), max: Math.max(...steps), steps };
}

const decode = (bytes, what) => {
  if (bytes.length >= 4 && bytes.readUInt32LE(0) === 0x20534444) {
    const image = decodeDds(bytes);
    if (!image.rgba) throw fail(`${what} is a ${image.fourCC ?? "uncompressed"} DDS; only DXT1 and DXT5 are read`, 2);
    return image;
  }
  const image = decodeTga(bytes);
  if (!image.rgba) throw fail(`${what} is not an uncompressed 32-bit TGA or a DXT1/DXT5 DDS`, 2);
  return image;
};

const USAGE = "usage: cell-steps.mjs --entry <cache entry> | --texture <file> --grid <n> --blend <b> [--pad <n>] | " +
  "--stock <path under Data> --grid <n> --blend <b>";
export function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!["--entry", "--texture", "--stock", "--grid", "--blend", "--pad"].includes(flag) || value === undefined ||
        value.startsWith("--") || flag.slice(2) in args) throw fail(USAGE, 2);
    args[flag.slice(2)] = value;
  }
  const sources = ["entry", "texture", "stock"].filter((name) => name in args);
  if (sources.length !== 1) throw fail(USAGE, 2);
  if (args.entry !== undefined && ["grid", "blend", "pad"].some((name) => name in args)) throw fail("--entry reads its grid, blend and guard band from provenance", 2);
  if (args.entry === undefined && !("grid" in args && "blend" in args)) throw fail("--grid and --blend are required", 2);
  if (args.stock !== undefined && "pad" in args) throw fail("the game's sheets have no guard band; --pad goes with --texture", 2);
  for (const name of ["grid", "pad"]) if (name in args) {
    if (!/^[0-9]{1,3}$/.test(args[name])) throw fail(`--${name} is not a whole number`, 2);
    args[name] = Number(args[name]);
  }
  // Backslash-separated names, none starting with a dot, so nothing climbs out of Data.
  if (args.stock !== undefined && !/^([A-Za-z0-9_-][A-Za-z0-9_.-]*\\)*[A-Za-z0-9_-][A-Za-z0-9_.-]*\.(dds|tga)$/i.test(args.stock))
    throw fail("--stock is a plain path under Data", 2);
  return args;
}

export function measure(args, { io = fs, roots = () => resolveReplayGameRoots({}), stockFile = readStockFile } = {}) {
  let image, shape, source;
  if (args.entry !== undefined) {
    const entry = resolve(args.entry);
    let provenance;
    try { provenance = JSON.parse(io.readFileSync(join(entry, "provenance.json"), "utf8")); }
    catch { throw fail(`not a cache entry: ${entry}`, 2); }
    const { name, grid, blend, pad } = provenance.texture ?? {};
    image = decode(io.readFileSync(join(entry, name)), name);
    shape = { grid, blend, pad: pad ?? PAD };
    source = name;
  } else if (args.texture !== undefined) {
    image = decode(io.readFileSync(resolve(args.texture)), args.texture);
    shape = { grid: args.grid, blend: args.blend, pad: args.pad ?? 0 };
    source = args.texture;
  } else {
    const bytes = stockFile(roots(), args.stock);
    if (!bytes) throw fail(`no ${args.stock} in the game data`, 2);
    image = decode(bytes, args.stock);
    shape = { grid: args.grid, blend: args.blend, pad: 0 };
    source = args.stock;
  }
  return { ok: true, exitCode: 0, source, ...shape, cell: image.width / shape.grid, ...cellSteps(image, shape) };
}

function main() {
  let record;
  try { record = measure(parseArgs(process.argv.slice(2))); }
  catch (error) { record = { ok: false, exitCode: error.exitCode ?? 1, error: error.message }; }
  console.log(`[houdini-cell-steps] ${JSON.stringify(record)}`);
  process.exitCode = record.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) main();
