// [houdini] The triplet check: one sim, matched times and spans, the late
// sheet following the early one, and the fire inside the smoke.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { align, parseArgs } from "./align.mjs";
import { encodeTga } from "./images.mjs";

const GRID = 2, CELL = 32, PAD = 2, SIZE = GRID * CELL;

// A 2x2 sheet: in every cell a centred disc of `radius` texels, coloured `inside`, `outside` beyond it.
function sheet(inside, outside, radius = () => 10) {
  const rgba = Buffer.alloc(SIZE * SIZE * 4);
  for (let y = 0; y < SIZE; y++) for (let x = 0; x < SIZE; x++) {
    const k = Math.floor(y / CELL) * GRID + Math.floor(x / CELL), dx = x % CELL - CELL / 2, dy = y % CELL - CELL / 2;
    rgba.set(dx * dx + dy * dy < radius(k) ** 2 ? inside : outside, (y * SIZE + x) * 4);
  }
  return encodeTga(SIZE, SIZE, rgba);
}

function entry(root, name, { blend, cellSeconds, cellSpan, texture, ...rest }) {
  const dir = join(root, name);
  mkdirSync(dir, { recursive: true });
  writeFileSync(join(dir, `${name}.tga`), texture);
  writeFileSync(join(dir, "provenance.json"), JSON.stringify({
    seed: 3, parameters: { blast: 2, turbulence: 0.6 }, tier: "preview", threads: 4, houdiniBuild: "19.5.435",
    modules: { explosion_sim: "a".repeat(64) }, cellSeconds, cellSpan,
    texture: { name: `${name}.tga`, format: "tga", blend, grid: GRID, cell: CELL, pad: PAD, size: SIZE }, ...rest,
  }));
  return dir;
}

function triplet(t, change = {}) {
  const root = mkdtempSync(join(tmpdir(), "pe-align-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const smoke = sheet([60, 55, 50, 200], [0, 0, 0, 0], () => 12);
  const base = {
    early: { blend: "transparent", cellSeconds: [0, 1 / 60, 2 / 60, 3 / 60], cellSpan: [1, 1.1, 1.3, 1.6], texture: smoke },
    late: { blend: "transparent", cellSeconds: [1.1, 2, 3, 4], cellSpan: [2.5, 3, 3.4, 3.6], texture: smoke },
    fire: { blend: "additive", cellSeconds: [0, 1 / 60, 2 / 60, 3 / 60], cellSpan: [1, 1.1, 1.3, 1.6],
      texture: sheet([255, 180, 60, 255], [0, 0, 0, 255], () => 8) },
  };
  return Object.fromEntries(Object.entries(base).map(([role, spec]) => [role, entry(root, role, { ...spec, ...change[role] })]));
}

const failed = (record) => record.checks.filter((c) => !c.ok).map((c) => c.name);

test("a triplet from one sim, matched and nested, is aligned", (t) => {
  const record = align(triplet(t));
  assert.deepEqual(failed(record), []);
  assert.deepEqual([record.ok, record.exitCode], [true, 0]);
  assert.equal(record.checks.at(-1).detail.outside, 0);
});

test("a different seed, parameter or module is not one simulation", (t) => {
  assert.deepEqual(failed(align(triplet(t, { fire: { seed: 4 } }))), ["one simulation: the same seed"]);
  assert.deepEqual(failed(align(triplet(t, { late: { parameters: { blast: 3, turbulence: 0.6 } } }))), ["one simulation: the same parameters"]);
  const other = align(triplet(t, { fire: { modules: { explosion_sim: "b".repeat(64) } } }));
  assert.deepEqual([failed(other), other.exitCode], [["one simulation: the same modules"], 3]);
});

test("the early smoke and the fire must share every cell's time and span", (t) => {
  assert.deepEqual(failed(align(triplet(t, { fire: { cellSeconds: [0, 2 / 60, 3 / 60, 4 / 60] } }))),
    ["the early smoke and the fire show the same sim time in every cell"]);
  assert.deepEqual(failed(align(triplet(t, { fire: { cellSpan: [1, 1.1, 1.3, 1.7] } }))),
    ["the early smoke and the fire are framed to the same span in every cell"]);
});

test("the late smoke must follow the early one, and no sheet's spans may shrink", (t) => {
  assert.deepEqual(failed(align(triplet(t, { late: { cellSeconds: [0.05, 2, 3, 4] } }))), ["the late smoke's cells follow the early smoke's"]);
  assert.deepEqual(failed(align(triplet(t, { late: { cellSpan: [1.5, 3, 3.4, 3.6] } }))), ["the late smoke's spans carry on without shrinking"]);
  assert.deepEqual(failed(align(triplet(t, { late: { cellSpan: [2.5, 3, 2.9, 3.6] } }))), ["the late sheet's spans never shrink"]);
});

test("fire lit where the smoke is clear fails, naming the worst cell", (t) => {
  // Cell 2's fire reaches past the smoke's edge.
  const fire = sheet([255, 180, 60, 255], [0, 0, 0, 255], (k) => (k === 2 ? 16 : 8));
  const record = align(triplet(t, { fire: { texture: fire } }));
  assert.deepEqual(failed(record), [record.checks.at(-1).name]);
  assert.equal(record.checks.at(-1).detail.cell, 2);
  assert.ok(record.checks.at(-1).detail.outside > 0.05);
});

test("bad input is refused with exit 2", (t) => {
  for (const argv of [[], ["--early", "a", "--late", "b"], ["--early", "a", "--late", "b", "--fire", "c", "--fire", "d"], ["--smoke", "a"]])
    assert.throws(() => parseArgs(argv), (error) => error.exitCode === 2, JSON.stringify(argv));
  const dirs = triplet(t);
  assert.throws(() => align({ ...dirs, fire: dirs.early }), /--fire is transparent, not additive/);
  assert.throws(() => align({ ...dirs, late: join(dirs.late, "missing") }), /--late is not a cache entry/);
  assert.throws(() => align(triplet(t, { early: { cellSpan: null } })), /records no cell times and spans/);
});
