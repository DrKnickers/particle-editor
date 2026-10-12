// Packs (plan 3.4 pack.mjs): what an agent sees, built from an allowlist.
//
// A single-bundle pack:
//   prompt.md  rules.md  findings.schema.json  intent.md  textures.json
//   inspect.json (primary condition only)
//   frames/frame-NNNNN.png  filmstrip.png  filmstrip.json  selected.png  selected.json
//   summary.json  frames.jsonl
// A three-bundle pack puts the frame and telemetry files under bundle-a/,
// bundle-b/ and bundle-c/ (a recorded permutation) and keeps the shared files
// at its root. Nothing else, ever: frames.jsonl loses rngState and
// rngDrawCount (the generator is invertible, so they would name the seed),
// PNG text and time chunks are stripped, every file and directory time is
// normalised, and the copy is re-verified against pack.manifest.json.
import fs from "node:fs";
import { createHash } from "node:crypto";
import { dirname, join, relative, sep } from "node:path";
import { buildConcatList, buildFilmstripArgs, spawnWithDeadline } from "../observe.mjs";

export const PACK_TIME = new Date(Date.UTC(2000, 0, 1));
export const SHARED_FILES = ["prompt.md", "rules.md", "findings.schema.json", "intent.md", "textures.json"];
export const BUNDLE_FILES = ["filmstrip.png", "filmstrip.json", "selected.png", "selected.json", "summary.json", "frames.jsonl"];
const RNG_FIELDS = ["rngState", "rngDrawCount"];
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

// `toPack` (optional) renumbers the emitters: toPack[caseIndex] = packIndex.
export function stripRngLines(text, toPack = null) {
  return text.split("\n").filter((l) => l.trim()).map((l) => {
    const value = JSON.parse(l);
    for (const k of RNG_FIELDS) delete value[k];
    if (toPack) value.perEmitter = renumbered(value.perEmitter, toPack);
    return JSON.stringify(value);
  }).join("\n") + "\n";
}

// An order-blind pack (a draw-order case without inspect.json) numbers the
// emitters as its base does, so neither the telemetry nor the intent shows
// the order: out[toPack[i]] = list[i].
export function renumbered(list, toPack) {
  if (!Array.isArray(list) || list.length !== toPack.length) throw new Error("the renumbering does not cover every emitter");
  const out = new Array(list.length);
  list.forEach((v, i) => { out[toPack[i]] = v; });
  return out;
}

export function renumberSummary(text, toPack) {
  const summary = JSON.parse(text);
  const at = summary.emitters.map((e) => e.index);
  if (!at.every((index, i) => index === i)) throw new Error("summary.json emitters are not in index order");
  summary.emitters = renumbered(summary.emitters, toPack).map((e, index) => ({ ...e, index }));
  return JSON.stringify(summary, null, 2);   // the observer's own layout, so a renumbering that changes nothing changes no byte
}

// PNG chunks that carry text or a timestamp are dropped; image data is kept byte for byte.
const DROP = new Set(["tEXt", "zTXt", "iTXt", "tIME"]);
export function stripPngText(buffer) {
  const SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  if (buffer.length < 8 || !buffer.subarray(0, 8).equals(SIG)) throw new Error("not a PNG");
  const parts = [SIG];
  let at = 8;
  while (at < buffer.length) {
    if (at + 12 > buffer.length) throw new Error("truncated PNG chunk");
    const length = buffer.readUInt32BE(at);
    const type = buffer.toString("latin1", at + 4, at + 8);
    const end = at + 12 + length;
    if (end > buffer.length) throw new Error("truncated PNG chunk");
    if (!DROP.has(type)) parts.push(buffer.subarray(at, end));
    at = end;
    if (type === "IEND") break;
  }
  return Buffer.concat(parts);
}

export const pngHasText = (buffer) => stripPngText(buffer).length !== buffer.length;

// Tile -> frame maps for the two contact sheets (8 tiles per row).
export function tileMap(frames) {
  return { columns: 8, tiles: frames.map((frame, tile) => ({ tile, row: Math.floor(tile / 8), column: tile % 8, frame })) };
}

export function inventory(resolvedSets) {
  return [...new Set(resolvedSets.flat())].filter((n) => n !== "").sort();
}

export function manifestOf(files) {
  return { schemaVersion: 1, files: Object.entries(files).sort(([a], [b]) => (a < b ? -1 : 1)).map(([path, bytes]) => ({ path, sha256: sha256(bytes) })) };
}

// The pack's exact file set for its shape and condition.
export function allowlist({ bundles, frames, inspect }) {
  const files = [...SHARED_FILES, ...(inspect ? ["inspect.json"] : [])];
  for (const b of bundles) {
    const prefix = bundles.length === 1 ? "" : `${b}/`;
    for (const f of BUNDLE_FILES) files.push(`${prefix}${f}`);
    for (const frame of frames[b]) files.push(`${prefix}frames/frame-${String(frame).padStart(5, "0")}.png`);
  }
  return files.sort();
}

// --- I/O ---------------------------------------------------------------

