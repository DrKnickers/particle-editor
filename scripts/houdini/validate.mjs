// [houdini] Editor validation (plan §3.6): does the editor load the
// generated texture, and only from the generated root?
//   node scripts/houdini/validate.mjs --entry <cache entry> --out <empty dir> [--exe <ParticleEditor.exe>]
//
// It writes the validation fixture for the entry's shape (a one-particle
// emitter naming the texture), then observes it twice:
//   layered - contentLayers = [the generated root], with ALO_SHADER_DIAG=1:
//             the capture resolves the name to the published file, by hash,
//             and the editor's log loads it as a 32-bit texture (fmt=21);
//   control - contentLayers = []: the same name resolves to nothing, so the
//             editor shows its placeholder, and the view differs.
// The content-addressed name and the explicit layer stack mean no other file
// can satisfy the check. A 4x4 atlas is observed through the fireball
// fixture, and the colour at the view's centre must change from cell to cell
// (the editor plays the flipbook, not one cell). Over debug-atlas it must
// step through the cells in order (its oracle: red = i % 4, green = i / 4,
// blue 0.5).
//
// The editor's log of each run is kept beside its bundle as <run>.editor-log.txt.
// Prints one `[houdini-validate]` JSON line. Exit 0 valid, 2 bad input,
// 4 not valid; an observation failure keeps observe's own code.
import * as fs from "node:fs";
import { spawn, spawnSync } from "node:child_process";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { resolveReplayGameRoots } from "../replay-proof.mjs";
import { runObservation, spawnWithDeadline } from "../observe.mjs";
import { fail } from "./rules.mjs";
import { sha256, verifyEntry } from "./cache.mjs";
import { ROOT_MARKER } from "./generate.mjs";
import { decodePng, meanRgb } from "./images.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
// The emitter's default normal texture: a fixture can't record "none" (PR 2a delta 6).
export const NORMAL_TEXTURE = "p_particle_depth_master.tga";
// src/effect/HoudiniValidationFixture.h: the fireball holds each cell 15 ticks of the 60 Hz clock.
export const TICKS_PER_CELL = 15;
const BOX = 32, CORNER = 8, BLUE = 128, BLUE_SLACK = 16, LEVEL_SLACK = 0.25, CONTROL_DELTA = 24;

export function kindFor({ blend, grid }) {
  if (grid === 4 && blend === "additive") return "fireball";
  if (grid === 1) return blend === "additive" ? "ring" : "puff";
  throw fail(`no validation fixture shows a ${grid}x${grid} ${blend} texture`, 2);
}

export const observationSpec = (layers, aloPath) => ({
  schemaVersion: 2, contentLayers: layers, scenario: { aloPath, maxFrames: 300 },
  lifecycle: { type: "oneshot" }, output: { strideInterval: 30, playbackStride: 1 },
});

