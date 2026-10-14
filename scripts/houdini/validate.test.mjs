// [houdini] Editor validation's checks, on records shaped like real ones:
// the capture's attestation, the editor's texture log, the flipbook playing
// and the 0..15 oracle.
// The editor itself runs in the `houdini` lane.
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { ROOT_MARKER } from "./generate.mjs";
import { KINDS, NORMAL_TEXTURE, TICKS_PER_CELL, checkAttestation, checkPlays, checkTextureLog, kindFor, lifetimeTicks, observationSpec,
  parseArgs, readCells, readOrder, validate } from "./validate.mjs";

const NAME = "pe_gen_debug-atlas_568d7942.tga", SHA = "568d7942".padEnd(64, "a"), ROOT = "C:\\gen\\out";
const capture = ({ resolved = [NAME, NORMAL_TEXTURE], unresolved = [], winner = { kind: "loose", path: `${ROOT}\\Data\\Art\\Textures\\${NAME}` }, sha = SHA } = {}) => ({
  content: { resolvedTextures: resolved, unresolvedTextures: unresolved,
    resolvedTextureProvenance: { [NAME]: { identity: NAME.toUpperCase(), sha256: sha, winner } } },
});
const layered = { name: NAME, sha: SHA, root: ROOT, layered: true }, control = { ...layered, layered: false };

test("each texture shape has its fixture", () => {
  assert.deepEqual([{ blend: "additive", grid: 4 }, { blend: "additive", grid: 1 }, { blend: "transparent", grid: 1 },
    { blend: "transparent", grid: 4 }, { blend: "transparent", grid: 8 }].map(kindFor), ["fireball", "ring", "puff", "smoke4", "smoke8"]);
  for (const shape of [{ blend: "additive", grid: 8 }, { blend: "additive", grid: 2 }, { blend: "multiply", grid: 1 }])
    assert.throws(() => kindFor(shape), (error) => error.exitCode === 2);
  // src/effect/HoudiniValidationFixture.h: 2 s single textures, 4 s 4x4 flipbooks, a 6.4 s 8x8 one.
  assert.deepEqual(Object.keys(KINDS).map((kind) => lifetimeTicks(kind) / 60), [2, 2, 4, 4, 6.4]);
  assert.equal(TICKS_PER_CELL, 15);
});

test("the observation spec adds exactly the given layers, and outlasts the fixture", () => {
  assert.deepEqual(observationSpec([ROOT], "C:\\v\\fixture.alo"), { schemaVersion: 2, contentLayers: [ROOT],
    scenario: { aloPath: "C:\\v\\fixture.alo", maxFrames: 300 }, lifecycle: { type: "oneshot" }, output: { strideInterval: 30, playbackStride: 1 } });
  for (const kind of Object.keys(KINDS)) assert.ok(observationSpec([], "f.alo", kind).scenario.maxFrames >= lifetimeTicks(kind) + 60, kind);
  assert.equal(observationSpec([], "f.alo", "smoke8").scenario.maxFrames, 444);
});

test("attestation: the layered run resolves the name to the published file, by hash", () => {
  assert.deepEqual(checkAttestation(capture(), layered), []);
  // The capture writes paths with either slash and any case.
  assert.deepEqual(checkAttestation(capture({ winner: { kind: "loose", path: `c:/GEN/out/Data/Art/Textures/${NAME}` } }), layered), []);
  for (const [damage, pattern] of [
    [{ resolved: [NORMAL_TEXTURE], unresolved: [NAME] }, /did not resolve/],
    [{ resolved: [NAME] }, /p_particle_depth_master.tga did not resolve/],
    [{ winner: { kind: "archive", path: "D:\\game\\Data\\Textures.meg" } }, /not the published file/],
    [{ winner: { kind: "loose", path: `C:\\other\\Data\\Art\\Textures\\${NAME}` } }, /not the published file/],
    [{ winner: { kind: "loose", path: `${ROOT}\\Data\\Art\\Textures\\${NAME}.bak` } }, /not the published file/],
    [{ sha: "b".repeat(64) }, /resolved to bytes b+, not 568d7942/],
  ]) assert.match(checkAttestation(capture(damage), layered).join(), pattern);
});

