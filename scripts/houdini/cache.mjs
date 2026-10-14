// [houdini] The keyed cache and its provenance (plan §3.1 steps 2, 9-10;
// spec §21.6).
//
// The key hashes everything that can change an output: the whole registry
// entry, every file under scripts/houdini/, the inputs, the pinned build, the
// thread count and, for a DDS entry, the pinned texconv and d3dx9. A cache entry is trusted only after its files hash to
// what its provenance says, on every hit.
//
// Provenance is deterministic: no times, hosts, users, absolute paths or
// licence data. Those that matter (times) go to the audit log instead.
import * as fs from "node:fs";
import { createHash } from "node:crypto";
import { join, relative, sep } from "node:path";

export const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });

// JSON with sorted keys at every level; refuses what JSON can't round-trip.
export function canonical(value) {
  if (value === null || typeof value === "boolean" || typeof value === "string") return JSON.stringify(value);
  if (typeof value === "number") {
    if (!Number.isFinite(value)) throw fail("canonical JSON has a non-finite number");
    return JSON.stringify(value);
  }
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (typeof value === "object") {
    return `{${Object.keys(value).sort().map((key) => `${JSON.stringify(key)}:${canonical(value[key])}`).join(",")}}`;
  }
  throw fail(`canonical JSON has a ${typeof value}`);
}

// Every file under `dir`, by forward-slash relative path. A link is refused:
// the key must describe the bytes actually here.
export function treeHashes(dir, io = fs) {
  const out = {};
  const walk = (at) => {
    for (const entry of io.readdirSync(at, { withFileTypes: true })) {
      const path = join(at, entry.name);
      if (entry.isSymbolicLink()) throw fail(`scripts/houdini holds a link: ${entry.name}`);
      if (entry.isDirectory()) walk(path);
      else if (entry.isFile()) out[relative(dir, path).split(sep).join("/")] = sha256(io.readFileSync(path));
      else throw fail(`scripts/houdini holds a special file: ${entry.name}`);
    }
  };
  walk(dir);
  return out;
}

export function cacheKey({ id, entry, tree, params, seed, tier, build, hoiiotool, threads, texconv = null }) {
  return sha256(canonical({ schemaVersion: 1, id, entry, tree, params, seed, tier, build, hoiiotool, threads, texconv }));
}

export function dirBytes(dir, io = fs) {
  let total = 0;
  const walk = (at) => {
    let entries;
    try { entries = io.readdirSync(at, { withFileTypes: true }); } catch { return; }
    for (const entry of entries) {
      const path = join(at, entry.name);
      if (entry.isDirectory()) walk(path);
      else { try { total += io.lstatSync(path).size; } catch { /* removed meanwhile */ } }
    }
  };
  walk(dir);
  return total;
}

// What hython reported, checked again by Node: exactly grid*grid frames,
// in order, each present and matching its hash.
export function checkManifest(text, count, dir, io = fs) {
  let manifest;
  try { manifest = JSON.parse(text); } catch { throw fail("frames.manifest.json is not JSON", 4); }
  const frames = manifest?.frames;
  if (manifest?.schemaVersion !== 1 || !Array.isArray(frames) || frames.length !== count)
    throw fail(`frame manifest lists ${frames?.length} frames, not ${count}`, 4);
  frames.forEach((frame, index) => {
    if (frame?.index !== index || frame.name !== `frames/frame_${String(index + 1).padStart(4, "0")}.exr`)
      throw fail(`frame ${index} is misnamed: ${JSON.stringify(frame?.name)}`, 4);
    let bytes;
    try { bytes = io.readFileSync(join(dir, ...frame.name.split("/"))); }
    catch { throw fail(`frame ${index} is missing`, 4); }
    if (sha256(bytes) !== frame.sha256) throw fail(`frame ${index} differs from the manifest`, 4);
  });
  const extra = io.readdirSync(join(dir, "frames")).filter((name) => !frames.some((f) => f.name === `frames/${name}`));
  if (extra.length) throw fail(`frames/ holds unlisted files: ${extra.join(", ")}`, 4);
  return manifest;
}