const folded = (path) => path.replace(/\//g, "\\").replace(/\\+$/, "").toLowerCase();

// The capture's own record of where each texture came from.
export function checkAttestation(capture, { name, sha, root, layered }) {
  const content = capture?.content ?? {};
  const resolved = content.resolvedTextures ?? [], unresolved = content.unresolvedTextures ?? [];
  const errors = [];
  if (!resolved.includes(NORMAL_TEXTURE)) errors.push(`${NORMAL_TEXTURE} did not resolve`);
  if (!layered) {
    if (resolved.includes(name) || !unresolved.includes(name)) errors.push(`without the generated root, ${name} still resolved`);
    return errors;
  }
  if (!resolved.includes(name)) return [...errors, `${name} did not resolve`];
  const winner = content.resolvedTextureProvenance?.[name];
  const want = folded(`${root}\\Data\\Art\\Textures\\${name}`);
  if (winner?.winner?.kind !== "loose" || folded(String(winner.winner.path)) !== want)
    errors.push(`${name} resolved to ${JSON.stringify(winner?.winner ?? null)}, not the published file`);
  if (winner?.sha256 !== sha) errors.push(`${name} resolved to bytes ${winner?.sha256 ?? "unknown"}, not ${sha}`);
  return errors;
}

// ALO_SHADER_DIAG: the request for the name, then its decode as A8R8G8B8 (21).
export function checkTextureLog(stdout, { name, size }) {
  const lines = stdout.split(/\r?\n/).filter((line) => line.startsWith("[tex-gate]"));
  const at = lines.indexOf(`[tex-gate] getTexture(${name})`);
  if (at < 0) return [`the editor never requested ${name}`];
  const next = lines[at + 1] ?? "";
  return next === `[tex-gate] loaded ${size}x${size} fmt=21` ? [] : [`${name} loaded as "${next}", not ${size}x${size} fmt=21`];
}

const centre = (image) => meanRgb(image, (image.width - BOX) >> 1, (image.height - BOX) >> 1, BOX, BOX);
const corner = (image) => meanRgb(image, CORNER, CORNER, CORNER, CORNER);

// The view centre's colour at the middle of each cell's 15 ticks, less the
// background. `frameAt(tick)` returns a decoded playback frame.
export function readCells(frameAt, grid = 4) {
  return Array.from({ length: grid * grid }, (_, k) => {
    const image = frameAt(TICKS_PER_CELL * k + (TICKS_PER_CELL >> 1)), background = corner(image);
    return centre(image).map((v, c) => v - background[c]);
  });
}

// A flipbook plays when its cells don't all look alike at the view's centre.
export function checkPlays(cells) {
  let spread = 0;
  for (const a of cells) for (const b of cells) spread = Math.max(spread, ...a.map((v, c) => Math.abs(v - b[c])));
  return spread >= CONTROL_DELTA ? [] : [`the cells all look alike at the view's centre (spread ${spread.toFixed(1)}): the flipbook does not play`];
}

// The 0..15 oracle, read at the middle of each cell's 15 ticks.
// `frameAt(tick)` returns a decoded playback frame.
export function readOrder(frameAt, grid = 4) {
  const step = 255 / (grid - 1), sequence = [], errors = [];
  for (let k = 0; k < grid * grid; k++) {
    const image = frameAt(TICKS_PER_CELL * k + (TICKS_PER_CELL >> 1));
    const background = corner(image), delta = centre(image).map((v, c) => v - background[c]);
    const [r, g] = [delta[0] / step, delta[1] / step];
    const level = [Math.round(r), Math.round(g)];
    const clean = Math.abs(r - level[0]) <= LEVEL_SLACK && Math.abs(g - level[1]) <= LEVEL_SLACK &&
      Math.abs(delta[2] - BLUE) <= BLUE_SLACK && level.every((v) => v >= 0 && v < grid);
    const seen = clean ? level[1] * grid + level[0] : null;
    sequence.push(seen);
    if (seen !== k) errors.push(`cell ${k} showed ${seen ?? `no cell (centre less background ${delta.map((v) => v.toFixed(1)).join(",")})`}`);
  }
  return { sequence, errors };
}

// Playback frame for a tick, by the capture's own index.
function playbackFrame(bundle, capture, io) {
  return (tick) => {
    const entry = (capture.playback ?? []).find((v) => v.frame === tick);
    if (!entry) throw fail(`the observation has no playback frame for tick ${tick}`, 4);
    return decodePng(io.readFileSync(join(bundle, "playback-frames", `frame_${String(entry.index).padStart(5, "0")}.png`)));
  };
}

export function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i += 2) {
    const flag = argv[i], value = argv[i + 1];
    if (!["--entry", "--out", "--exe"].includes(flag) || value === undefined || value.startsWith("--") || flag.slice(2) in args)
      throw fail(`usage: validate.mjs --entry <cache entry> --out <empty dir> [--exe <ParticleEditor.exe>]`, 2);
    args[flag.slice(2)] = resolve(value);
  }
  if (!args.entry || !args.out) throw fail("--entry and --out are required", 2);
  return args;
}

// A cache entry in a generated root, checked file by file against its
// provenance, whose texture is published byte for byte.
export function openEntry(entry, io = fs) {
  let provenance;
  try { provenance = JSON.parse(io.readFileSync(join(entry, "provenance.json"), "utf8")); }
  catch { throw fail(`not a cache entry: ${entry}`, 2); }
  const { name, grid } = provenance.texture ?? {};
  // The entry must sit under its own key, in a root the wrapper made.
  const root = resolve(entry, "..", "..");
  if (basename(entry) !== provenance.cacheKey || basename(dirname(entry)) !== "cache" || !io.existsSync(join(root, ROOT_MARKER)))
    throw fail(`not a cache entry in a generated root: ${entry}`, 2);
  // Which checks run depends on the generator, so an entry that doesn't name
  // the one its texture is named for is refused, not validated with fewer.
  const generator = provenance.generatorId;
  if (typeof generator !== "string" || typeof name !== "string" || !name.startsWith(`pe_gen_${generator}_`))
    throw fail(`the entry's provenance names no generator matching its texture (${generator}, ${name})`, 4);
  verifyEntry(entry, provenance.cacheKey, grid * grid, io);
  let published;
  try { published = io.readFileSync(join(root, "Data", "Art", "Textures", name)); }
  catch { throw fail(`${name} is not published under ${root}`, 4); }
  if (sha256(published) !== provenance.contentHash) throw fail(`the published ${name} differs from its cache entry`, 4);
  return { provenance, root, published };
}