test("attestation: the control run leaves the name unresolved", () => {
  assert.deepEqual(checkAttestation(capture({ resolved: [NORMAL_TEXTURE], unresolved: [NAME] }), control), []);
  assert.match(checkAttestation(capture(), control).join(), /still resolved/);
  assert.match(checkAttestation(capture({ resolved: [NORMAL_TEXTURE], unresolved: [] }), control).join(), /still resolved/);
});

test("the editor's log must request the name, then decode it in the file's format", () => {
  const log = (loaded) => ["[shader-gate] something", "[tex-gate] getTexture(p_particle_depth_master.tga)",
    "[tex-gate] loaded 512x512 fmt=894720068", `[tex-gate] getTexture(${NAME})`, loaded, `[tex-gate] getTexture(${NAME})`].join("\r\n");
  assert.deepEqual(checkTextureLog(log("[tex-gate] loaded 128x128 fmt=21"), { name: NAME, size: 128 }), []);
  assert.match(checkTextureLog(log("[tex-gate] loaded 128x128 fmt=22"), { name: NAME, size: 128 }).join(), /not 128x128 fmt=21/);
  assert.match(checkTextureLog(log("[tex-gate] loaded 64x64 fmt=21"), { name: NAME, size: 128 }).join(), /not 128x128 fmt=21/);
  // The placeholder path logs no load at all.
  assert.match(checkTextureLog(log("[tex-gate] getTexture(other.tga)"), { name: NAME, size: 128 }).join(), /loaded as "\[tex-gate\] getTexture/);
  assert.match(checkTextureLog("[tex-gate] getTexture(other.tga)", { name: NAME, size: 128 }).join(), /never requested/);
  // A DDS loads as its DXT format, which D3D reports by FourCC.
  assert.deepEqual(checkTextureLog(log("[tex-gate] loaded 128x128 fmt=894720068"), { name: NAME, size: 128, format: "dxt5" }), []);
  assert.deepEqual(checkTextureLog(log("[tex-gate] loaded 128x128 fmt=827611204"), { name: NAME, size: 128, format: "dxt1" }), []);
  assert.match(checkTextureLog(log("[tex-gate] loaded 128x128 fmt=21"), { name: NAME, size: 128, format: "dxt5" }).join(),
    /not 128x128 fmt=894720068/);
});

// A playback frame: a dark blue background, the particle's colour added over a centre square.
function frame(cell, { background = [0, 0, 14], blue = 128, grid = 4, red = 0 } = {}) {
  const width = 96, height = 72, rgba = Buffer.alloc(width * height * 4);
  const step = 255 / (grid - 1), add = cell === null ? [0, 0, 0] : [Math.round((cell % grid) * step) + red, Math.round(Math.floor(cell / grid) * step), blue];
  for (let y = 0; y < height; y++) for (let x = 0; x < width; x++) {
    const inside = Math.abs(x - width / 2) < 24 && Math.abs(y - height / 2) < 24;
    rgba.set([...background.map((v, c) => Math.min(255, v + (inside ? add[c] : 0))), 255], (y * width + x) * 4);
  }
  return { width, height, rgba };
}
const cellAt = (map) => (tick) => frame(map(Math.floor(tick / TICKS_PER_CELL)));

test("the order oracle reads 0..15 and catches a flip, a transposition and an off-by-one", () => {
  const ticks = [];
  const order = readOrder((tick) => { ticks.push(tick); return frame(Math.floor(tick / TICKS_PER_CELL)); });
  assert.deepEqual(order, { sequence: [...Array(16).keys()], errors: [] });
  assert.deepEqual(ticks, [...Array(16).keys()].map((k) => 15 * k + 7), "each cell is read mid-way");
  const wrong = (map) => readOrder(cellAt(map));
  assert.match(wrong((k) => 15 - k).errors[0], /cell 0 showed 15/);                       // flipped
  assert.match(wrong((k) => (k % 4) * 4 + Math.floor(k / 4)).errors[0], /cell 1 showed 4/); // transposed
  assert.match(wrong((k) => Math.max(0, k - 1)).errors[0], /cell 1 showed 0/);             // a cell late
  assert.match(wrong((k) => 12 - 4 * Math.floor(k / 4) + (k % 4)).errors[0], /cell 0 showed 12/); // rows bottom-up
  // The placeholder, or no particle, is no cell at all.
  assert.match(readOrder(() => frame(null)).errors[0], /cell 0 showed no cell/);
  const white = { width: 96, height: 72, rgba: Buffer.alloc(96 * 72 * 4, 255) };
  assert.equal(readOrder(() => white).sequence[0], null);
  // A colour between levels is ambiguous, not rounded into a pass.
  assert.deepEqual(readOrder((tick) => frame(Math.floor(tick / 15), { red: 20 })).errors, [], "a quarter level off still reads");
  assert.match(readOrder((tick) => frame(Math.floor(tick / 15), { red: 42 })).errors[0], /cell 0 showed no cell/);
  assert.match(readOrder((tick) => frame(Math.floor(tick / 15), { blue: 60 })).errors[0], /no cell/);
});

test("a flipbook plays when its cells differ at the view's centre", () => {
  const ticks = [];
  const cells = readCells((tick) => { ticks.push(tick); return frame(Math.floor(tick / TICKS_PER_CELL)); });
  assert.deepEqual(ticks, [...Array(16).keys()].map((k) => 15 * k + 7), "each cell is read mid-way");
  assert.deepEqual(cells[0], [0, 0, 128], "less the background");
  // An 8x8 sheet held 6 ticks a cell is read at 3, 9, 15, ...
  const eight = [];
  readCells((tick) => { eight.push(tick); return frame(0); }, 8, KINDS.smoke8.ticksPerCell);
  assert.deepEqual(eight, [...Array(64).keys()].map((k) => 6 * k + 3));
  assert.deepEqual(checkPlays(cells), []);
  // Any order plays; one cell held, or nothing shown, does not.
  assert.deepEqual(checkPlays(readCells(cellAt((k) => 15 - k))), []);
  assert.match(checkPlays(readCells(cellAt(() => 5))).join(), /does not play/);
  assert.match(checkPlays(readCells(() => frame(null))).join(), /spread 0\.0/);
  // A flicker below the control margin is not playback.
  assert.match(checkPlays(readCells((tick) => frame(0, { red: Math.floor(tick / TICKS_PER_CELL) }))).join(), /spread 15\.0/);
});

test("parseArgs needs --entry and --out, once each", () => {
  const args = parseArgs(["--entry", "C:\\gen\\cache\\k", "--out", "C:\\v"]);
  assert.deepEqual(Object.keys(args).sort(), ["entry", "out"]);
  for (const argv of [[], ["--entry", "a"], ["--entry", "a", "--out", "b", "--entry", "c"], ["--entry", "a", "--out", "b", "--seed", "1"], ["--out"]])
    assert.throws(() => parseArgs(argv), (error) => error.exitCode === 2);
});

test("only a cache entry under its own key, in a root the wrapper made, is validated", async (t) => {
  const base = mkdtempSync(join(tmpdir(), "pe-houdini-validate-"));
  t.after(() => rmSync(base, { recursive: true, force: true }));
  const key = "a".repeat(64);
  const entry = (root, folder, generatorId = "debug-atlas") => {
    const dir = join(base, root, "cache", folder);
    mkdirSync(dir, { recursive: true });
    writeFileSync(join(dir, "provenance.json"), JSON.stringify({ cacheKey: key, generatorId, texture: { name: NAME, grid: 4, blend: "additive", size: 128 } }));
    return dir;
  };
  const refuses = (dir, pattern) => assert.rejects(validate({ entry: dir, out: join(base, "out") }, { observeRun: () => assert.fail("must not observe") }),
    (error) => error.exitCode === 2 && pattern.test(error.message));
  await refuses(join(base, "nowhere"), /not a cache entry/);
  await refuses(entry("unmarked", key), /not a cache entry in a generated root/);
  const misplaced = entry("marked", "b".repeat(64));
  writeFileSync(join(base, "marked", ROOT_MARKER), "x");
  await refuses(misplaced, /not a cache entry in a generated root/);
  // The checks depend on the generator, so one missing, or not the texture's, is refused.
  for (const [root, generatorId] of [["unnamed", null], ["misnamed", "ground-fireball-4x4"], ["prefix", "debug"]]) {
    const dir = entry(root, key, generatorId);
    writeFileSync(join(base, root, ROOT_MARKER), "x");
    await assert.rejects(validate({ entry: dir, out: join(base, "out") }), (error) => error.exitCode === 4 && /names no generator/.test(error.message));
  }
  // Under its key in a marked root, it gets as far as re-hashing the entry.
  await assert.rejects(validate({ entry: entry("marked", key), out: join(base, "out") }),
    (error) => error.exitCode === 4 && !/names no generator/.test(error.message));
});
