// [houdini] The cache key, the frame manifest, provenance and eviction
// (plan §5 "Cache key", "Provenance", "Quotas").
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, mkdirSync, readFileSync, rmSync, symlinkSync, utimesSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { buildProvenance, cacheKey, canonical, checkManifest, evict, sha256, treeHashes, verifyEntry, serialise } from "./cache.mjs";

const scratch = (t) => {
  const dir = mkdtempSync(join(tmpdir(), "pe-houdini-cache-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
};

const ENTRY = { version: 1, builder: "generators/a.py", builderSha256: "b".repeat(64), renderer: "cop", grid: 2,
  params: { heat: { type: "float", min: 0, max: 2, default: 1 } },
  tiers: { preview: { cell: 32, timeoutMin: 5, maxStageMiB: 64 } } };
const BASE = { id: "thing", entry: ENTRY, tree: { "a.py": "1".repeat(64), "registry.json": "2".repeat(64) },
  params: { heat: 1 }, seed: 7, tier: "preview", build: "19.5.435", hoiiotool: "OpenImageIO 2.3.14", threads: 4 };

test("canonical JSON sorts keys at every level and refuses what JSON can't hold", () => {
  assert.equal(canonical({ b: 1, a: { d: [2, { y: 1, x: 0 }], c: null } }), '{"a":{"c":null,"d":[2,{"x":0,"y":1}]},"b":1}');
  for (const bad of [Infinity, NaN, undefined, () => 1, { a: Infinity }]) assert.throws(() => canonical(bad));
});

test("the cache key is stable under key order", () => {
  const reordered = { threads: 4, hoiiotool: BASE.hoiiotool, build: BASE.build, tier: "preview", seed: 7,
    params: { heat: 1 }, tree: { "registry.json": "2".repeat(64), "a.py": "1".repeat(64) },
    entry: Object.fromEntries(Object.entries(ENTRY).reverse()), id: "thing" };
  assert.equal(cacheKey(reordered), cacheKey(BASE));
});

test("every component changes the cache key", () => {
  const base = cacheKey(BASE);
  const variants = {
    id: { id: "other" },
    seed: { seed: 8 },
    tier: { tier: "final" },
    params: { params: { heat: 1.5 } },
    build: { build: "20.0.1" },
    hoiiotool: { hoiiotool: "OpenImageIO 2.4" },
    threads: { threads: 8 },
    "a tree file": { tree: { ...BASE.tree, "a.py": "3".repeat(64) } },
    "a new tree file": { tree: { ...BASE.tree, "b.py": "1".repeat(64) } },
    "the entry version": { entry: { ...ENTRY, version: 2 } },
    "the builder pin": { entry: { ...ENTRY, builderSha256: "c".repeat(64) } },
    "the renderer": { entry: { ...ENTRY, renderer: "mantra" } },
    "the grid": { entry: { ...ENTRY, grid: 4 } },
    "a parameter bound": { entry: { ...ENTRY, params: { heat: { ...ENTRY.params.heat, max: 3 } } } },
    "a tier setting": { entry: { ...ENTRY, tiers: { preview: { ...ENTRY.tiers.preview, cell: 64 } } } },
    "the deadline": { entry: { ...ENTRY, tiers: { preview: { ...ENTRY.tiers.preview, timeoutMin: 6 } } } },
  };
  const seen = new Set([base]);
  for (const [what, change] of Object.entries(variants)) {
    const key = cacheKey({ ...BASE, ...change });
    assert.ok(!seen.has(key), `${what} did not change the key`);
    seen.add(key);
  }
});

test("treeHashes covers every file under scripts/houdini, nested ones included", () => {
  const tree = treeHashes(fileURLToPath(new URL(".", import.meta.url)));
  for (const name of ["registry.json", "generate.mjs", "launcher.ps1", "hython/run_generator.py", "generators/debug_atlas.py"])
    assert.match(tree[name] ?? "", /^[0-9a-f]{64}$/, name);
});

function writeFrames(dir, count, { skip = -1 } = {}) {
  mkdirSync(join(dir, "frames"), { recursive: true });
  const frames = [];
  for (let index = 0; index < count; index++) {
    const name = `frames/frame_${String(index + 1).padStart(4, "0")}.exr`;
    const bytes = Buffer.from(`frame ${index}`);
    if (index !== skip) writeFileSync(join(dir, ...name.split("/")), bytes);
    frames.push({ index, name, sha256: sha256(bytes) });
  }
  return JSON.stringify({ schemaVersion: 1, frames });
}

test("the manifest must list exactly grid*grid present, unaltered frames in order", (t) => {
  const dir = scratch(t);
  const text = writeFrames(dir, 4);
  assert.equal(checkManifest(text, 4, dir).frames.length, 4);
  const validator = (error) => error.exitCode === 4;
  assert.throws(() => checkManifest(text, 9, dir), validator);
  assert.throws(() => checkManifest("{", 4, dir), validator);
  const manifest = JSON.parse(text);
  const swapped = structuredClone(manifest);
  [swapped.frames[0].name, swapped.frames[1].name] = [swapped.frames[1].name, swapped.frames[0].name];
  assert.throws(() => checkManifest(JSON.stringify(swapped), 4, dir), /misnamed/);
  const escaping = structuredClone(manifest);
  escaping.frames[0].name = "../frames/frame_0001.exr";
  assert.throws(() => checkManifest(JSON.stringify(escaping), 4, dir), /misnamed/);
  writeFileSync(join(dir, "frames", "frame_0002.exr"), "changed");
  assert.throws(() => checkManifest(text, 4, dir), /frame 1 differs/);
  writeFrames(dir, 4);
  writeFileSync(join(dir, "frames", "extra.exr"), "x");
  assert.throws(() => checkManifest(text, 4, dir), /unlisted files: extra.exr/);
  const missing = scratch(t);
  assert.throws(() => checkManifest(writeFrames(missing, 4, { skip: 3 }), 4, missing), /frame 3 is missing/);
});

const provenance = (overrides = {}) => buildProvenance({ id: "thing", entry: ENTRY, key: "k".repeat(64), params: { heat: 1 },
  seed: 7, tier: "preview", build: "19.5.435", hoiiotool: "OpenImageIO 2.3.14", threads: 4, manifestSha256: "m".repeat(64), ...overrides });

test("provenance carries every spec §21.6 field and nothing about the machine", () => {
  const record = provenance();
  // §21.6: generator ID, version, HDA version, seed, parameters, output profile,
  // content hash, Particle Editor validation, game verification, effects using it.
  for (const field of ["generatorId", "generatorVersion", "hdaVersion", "seed", "parameters", "outputProfile",
    "contentHash", "particleEditorValidation", "gameVerification", "usedBy"])
    assert.ok(Object.hasOwn(record, field), field);
  assert.equal(record.hdaVersion, ENTRY.builderSha256);
  assert.deepEqual(record.usedBy, []);
  assert.equal(record.particleEditorValidation, "not-run");
  const text = serialise(record);
  assert.doesNotMatch(text, /licen[cs]e|host|user|time|date|[A-Za-z]:\\|\/Users\//i);
  assert.equal(serialise(provenance()), text, "provenance is deterministic");
});

test("a cache hit is trusted only when every file re-hashes to its provenance", (t) => {
  const dir = scratch(t);
  const text = writeFrames(dir, 4);
  writeFileSync(join(dir, "frames.manifest.json"), text);
  const key = "k".repeat(64);
  writeFileSync(join(dir, "provenance.json"), serialise(provenance({ key, manifestSha256: sha256(text) })));
  assert.equal(verifyEntry(dir, key, 4).contentHash, sha256(text));
  assert.throws(() => verifyEntry(dir, "x".repeat(64), 4), /another key/);
  writeFileSync(join(dir, "frames", "frame_0003.exr"), "poisoned");
  assert.throws(() => verifyEntry(dir, key, 4), /frame 2 differs/);
  writeFileSync(join(dir, "frames.manifest.json"), `${text} `);
  assert.throws(() => verifyEntry(dir, key, 4), /manifest differs/);
  rmSync(join(dir, "provenance.json"));
  assert.throws(() => verifyEntry(dir, key, 4), /no readable provenance/);
});

test("eviction drops least recently used entries until under the cap, never a published one", (t) => {
  const cache = scratch(t);
  // Entries are named by their 64-hex key; the label keeps the test readable.
  const key = (label) => sha256(Buffer.from(label));
  const make = (label, bytes, age, { published = false, provenance = true, name = key(label) } = {}) => {
    mkdirSync(join(cache, name));
    writeFileSync(join(cache, name, "data"), Buffer.alloc(bytes));
    if (provenance) writeFileSync(join(cache, name, "provenance.json"), "{}");
    if (published) writeFileSync(join(cache, name, "published"), "");
    const at = new Date(Date.now() - age * 60000);
    utimesSync(join(cache, name), at, at);
  };
  make("oldest-published", 100, 50, { published: true });
  make("old", 100, 40);
  make("middle", 100, 30);
  make("current", 100, 1);
  make("new", 100, 0);
  assert.deepEqual(evict(cache, 450), [key("old")]);
  assert.deepEqual(evict(cache, 250, { keep: key("current") }), [key("middle"), key("new")]);
  assert.ok(existsSync(join(cache, key("oldest-published"))) && existsSync(join(cache, key("current"))));
  // Over the cap with only protected entries left: nothing else to drop.
  assert.deepEqual(evict(cache, 0, { keep: key("current") }), []);
  assert.deepEqual(evict(join(cache, "missing"), 0), []);
});

test("eviction never counts or removes what the wrapper did not name", (t) => {
  const cache = scratch(t);
  const foreign = join(cache, "http");
  mkdirSync(foreign);
  writeFileSync(join(foreign, "big"), Buffer.alloc(1000));
  const unproven = join(cache, "a".repeat(64));
  mkdirSync(unproven);
  writeFileSync(join(unproven, "data"), Buffer.alloc(1000));
  const target = scratch(t);
  writeFileSync(join(target, "provenance.json"), "{}");
  writeFileSync(join(target, "keep-me"), Buffer.alloc(1000));
  const linked = join(cache, "b".repeat(64));
  if (process.platform === "win32") symlinkSync(target, linked, "junction");
  assert.deepEqual(evict(cache, 0), []);
  assert.ok(existsSync(join(foreign, "big")) && existsSync(join(unproven, "data")) && existsSync(join(target, "keep-me")));
});

test("the registry pins each builder to its exact bytes", () => {
  const registry = JSON.parse(readFileSync(new URL("./registry.json", import.meta.url), "utf8"));
  for (const [id, entry] of Object.entries(registry.generators)) {
    const bytes = readFileSync(new URL(`./${entry.builder}`, import.meta.url));
    assert.equal(sha256(bytes), entry.builderSha256, `${id}: re-pin builderSha256 after editing ${entry.builder}`);
    assert.ok(!bytes.includes(13), `${id}: ${entry.builder} must be LF-only (see .gitattributes)`);
  }
});
