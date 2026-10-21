// [houdini] A generators root read for one run: the registry read once, every
// pin checked in Node, and links, junctions and reparse points refused.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { sha256 } from "./cache.mjs";
import { readRegistry } from "./rules.mjs";
import { readSource } from "./source.mjs";

const windows = { skip: process.platform !== "win32" && "generators roots are Windows drive paths" };
const here = fileURLToPath(new URL(".", import.meta.url)).replace(/\\$/, "");
const ATLAS = readFileSync(join(here, "generators", "debug_atlas.py"));
const TEMPLATE = JSON.parse(readFileSync(join(here, "registry.json"))).generators["debug-atlas"];
const MODULE = Buffer.from("def burst():\n    return 3\n");
const rejects = (fn, pattern, exitCode) =>
  assert.throws(fn, (error) => error.exitCode === exitCode && pattern.test(error.message), String(pattern));

// A root holding debug_atlas.py as `thing`, using one pinned module `sim`.
function makeRoot(t, { entry = {}, files = {} } = {}) {
  const root = mkdtempSync(join(tmpdir(), "pe-houdini-source-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  mkdirSync(join(root, "generators"));
  writeFileSync(join(root, "generators", "thing.py"), ATLAS);
  writeFileSync(join(root, "generators", "sim.py"), MODULE);
  for (const [name, bytes] of Object.entries(files)) writeFileSync(join(root, ...name.split("/")), bytes);
  const thing = { ...TEMPLATE, builder: "generators/thing.py", builderSha256: sha256(ATLAS), modules: { sim: sha256(MODULE) }, ...entry };
  writeFileSync(join(root, "registry.json"), JSON.stringify({ schemaVersion: 1, generators: { thing } }));
  return root;
}

test("the editor's own root reads as readRegistry does, with the builder's bytes", windows, () => {
  const source = readSource(here, "debug-atlas");
  assert.equal(source.digest, readRegistry(here).digest);
  assert.deepEqual(source.entry, readRegistry(here).registry.generators["debug-atlas"]);
  assert.deepEqual(Object.keys(source.files), ["generators/debug_atlas.py"]);
  assert.deepEqual(source.files["generators/debug_atlas.py"], ATLAS);
  assert.deepEqual(Object.keys(source.registryFiles).sort(), fs.existsSync(join(here, "registry-private.json"))
    ? ["registry-private.json", "registry.json"] : ["registry.json"]);
});

test("another root: the builder and every pinned module, hashed in Node", windows, (t) => {
  const root = makeRoot(t);
  const source = readSource(root, "thing");
  assert.deepEqual(Object.keys(source.files).sort(), ["generators/sim.py", "generators/thing.py"]);
  assert.deepEqual(source.files["generators/sim.py"], MODULE);
  assert.equal(source.digest, sha256(readFileSync(join(root, "registry.json"))));
  assert.deepEqual(Object.keys(source.registryFiles), ["registry.json"]);
});

test("a drifted builder or module, or a missing module, fails before anything runs", windows, (t) => {
  rejects(() => readSource(makeRoot(t, { files: { "generators/thing.py": "def generate(hou, ctx):\n    return []\n" } }), "thing"),
    /generators\/thing.py does not match its pinned builderSha256/, 1);
  rejects(() => readSource(makeRoot(t, { files: { "generators/sim.py": "x = 1\n" } }), "thing"),
    /module sim does not match its pinned hash/, 1);
  rejects(() => readSource(makeRoot(t, { entry: { modules: { other: sha256(MODULE) } } }), "thing"),
    /module other is not in generators\//, 1);
  rejects(() => readSource(makeRoot(t), "nope"), /unknown generator: nope/, 2);
});

test("a junction inside the root is refused, even when its bytes match the pins", windows, (t) => {
  const root = makeRoot(t), elsewhere = mkdtempSync(join(tmpdir(), "pe-houdini-elsewhere-"));
  t.after(() => rmSync(elsewhere, { recursive: true, force: true }));
  fs.cpSync(join(root, "generators"), elsewhere, { recursive: true });
  rmSync(join(root, "generators"), { recursive: true });
  symlinkSync(elsewhere, join(root, "generators"), "junction");
  rejects(() => readSource(root, "thing"), /runs through a link or junction: .*\\generators$/, 2);
});

// A file symlink needs Developer Mode to create, so these two go through a stub over the real files.
test("a linked file, or one whose real path is elsewhere, is refused", windows, (t) => {
  const root = makeRoot(t), builder = join(root, "generators", "thing.py").toLowerCase();
  const linked = { ...fs, lstatSync: (path, ...rest) => {
    const stat = fs.lstatSync(path, ...rest);
    return path.toLowerCase() === builder ? Object.assign(Object.create(stat), { isSymbolicLink: () => true }) : stat;
  } };
  rejects(() => readSource(root, "thing", linked), /thing.py runs through a link or junction/, 2);
  const moved = { ...fs, realpathSync: { native: (path) => path.toLowerCase() === builder ? "E:\\else\\thing.py" : fs.realpathSync.native(path) } };
  rejects(() => readSource(root, "thing", moved), /thing.py runs through a reparse point/, 2);
});

test("the registry is read once: its digest, entry and bytes all come from that read", windows, (t) => {
  const root = makeRoot(t), registry = join(root, "registry.json").toLowerCase();
  const first = readFileSync(registry);
  let reads = 0;
  const io = { ...fs, readFileSync: (path, ...rest) => {
    const bytes = fs.readFileSync(path, ...rest);
    if (String(path).toLowerCase() === registry && ++reads === 1)
      writeFileSync(registry, JSON.stringify({ schemaVersion: 1, generators: {} }));
    return bytes;
  } };
  const source = readSource(root, "thing", io);
  assert.equal(reads, 1);
  assert.equal(source.digest, sha256(first));
  assert.deepEqual(source.registryFiles["registry.json"], first);
  assert.equal(source.entry.builder, "generators/thing.py");
});

test("a malformed root is refused before anything is read", windows, () => {
  for (const root of ["houdini", "\\\\server\\share\\houdini", "\\\\?\\C:\\houdini"])
    rejects(() => readSource(root, "thing"), /--generators-root must be an absolute drive path/, 2);
});
