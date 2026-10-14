// [houdini] The cell-step measure: exact on flat cells, blind to the guard
// band and to empty space, fair across cell sizes, and the same through a
// TGA, a DDS, a cache entry and the game's data.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { BINS, cellSteps, measure, parseArgs } from "./cell-steps.mjs";
import { decodeTga, encodeTga } from "./images.mjs";
import { encodeDds } from "./test-atlas.mjs";

// A grid x grid sheet whose cell i is `colour(i)` over its frame, with
// `band` in the guard band and `empty` outside a centred square of `fill`
// of the frame.
function sheet({ grid = 4, cell = 32, pad = 2, colour, band = [0, 0, 0, 0], empty = null, fill = 1 }) {
  const size = grid * cell, rgba = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) for (let x = 0; x < size; x++) {
    const i = Math.floor(y / cell) * grid + Math.floor(x / cell), u = x % cell - pad, v = y % cell - pad, frame = cell - 2 * pad;
    const inFrame = u >= 0 && v >= 0 && u < frame && v < frame;
    const lo = frame * (1 - fill) / 2, inFill = u >= lo && v >= lo && u < frame - lo && v < frame - lo;
    rgba.set(!inFrame ? band : inFill || !empty ? colour(i) : empty, (y * size + x) * 4);
  }
  return { width: size, height: size, rgba };
}

test("flat additive cells step by their mean RGB difference; the guard band is not measured", () => {
  const image = sheet({ colour: (i) => [(i % 4) * 85, Math.floor(i / 4) * 85, 128, 255], band: [255, 255, 255, 255] });
  const { steps, mean, max } = cellSteps(image, { grid: 4, blend: "additive", pad: 2 });
  // Along a row red rises 85; at a row's end red falls 255 and green rises 85.
  const along = 85 / 3, wrap = (255 + 85) / 3;
  assert.deepEqual(steps, [along, along, along, wrap, along, along, along, wrap, along, along, along, wrap, along, along, along]
    .map((v) => Math.round(v * 100) / 100));
  assert.equal(max, Math.round(wrap * 100) / 100);
  assert.ok(Math.abs(mean - (12 * along + 3 * wrap) / 15) < 0.01);
  // Read with no guard band, the white band counts and the steps change.
  assert.notDeepEqual(cellSteps(image, { grid: 4, blend: "additive", pad: 0 }).steps, steps);
});

test("transparent cells compare premultiplied colour and alpha", () => {
  // Every cell white; alpha alternates 255 and 51: premultiplied RGB moves 204 and alpha 204.
  const image = sheet({ colour: (i) => [255, 255, 255, i % 2 ? 51 : 255] });
  assert.ok(cellSteps(image, { grid: 4, blend: "transparent", pad: 2 }).steps.every((v) => Math.abs(v - 204) <= 0.01));
  // The colour under alpha 0 is not seen, so it is not measured.
  const hidden = sheet({ colour: (i) => [i * 16, 0, 0, 0] });
  assert.deepEqual(cellSteps(hidden, { grid: 4, blend: "transparent", pad: 2 }).steps, Array(15).fill(0));
});

test("only covered space counts, so a small sprite steps as much as a big one", () => {
  const colour = (i) => [(i % 2) * 200, 0, 0, 255];
  const full = cellSteps(sheet({ colour }), { grid: 4, blend: "additive", pad: 2 });
  const small = cellSteps(sheet({ cell: 128, colour, empty: [0, 0, 0, 255], fill: 0.5 }), { grid: 4, blend: "additive", pad: 2 });
  // Odd cells are black, so only the even cells' square is covered in either.
  assert.ok(full.steps.every((v) => Math.abs(v - 200 / 3) < 0.01));
  assert.ok(small.steps.every((v) => Math.abs(v - 200 / 3) < 1), JSON.stringify(small.steps));
});

