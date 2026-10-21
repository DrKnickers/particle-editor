// [houdini] Read one generator's files from a generators root, once.
//
// A generators root holds registry.json (and optionally registry-private.json)
// and generators/*.py. The default root is this folder; --generators-root
// names another repository's. Everything a run executes is read here, in Node,
// before the cache is consulted: the registry's bytes once (so the entry, the
// digest, the key and the snapshot all describe the same bytes), then the
// builder and each module the entry pins, each hashed against its pin.
// generate.mjs writes exactly these bytes into the stage, and hython runs
// them from there (run_generator.py), never from the root.
//
// Choosing a root runs its Python as you: the pins catch drift, not a hostile
// author. The path checks catch a mistaken root: a link, junction or other
// reparse point anywhere in the root or under it is refused.
import * as fs from "node:fs";
import { createHash } from "node:crypto";
import { join, win32 } from "node:path";
import { PRIVATE_REGISTRY, checkSourceRoot, fail, lookup, registryFrom } from "./rules.mjs";

const hex = (data) => createHash("sha256").update(data).digest("hex");

// A file under the root, reached through plain folders only, whose real path
// is itself; null when it does not exist.
function readPlain(root, relative, io) {
  const parts = relative.split("/");
  let at = root;
  for (const part of parts) {
    at = win32.join(at, part);
    let stat;
    try { stat = io.lstatSync(at); } catch (error) {
      if (error.code === "ENOENT") return null;
      throw error;
    }
    if (stat.isSymbolicLink()) throw fail(`${relative} runs through a link or junction: ${at}`);
    if (at !== win32.join(root, ...parts) && !stat.isDirectory()) throw fail(`${relative} runs through a file: ${at}`);
  }
  const real = io.realpathSync.native(at);
  if (real.toLowerCase() !== at.toLowerCase()) throw fail(`${relative} runs through a reparse point: ${at} is ${real}`);
  return io.readFileSync(at);
}

// { root, registry, entry, digest, registryFiles: {name: bytes}, files: {relative: bytes} }
export function readSource(root, id, io = fs, env = process.env) {
  const path = checkSourceRoot(root, { env, io });
  const main = readPlain(path, "registry.json", io);
  if (main === null) throw fail(`--generators-root has no registry.json: ${path}`);
  const extra = readPlain(path, PRIVATE_REGISTRY, io);
  const { registry, digest } = registryFrom(main, extra);
  const entry = lookup(registry, id);
  const registryFiles = { "registry.json": main, ...(extra === null ? {} : { [PRIVATE_REGISTRY]: extra }) };
  const files = {};
  for (const [name, pin] of Object.entries(entry.modules ?? {}).sort(([a], [b]) => (a < b ? -1 : 1))) {
    const bytes = readPlain(path, `generators/${name}.py`, io);
    if (bytes === null) throw fail(`module ${name} is not in generators/`, 1);
    if (hex(bytes) !== pin) throw fail(`module ${name} does not match its pinned hash`, 1);
    files[`generators/${name}.py`] = bytes;
  }
  const builder = readPlain(path, entry.builder, io);
  if (builder === null || hex(builder) !== entry.builderSha256)
    throw fail(`${entry.builder} does not match its pinned builderSha256`, 1);
  files[entry.builder] = builder;
  return { root: path, registry, entry, digest, registryFiles, files };
}