export async function validate({ entry, out, exe = join(repoRoot, "x64", "Release", "ParticleEditor.exe") }, {
  io = fs, roots = () => resolveReplayGameRoots({}), observeRun = runObservation, fixture = writeFixture,
} = {}) {
  const record = { ok: false, exitCode: 4, entry };
  const { provenance, root } = openEntry(entry, io);
  const { name, grid, blend, size } = provenance.texture;
  const generator = provenance.generatorId;
  const kind = kindFor({ blend, grid });
  Object.assign(record, { texture: name, kind, contentHash: provenance.contentHash });
  if (io.existsSync(out) && io.readdirSync(out).length) throw fail(`--out must be absent or empty: ${out}`, 2);
  io.mkdirSync(out, { recursive: true });
  const gameRoots = roots();
  if (!gameRoots.length) throw fail("missing game data: host lookup found no usable MEG archive", 1);

  const alo = join(out, "fixture.alo");
  fixture(exe, kind, name, alo);
  const errors = [], runs = {};
  for (const [label, layers] of [["layered", [root]], ["control", []]]) {
    const spec = join(out, `${label}.json`), bundle = join(out, label);
    io.writeFileSync(spec, `${JSON.stringify(observationSpec(layers, alo), null, 2)}\n`);
    let stdout = "";
    // Only the editor gets the diagnostic switch; observe's ffmpeg runs as usual.
    const run = (bin, args, options) => (bin !== exe ? spawnWithDeadline(bin, args, options)
      : spawnWithDeadline(bin, args, { ...options, spawnImpl: (b, a, o) => spawn(b, a, { ...o, env: { ...process.env, ALO_SHADER_DIAG: "1" } }) })
        .then((result) => { stdout = result.stdout; return result; }));
    await observeRun(spec, bundle, { exe, roots: gameRoots, run, io });
    io.writeFileSync(join(out, `${label}.editor-log.txt`), stdout);
    const capture = JSON.parse(io.readFileSync(join(bundle, "capture.json"), "utf8"));
    runs[label] = { bundle, capture, stdout };
    errors.push(...checkAttestation(capture, { name, sha: provenance.contentHash, root, layered: label === "layered" })
      .map((error) => `${label}: ${error}`));
  }
  errors.push(...checkTextureLog(runs.layered.stdout, { name, size }).map((error) => `layered: ${error}`));
  // The placeholder must look different from the texture at the view's centre.
  const tick = TICKS_PER_CELL >> 1;
  const [shown, placeholder] = ["layered", "control"].map((label) => centre(playbackFrame(runs[label].bundle, runs[label].capture, io)(tick)));
  const difference = Math.max(...shown.map((v, c) => Math.abs(v - placeholder[c])));
  if (difference < CONTROL_DELTA) errors.push(`the control run looks the same as the layered one (centre differs by ${difference.toFixed(1)})`);
  if (kind === "fireball") {
    const frameAt = playbackFrame(runs.layered.bundle, runs.layered.capture, io);
    errors.push(...checkPlays(readCells(frameAt, grid)).map((error) => `layered: ${error}`));
    if (generator === "debug-atlas") {
      const order = readOrder(frameAt, grid);
      record.sequence = order.sequence;
      errors.push(...order.errors.map((error) => `order: ${error}`));
    }
  }
  Object.assign(record, errors.length ? { exitCode: 4, error: errors.join("; ") } : { ok: true, exitCode: 0 });
  return record;
}

function writeFixture(exe, kind, name, path) {
  const r = spawnSync(exe, ["--gen-houdini-validation-fixture", kind, name, path], { windowsHide: true, encoding: "utf8", timeout: 60000 });
  if (r.status !== 0 || !fs.existsSync(path)) throw fail(`the fixture flag failed (exit ${r.status}): ${r.stderr}`, 1);
}

async function main() {
  let record;
  try { record = await validate(parseArgs(process.argv.slice(2))); }
  catch (error) { record = { ok: false, exitCode: error.exitCode ?? 1, error: error.message }; }
  console.log(`[houdini-validate] ${JSON.stringify(record)}`);
  process.exitCode = record.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) await main();