test("cells are averaged to at most BINS a side, so the cell size doesn't change the measure", () => {
  const colour = (i) => [(i * 37) % 256, (i * 91) % 256, 60, 255];
  const at = (cell) => cellSteps(sheet({ cell, colour }), { grid: 4, blend: "additive", pad: 2 }).steps;
  assert.equal(BINS, 64);
  assert.deepEqual(at(32), at(256));
  assert.throws(() => cellSteps(sheet({ colour }), { grid: 3, blend: "additive" }), (e) => e.exitCode === 2);
  assert.throws(() => cellSteps(sheet({ colour }), { grid: 4, blend: "multiply" }), (e) => e.exitCode === 2);
  assert.throws(() => cellSteps(sheet({ colour }), { grid: 4, blend: "additive", pad: 16 }), /no frame/);
});

test("a TGA, a DDS, a cache entry and the game's data measure alike", (t) => {
  const dir = mkdtempSync(join(tmpdir(), "pe-cell-steps-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const image = sheet({ colour: (i) => [(i % 4) * 85, Math.floor(i / 4) * 85, 128, 255] });
  const tga = encodeTga(image.width, image.height, image.rgba), dds = encodeDds(image.width, image.height, image.rgba, "DXT1");
  writeFileSync(join(dir, "sheet.tga"), tga);
  writeFileSync(join(dir, "sheet.dds"), dds);
  const exact = cellSteps(decodeTga(tga), { grid: 4, blend: "additive", pad: 2 });
  const fromTga = measure({ texture: join(dir, "sheet.tga"), grid: 4, blend: "additive", pad: 2 });
  assert.deepEqual([fromTga.ok, fromTga.steps, fromTga.cell], [true, exact.steps, 32]);
  const fromDds = measure({ texture: join(dir, "sheet.dds"), grid: 4, blend: "additive", pad: 2 });
  assert.ok(Math.abs(fromDds.mean - exact.mean) < 2, `${fromDds.mean} vs ${exact.mean}`);
  const entry = join(dir, "entry");
  mkdirSync(entry);
  writeFileSync(join(entry, "pe_gen_x_00000000.tga"), tga);
  writeFileSync(join(entry, "provenance.json"), JSON.stringify({ texture: { name: "pe_gen_x_00000000.tga", grid: 4, blend: "additive", pad: 2 } }));
  assert.deepEqual(measure({ entry }).steps, exact.steps);
  const stock = measure({ stock: "ART\\TEXTURES\\X.DDS", grid: 4, blend: "additive" },
    { roots: () => ["C:\\game"], stockFile: (roots, path) => (path === "ART\\TEXTURES\\X.DDS" ? dds : null) });
  assert.deepEqual([stock.source, stock.pad], ["ART\\TEXTURES\\X.DDS", 0]);
  assert.throws(() => measure({ stock: "ART\\TEXTURES\\NONE.DDS", grid: 4, blend: "additive" }, { roots: () => [], stockFile: () => null }),
    /no ART\\TEXTURES\\NONE.DDS/);
  writeFileSync(join(dir, "dxt3.dds"), Buffer.concat([dds.subarray(0, 84), Buffer.from("DXT3"), dds.subarray(88)]));
  assert.throws(() => measure({ texture: join(dir, "dxt3.dds"), grid: 4, blend: "additive" }), /DXT3 DDS/);
});

test("parseArgs takes exactly one source, and the shape only where provenance can't give it", () => {
  assert.deepEqual(parseArgs(["--texture", "a.tga", "--grid", "8", "--blend", "transparent", "--pad", "2"]),
    { texture: "a.tga", grid: 8, blend: "transparent", pad: 2 });
  assert.deepEqual(parseArgs(["--entry", "e"]), { entry: "e" });
  for (const bad of [[], ["--entry", "e", "--texture", "t"], ["--entry", "e", "--grid", "4"], ["--texture", "t", "--grid", "4"],
    ["--stock", "A.DDS", "--grid", "4", "--blend", "additive", "--pad", "2"], ["--stock", "..\\..\\x.dds", "--grid", "4", "--blend", "additive"],
    ["--texture", "t", "--grid", "four", "--blend", "additive"], ["--texture", "t", "--texture", "u"], ["--bogus", "1"]])
    assert.throws(() => parseArgs(bad), (e) => e.exitCode === 2, JSON.stringify(bad));
});