// Spec §21.6. "contentHash" is the texture's SHA-256, the identity its name
// carries; "usedBy" starts empty and validation starts "not-run".
export function buildProvenance({ id, entry, key, params, seed, tier, build, hoiiotool, threads, manifestSha256, texture,
  texconv = null }) {
  return {
    schemaVersion: 3,
    generatorId: id,
    generatorVersion: entry.version,
    hdaVersion: entry.builderSha256,
    modules: entry.modules ?? {},
    seed,
    parameters: params,
    outputProfile: texture.profile,
    contentHash: texture.sha256,
    particleEditorValidation: "not-run",
    gameVerification: "not-run",
    usedBy: [],
    texture: { name: texture.name, format: entry.format ?? "tga", blend: entry.blend, grid: entry.grid, cell: texture.cell,
      pad: texture.pad, size: texture.size, bytes: texture.bytes },
    cellSeconds: entry.cellSeconds ?? null,
    framesManifestSha256: manifestSha256,
    cacheKey: key,
    tier,
    renderer: entry.renderer,
    threads,
    houdiniBuild: build,
    hoiiotool,
    texconv,
  };
}

export const serialise = (value) => `${JSON.stringify(value, null, 2)}\n`;
export const TEXTURE_NAME = /^pe_gen_[a-z0-9-]{1,40}_[0-9a-f]{8}\.(tga|dds)$/;

// A cache hit is re-hashed before it is trusted; a bad entry is reported, not used.
export function verifyEntry(dir, key, count, io = fs) {
  let provenance;
  try { provenance = JSON.parse(io.readFileSync(join(dir, "provenance.json"), "utf8")); }
  catch { throw fail("cache entry has no readable provenance", 4); }
  if (provenance.cacheKey !== key) throw fail("cache entry belongs to another key", 4);
  const name = provenance.texture?.name;
  if (typeof name !== "string" || !TEXTURE_NAME.test(name) || name.slice(-12, -4) !== String(provenance.contentHash).slice(0, 8))
    throw fail("cache entry provenance names no texture of its own", 4);
  let texture;
  try { texture = io.readFileSync(join(dir, name)); } catch { throw fail("cache entry texture is missing", 4); }
  if (sha256(texture) !== provenance.contentHash) throw fail("cache entry texture differs from its provenance", 4);
  const text = io.readFileSync(join(dir, "frames.manifest.json"));
  if (sha256(text) !== provenance.framesManifestSha256) throw fail("cache entry manifest differs from its provenance", 4);
  checkManifest(text.toString("utf8"), count, dir, io);
  return provenance;
}

// Least recently used first, by the entry folder's mtime (a hit touches it).
// An entry holding a `published` marker is never evicted: its texture was
// copied out to Data\Art\Textures. Only folders the wrapper names (a
// 64-hex key holding provenance.json) are ever counted or removed.
export const KEY = /^[0-9a-f]{64}$/;
export function evict(cacheDir, maxBytes, { keep = null, io = fs } = {}) {
  let names;
  try { names = io.readdirSync(cacheDir); } catch { return []; }
  const ours = (name) => {
    const path = join(cacheDir, name);
    try { return KEY.test(name) && io.lstatSync(path).isDirectory() && io.existsSync(join(path, "provenance.json")); }
    catch { return false; }
  };
  const entries = names.filter(ours).map((name) => {
    const path = join(cacheDir, name);
    return { name, path, bytes: dirBytes(path, io), mtime: io.statSync(path).mtimeMs,
      published: io.existsSync(join(path, "published")) };
  });
  let total = entries.reduce((sum, e) => sum + e.bytes, 0);
  const evicted = [];
  for (const e of entries.filter((v) => !v.published && v.name !== keep).sort((a, b) => a.mtime - b.mtime)) {
    if (total <= maxBytes) break;
    io.rmSync(e.path, { recursive: true, force: true });
    total -= e.bytes;
    evicted.push(e.name);
  }
  return evicted;
}
