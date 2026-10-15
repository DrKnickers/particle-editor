// [houdini] Whether three cache entries make one core triplet of the layered
// explosion: an early smoke, a late smoke and a fire rendered from one sim.
//   node scripts/houdini/align.mjs --early <cache entry> --late <cache entry> --fire <cache entry>
//
// The three play together on one particle each (tasks/2026-10-14-layered-explosion-design.md,
// "The pair rule"), so they must agree:
//   - one simulation: the same seed, parameters, tier, threads, Houdini build
//     and explosion_sim module;
//   - the early smoke and the fire show the same sim time and span in every
//     cell, and the fire sits inside the smoke, cell by cell (a fire bin lit
//     where the smoke is clear would glow in the air);
//   - the late smoke picks up after the early one: later times, and spans that
//     carry on from the early sheet's last without shrinking;
//   - each sheet's spans never shrink.
// Prints one `[houdini-align]` JSON line. Exit 0 aligned, 3 not aligned, 2 bad input.
import * as fs from "node:fs";
import { join, resolve } from "node:path";
import { pathToFileURL } from "node:url";
import { canonical } from "./cache.mjs";
import { fail } from "./rules.mjs";
import { binCells, coverage } from "./cell-steps.mjs";
import { decodeDds, decodeTga } from "./images.mjs";

// A fire bin counts as lit at this brightest RGB, a smoke bin as present at
// this alpha; at most OUTSIDE_MAX of a cell's lit fire bins may lie where the
// smoke is clear. Measured on a preview triplet: the flame front leads the
// smoke by up to 2.5 % of the fire's bins in the first frames, about 1 %
// after; the same fire against the late smoke, or 16 cells out of step, put
// 49 % and 27 % outside.
const FIRE_LIT = 16, SMOKE_PRESENT = 4, OUTSIDE_MAX = 0.05;
const ROLES = ["early", "late", "fire"];

export function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!ROLES.map((r) => `--${r}`).includes(flag) || value === undefined || value.startsWith("--") || flag.slice(2) in args)
      throw fail("usage: align.mjs --early <cache entry> --late <cache entry> --fire <cache entry>", 2);
    args[flag.slice(2)] = value;
  }
  if (ROLES.some((role) => !(role in args))) throw fail("usage: align.mjs --early <cache entry> --late <cache entry> --fire <cache entry>", 2);
  return args;
}

function load(dir, role, io) {
  let provenance;
  try { provenance = JSON.parse(io.readFileSync(join(dir, "provenance.json"), "utf8")); }
  catch { throw fail(`--${role} is not a cache entry: ${dir}`, 2); }
  const { cellSeconds, cellSpan, texture } = provenance;
  if (!Array.isArray(cellSeconds) || !Array.isArray(cellSpan) || cellSpan.length !== cellSeconds.length || !texture)
    throw fail(`--${role} records no cell times and spans; it is not an explosion sheet`, 2);
  const want = role === "fire" ? "additive" : "transparent";
  if (texture.blend !== want) throw fail(`--${role} is ${texture.blend}, not ${want}`, 2);
  return { provenance, dir };
}

function decode(entry, io) {
  const { name } = entry.provenance.texture;
  const bytes = io.readFileSync(join(entry.dir, name));
  const image = /\.dds$/i.test(name) ? decodeDds(bytes) : decodeTga(bytes);
  if (!image.rgba) throw fail(`${name} cannot be read`, 2);
  return image;
}

const rising = (values) => values.every((v, i) => i === 0 || v >= values[i - 1]);
const same = (a, b) => canonical(a) === canonical(b);

export function align(args, { io = fs } = {}) {
  const [early, late, fire] = ROLES.map((role) => load(resolve(args[role]), role, io));
  const p = { early: early.provenance, late: late.provenance, fire: fire.provenance };
  const checks = [];
  const check = (name, ok, detail = undefined) => checks.push({ name, ok: Boolean(ok), ...(detail === undefined ? {} : { detail }) });

  for (const field of ["seed", "parameters", "tier", "threads", "houdiniBuild", "modules"])
    check(`one simulation: the same ${field}`, same(p.early[field], p.late[field]) && same(p.early[field], p.fire[field]));
  check("the early smoke and the fire show the same sim time in every cell", same(p.early.cellSeconds, p.fire.cellSeconds));
  check("the early smoke and the fire are framed to the same span in every cell", same(p.early.cellSpan, p.fire.cellSpan));
  check("the late smoke's cells follow the early smoke's", p.late.cellSeconds[0] > p.early.cellSeconds.at(-1));
  check("the late smoke's spans carry on without shrinking", p.late.cellSpan[0] >= p.early.cellSpan.at(-1));
  for (const role of ROLES) check(`the ${role} sheet's spans never shrink`, rising(p[role].cellSpan));

  const shape = (entry) => ({ grid: entry.provenance.texture.grid, pad: entry.provenance.texture.pad });
  let worst = null;
  if (same(shape(early), shape(fire)) && early.provenance.texture.size === fire.provenance.texture.size) {
    const smoke = binCells(decode(early, io), { ...shape(early), blend: "transparent" });
    const flame = binCells(decode(fire, io), { ...shape(fire), blend: "additive" });
    worst = { cell: 0, outside: 0 };
    flame.cells.forEach((cell, k) => {
      let lit = 0, outside = 0;
      for (let b = 0; b < flame.bins * flame.bins; b++) {
        if (coverage(cell, b, "additive") < FIRE_LIT) continue;
        lit++;
        if (coverage(smoke.cells[k], b, "transparent") < SMOKE_PRESENT) outside++;
      }
      const fraction = lit ? outside / lit : 0;
      if (fraction > worst.outside) worst = { cell: k, outside: Math.round(fraction * 10000) / 10000 };
    });
    check(`the fire sits inside the early smoke in every cell (at most ${OUTSIDE_MAX} of its lit bins outside)`,
      worst.outside <= OUTSIDE_MAX, worst);
  } else check("the early smoke and the fire are the same atlas shape", false);

  const ok = checks.every((c) => c.ok);
  return { ok, exitCode: ok ? 0 : 3, checks };
}

function main() {
  let record;
  try { record = align(parseArgs(process.argv.slice(2))); }
  catch (error) { record = { ok: false, exitCode: error.exitCode ?? 1, error: error.message }; }
  console.log(`[houdini-align] ${JSON.stringify(record)}`);
  process.exitCode = record.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) main();