function listTree(root, io = fs) {
  const out = { files: [], dirs: [] };
  const walk = (at) => {
    for (const entry of io.readdirSync(join(root, at), { withFileTypes: true })) {
      const rel = at ? `${at}/${entry.name}` : entry.name;
      if (entry.isSymbolicLink()) throw new Error(`pack holds a link: ${rel}`);
      if (entry.isDirectory()) { out.dirs.push(rel); walk(rel); } else out.files.push(rel);
    }
  };
  walk("");
  return { files: out.files.sort(), dirs: out.dirs.sort() };
}

// Builds one pack. `bundles`: { label: bundleDirectory } (one entry, or bundle-a/b/c).
// `toPack` renumbers the telemetry's emitters (an order-blind pack).
export async function buildPack({ out, bundles, shared, inspectJson = null, toPack = null, run = spawnWithDeadline, io = fs }) {
  if (toPack && inspectJson !== null) throw new Error("a pack with inspect.json keeps the case's numbering");
  if (io.existsSync(out)) throw new Error(`pack directory exists: ${out}`);
  io.mkdirSync(out, { recursive: true });
  const written = {};
  const put = (rel, bytes) => {
    const path = join(out, ...rel.split("/"));
    io.mkdirSync(dirname(path), { recursive: true });
    io.writeFileSync(path, bytes);
    written[rel] = Buffer.from(bytes);
  };
  for (const name of SHARED_FILES) put(name, shared[name]);
  if (inspectJson !== null) put("inspect.json", inspectJson);
  const labels = Object.keys(bundles).sort();
  const frameSets = {};
  for (const label of labels) {
    const dir = bundles[label];
    const prefix = labels.length === 1 ? "" : `${label}/`;
    const capture = JSON.parse(io.readFileSync(join(dir, "capture.json"), "utf8"));
    const frames = capture.frames.map((f) => f.frame);
    frameSets[label] = frames;
    for (const f of capture.frames) put(`${prefix}frames/frame-${String(f.frame).padStart(5, "0")}.png`, stripPngText(io.readFileSync(join(dir, f.png))));
    put(`${prefix}filmstrip.png`, stripPngText(io.readFileSync(join(dir, "filmstrip.png"))));
    put(`${prefix}filmstrip.json`, `${JSON.stringify(tileMap(capture.stride), null, 2)}\n`);
    const list = join(out, `.${label}.selected.txt`);
    io.writeFileSync(list, buildConcatList(capture, dir, frames));
    const sheet = join(out, `.${label}.selected.png`);
    const result = await run("ffmpeg", buildFilmstripArgs({ list, out: sheet, frameCount: frames.length }), { timeoutMs: 120000 });
    if (result.code !== 0) throw new Error(`ffmpeg selected.png exit ${result.code}`);
    put(`${prefix}selected.png`, stripPngText(io.readFileSync(sheet)));
    io.rmSync(list); io.rmSync(sheet);
    put(`${prefix}selected.json`, `${JSON.stringify(tileMap(frames), null, 2)}\n`);
    const summary = io.readFileSync(join(dir, "telemetry", "summary.json"));
    put(`${prefix}summary.json`, toPack ? renumberSummary(summary.toString("utf8"), toPack) : summary);
    put(`${prefix}frames.jsonl`, stripRngLines(io.readFileSync(join(dir, "telemetry", "frames.jsonl"), "utf8"), toPack));
  }
  const manifest = manifestOf(written);
  const tree = listTree(out, io);
  for (const rel of [...tree.files, ...tree.dirs]) io.utimesSync(join(out, ...rel.split("/")), PACK_TIME, PACK_TIME);
  io.utimesSync(out, PACK_TIME, PACK_TIME);
  return { manifest, frames: frameSets };
}

// Re-verifies a pack (or a staged copy) against its manifest and allowlist:
// content, the exact file set, normalised times, no PNG text, no RNG fields.
export function verifyPack(dir, manifest, expected, { io = fs, streams = () => [] } = {}) {
  const errors = [];
  const tree = listTree(dir, io);
  const want = [...expected].sort();
  if (JSON.stringify(tree.files) !== JSON.stringify(want)) errors.push(`file set differs from the allowlist: ${tree.files.filter((f) => !want.includes(f)).concat(want.filter((f) => !tree.files.includes(f))).join(", ")}`);
  const listed = new Map(manifest.files.map((f) => [f.path, f.sha256]));
  for (const rel of tree.files) {
    const bytes = io.readFileSync(join(dir, ...rel.split("/")));
    if (listed.get(rel) !== sha256(bytes)) errors.push(`${rel} does not match the manifest`);
    if (rel.endsWith(".png") && pngHasText(bytes)) errors.push(`${rel} carries PNG text`);
    if (rel.endsWith("frames.jsonl") && /"rng(State|DrawCount)"/.test(bytes.toString("utf8"))) errors.push(`${rel} carries RNG fields`);
  }
  for (const rel of ["", ...tree.files, ...tree.dirs]) {
    const st = io.statSync(rel ? join(dir, ...rel.split("/")) : dir);
    if (st.mtimeMs !== PACK_TIME.getTime()) errors.push(`${rel || "."} time is not normalised`);
    for (const s of streams(rel ? join(dir, ...rel.split("/")) : dir)) errors.push(`${rel || "."} carries an alternate data stream ${s}`);
  }
  return errors;
}

export const relativePath = (root, path) => relative(root, path).split(sep).join("/");
