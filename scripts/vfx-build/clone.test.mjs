// [vfx-build] The stock-emitter cloner, on inspect JSON: no exe. `apply` stands
// in for `--tool build`, near enough for these checks: it writes the fields,
// group fields, vector components, locks and tracks the ops name onto a new
// emitter.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { cloneOps } from "./clone.mjs";
import { LINEAR, SMOOTH, STEP } from "./tracks.mjs";
const here = fileURLToPath(new URL(".", import.meta.url));
const NEW = JSON.parse(readFileSync(join(here, "fixtures", "new-emitter.json"), "utf8"));

function apply(ops, name = "E") {
  const e = structuredClone(NEW);
  e.name = name;
  for (const op of ops) {
    if (op.op === "setProperty") {
      const group = /^groups\[(\d)\]\.(\w+)$/.exec(op.field), vector = /^(\w+)\[(\d)\]$/.exec(op.field);
      if (group) e.groups[group[1]][group[2]] = op.value;
      else if (vector) e[vector[1]][vector[2]] = op.value;
      else e[op.field] = op.value;
    }
    if (op.op === "setColorTexture") e.colorTexture = op.name;
    if (op.op === "setNormalTexture") e.normalTexture = op.name;
    if (op.op === "setTrackLock") e.tracks[op.track].source = op.lockTo ?? op.track;
    if (op.op === "setInterpolation") e.tracks[op.track].interpolation = { linear: LINEAR, smooth: SMOOTH, step: STEP }[op.interpolation];
    if (op.op === "replaceTrack") e.tracks[op.track].keys = op.keys;
  }
  return e;
}

test("cloneOps names nothing for a new emitter", () => {
  assert.deepEqual(cloneOps(NEW, NEW), []);
});

test("cloneOps rebuilds an emitter's fields, groups, vectors, texture, locks and tracks", () => {
  const stock = structuredClone(NEW);
  Object.assign(stock, { name: "debre", lifetime: 6, blendMode: 2, useBursts: true, nParticlesPerBurst: 3, gravity: 49,
                         colorTexture: "p_particle_master.tga" });
  stock.acceleration[2] = -5;
  stock.groups[0] = { ...stock.groups[0], type: 4, cylinderRadius: 75, cylinderHeight: 78 };
  stock.groups[2] = { ...stock.groups[2], type: 0, valZ: 40 };
  stock.tracks[1].source = 1;                     // green owns its keys
  stock.tracks[1].keys = [[0, 0.5], [100, 0]];
  stock.tracks[3].source = 1;                     // alpha follows green
  stock.tracks[0].keys = [[0, 1], [40, 0.6], [100, 0]];
  stock.tracks[0].interpolation = SMOOTH;
  stock.tracks[5] = { ...stock.tracks[5], keys: [[0, 6], [100, 2]] };
  const ops = cloneOps(stock, NEW);
  const built = apply(ops, stock.name);
  for (const field of ["lifetime", "blendMode", "useBursts", "nParticlesPerBurst", "gravity", "colorTexture", "acceleration", "tracks"])
    assert.deepEqual(built[field], stock[field], field);
  assert.deepEqual([built.groups[0], built.groups[2]], [stock.groups[0], stock.groups[2]]);
  // Red is edited while blue still follows it: the ops name it, as the lock rules require.
  const red = ops.filter((o) => o.track === 0);
  assert.ok(red.length && red.every((o) => JSON.stringify(o.dependents) === "[2]"), JSON.stringify(red));
  assert.ok(ops.find((o) => o.track === 1 && o.op === "replaceTrack").dependents.includes(3));
  // Locks come before any key edit.
  const firstKeys = ops.findIndex((o) => o.op === "replaceTrack" || o.op === "setInterpolation");
  assert.ok(ops.slice(firstKeys).every((o) => o.op !== "setTrackLock"));
  // The lifetime group is derived on save: never written.
  assert.ok(!ops.some((o) => /^groups\[1\]/.test(o.field ?? "")));
});

test("cloneOps leaves the rotation track alone while randomRotation is on", () => {
  const stock = structuredClone(NEW);
  stock.randomRotation = true;
  stock.tracks[6] = { ...stock.tracks[6], keys: [[0, 0], [100, 3]] };
  assert.ok(!cloneOps(stock, NEW).some((o) => o.track === 6));
});

