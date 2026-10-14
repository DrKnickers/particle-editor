// [houdini] Input rules (plan §3.3). Everything a caller supplies is checked
// here before any process starts; run_generator.py checks the job again.
//
// Ids are looked up with Object.hasOwn, so "__proto__" and "constructor" are
// unknown ids rather than inherited members. Parameters are bounded numbers
// or enum members; nothing a caller types reaches a Houdini string parameter.
import * as fs from "node:fs";
import { win32 } from "node:path";
import { BLENDS } from "./profiles.mjs";

export const ID = /^[a-z0-9-]{1,64}$/;
export const TIERS = ["preview", "validate", "final"];
export const RENDERERS = ["cop", "mantra", "karma"];
export const MAX_SEED = 2 ** 31 - 1;
const BUILDER = /^generators\/[a-z0-9_]{1,64}\.py$/;
const HEX64 = /^[0-9a-f]{64}$/;
// Drive-rooted, and no character Houdini, HScript, cmd or hoiiotool expands:
// no backtick, $, %, braces, #, quotes or wildcards.
const ROOT = /^[A-Za-z]:\\[A-Za-z0-9 _.()\\-]*$/;
const DEVICE = /^(con|prn|aux|nul|com[0-9]|lpt[0-9])(\..*)?$/i;

export const fail = (message, exitCode = 2) => Object.assign(new Error(message), { exitCode });
const check = (condition, message) => { if (!condition) throw fail(message); };
const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const exactKeys = (value, keys, what) => {
  check(isObject(value), `${what} is not an object`);
  const extra = Object.keys(value).filter((key) => !keys.includes(key));
  check(!extra.length, `${what} has unknown fields: ${extra.join(", ")}`);
  const missing = keys.filter((key) => !Object.hasOwn(value, key));
  check(!missing.length, `${what} is missing fields: ${missing.join(", ")}`);
};
const integer = (value, min, max) => Number.isSafeInteger(value) && value >= min && value <= max;

function checkSchema(name, rule) {
  const what = `parameter ${name}`;
  check(isObject(rule), `${what} has no rule`);
  if (rule.type === "enum") {
    exactKeys(rule, ["type", "values", "default"], what);
    check(Array.isArray(rule.values) && rule.values.length > 0 &&
      rule.values.every((v) => typeof v === "string" && /^[a-z0-9-]{1,32}$/.test(v)), `${what} has bad values`);
    check(rule.values.includes(rule.default), `${what} default is not one of its values`);
  } else {
    check(rule.type === "float" || rule.type === "int", `${what} has an unknown type`);
    exactKeys(rule, ["type", "min", "max", "default"], what);
    check([rule.min, rule.max, rule.default].every(Number.isFinite) && rule.min <= rule.max &&
      rule.min <= rule.default && rule.default <= rule.max, `${what} has bad bounds`);
    if (rule.type === "int") check([rule.min, rule.max, rule.default].every(Number.isSafeInteger), `${what} bounds are not integers`);
  }
}

function checkEntry(id, entry) {
  const what = `generator ${id}`;
  check(ID.test(id), `${what} id is malformed`);
  // pe_gen_<id>_<8 hex>.tga must fit the editor fixture's 64-byte texture names.
  check(id.length <= 40, `${what} id is longer than 40 characters`);
  const keys = ["version", "builder", "builderSha256", "renderer", "blend", "grid", "params", "tiers"];
  exactKeys(entry, Object.hasOwn(entry ?? {}, "testOnly") ? [...keys, "testOnly"] : keys, what);
  check(integer(entry.version, 1, 1e6), `${what} version is not a positive integer`);
  check(typeof entry.builder === "string" && BUILDER.test(entry.builder), `${what} builder path is malformed`);
  check(typeof entry.builderSha256 === "string" && HEX64.test(entry.builderSha256), `${what} builderSha256 is malformed`);
  check(RENDERERS.includes(entry.renderer), `${what} renderer is unknown`);
  check(BLENDS.includes(entry.blend), `${what} blend is not one of ${BLENDS.join(", ")}`);
  check([1, 2, 4, 8].includes(entry.grid), `${what} grid is not 1, 2, 4 or 8`);
  check(entry.testOnly === undefined || typeof entry.testOnly === "boolean", `${what} testOnly is not a boolean`);
  check(isObject(entry.params), `${what} params is not an object`);
  for (const [name, rule] of Object.entries(entry.params)) {
    check(/^[a-z][a-zA-Z0-9]{0,31}$/.test(name), `${what} parameter name is malformed: ${name}`);
    checkSchema(name, rule);
  }
  exactKeys(entry.tiers, TIERS, `${what} tiers`);
  for (const tier of TIERS) {
    const t = entry.tiers[tier];
    exactKeys(t, ["cell", "timeoutMin", "maxStageMiB"], `${what} tier ${tier}`);
    check(integer(t.cell, 8, 4096) && (t.cell & (t.cell - 1)) === 0, `${what} tier ${tier} cell is not a power of two`);
    check(integer(t.timeoutMin, 1, 240), `${what} tier ${tier} timeoutMin is out of range`);
    check(integer(t.maxStageMiB, 1, 65536), `${what} tier ${tier} maxStageMiB is out of range`);
  }
}

export function parseRegistry(bytes) {
  let registry;
  try { registry = JSON.parse(Buffer.from(bytes).toString("utf8")); }
  catch (error) { throw fail(`registry is not JSON: ${error.message}`, 1); }
  try {
    exactKeys(registry, ["schemaVersion", "generators"], "registry");
    check(registry.schemaVersion === 1, "registry schema is unsupported");
    check(isObject(registry.generators), "registry generators is not an object");
    for (const [id, entry] of Object.entries(registry.generators)) checkEntry(id, entry);
  } catch (error) { error.exitCode = 1; throw error; }
  return registry;
}

export function lookup(registry, id) {
  check(typeof id === "string" && ID.test(id), `generator id is malformed: ${JSON.stringify(id)}`);
  check(Object.hasOwn(registry.generators, id), `unknown generator: ${id}`);
  return registry.generators[id];
}

// Same rules as run_generator.check_params, so the two can't disagree.
export function checkParams(params, schema) {
  check(isObject(params), "params is not an object");
  const extra = Object.keys(params).filter((name) => !Object.hasOwn(schema, name));
  check(!extra.length, `unknown parameters: ${extra.join(", ")}`);
  const checked = {};
  for (const name of Object.keys(schema).sort()) {
    const rule = schema[name];
    check(Object.hasOwn(params, name), `parameter ${name} is missing`);
    const value = params[name];
    if (rule.type === "enum") {
      check(typeof value === "string" && rule.values.includes(value), `parameter ${name} is not one of its values`);
    } else {
      check(typeof value === "number", `parameter ${name} is not a number`);
      check(Number.isFinite(value), `parameter ${name} is not finite`);
      check(value >= rule.min && value <= rule.max, `parameter ${name} is out of range`);
      if (rule.type === "int") check(Number.isInteger(value), `parameter ${name} is not an integer`);
    }
    checked[name] = value;
  }
  return checked;
}

// Defaults for every parameter: what a caller gets without --params.
export const defaultParams = (schema) =>
  Object.fromEntries(Object.keys(schema).sort().map((name) => [name, schema[name].default]));

export function parseSeed(text) {
  check(typeof text === "string" && /^[0-9]{1,10}$/.test(text), `seed is not 0..${MAX_SEED}: ${JSON.stringify(text)}`);
  const seed = Number(text);
  check(seed <= MAX_SEED, `seed is not 0..${MAX_SEED}: ${text}`);
  return seed;
}

export function checkTier(tier) {
  check(TIERS.includes(tier), `tier is not one of ${TIERS.join(", ")}: ${JSON.stringify(tier)}`);
  return tier;
}

const lstatOrNull = (io, path) => { try { return io.lstatSync(path); } catch { return null; } };
const within = (base, path) => {
  const b = base.replace(/\\+$/, "").toLowerCase(), p = path.toLowerCase();
  return p === b || p.startsWith(`${b}\\`);
};

const GAME_EXES = ["swfoc.exe", "sweaw.exe", "starwarsg.exe"];
const listOrEmpty = (io, path) => { try { return io.readdirSync(path).map((name) => name.toLowerCase()); } catch { return []; } };
// A game install, found by what it holds rather than by the editor's settings:
// a game exe, or a Data folder of .meg archives (a mod folder has one too).
const looksLikeGame = (io, at) => listOrEmpty(io, at).some((name) => GAME_EXES.includes(name)) ||
  listOrEmpty(io, win32.join(at, "Data")).some((name) => name.endsWith(".meg"));

// The generated root: absolute, plain characters, outside the repo, every game
// install, Steam's folders and OneDrive, and reached through no link, junction
// or other reparse point (decision 5, §24.1). It may not exist yet.
// `repoRoot` may list several roots: a worktree and the checkout it belongs to.
export function checkRoot(root, { repoRoot, gameRoots = [], env = process.env, io = fs } = {}) {
  check(typeof root === "string" && /^[A-Za-z]:[\\/]/.test(root), `--out must be an absolute drive path: ${JSON.stringify(root)}`);
  const path = win32.resolve(root).replace(/\\+$/, "");
  check(ROOT.test(path), `--out may hold only letters, digits, space and _ . ( ) - : ${path}`);
  check(path.split("\\").slice(1).every((part) => part && !/[. ]$/.test(part) && !DEVICE.test(part)),
    `--out has a reserved or trailing-dot name: ${path}`);
  const forbidden = [
    ...[].concat(repoRoot ?? []).map((repo) => ["the repository", repo]),
    ...gameRoots.map((game) => ["a game install", game]),
    ...["OneDrive", "OneDriveConsumer", "OneDriveCommercial"].map((name) => ["OneDrive", env[name]]),
  ];
  check(!path.split("\\").some((part) => /^onedrive\b/i.test(part)), `--out is inside OneDrive: ${path}`);
  check(!path.split("\\").some((part) => /^steamapps$/i.test(part)), `--out is inside a game install (steamapps): ${path}`);
  // Every existing ancestor must be a plain folder whose real path is itself.
  let existing = null;
  for (let at = path; ; at = win32.dirname(at)) {
    const stat = lstatOrNull(io, at);
    if (stat) {
      existing ??= at;
      check(!stat.isSymbolicLink(), `--out runs through a link or junction: ${at}`);
      check(stat.isDirectory(), `--out runs through a file: ${at}`);
      check(!looksLikeGame(io, at), `--out is inside a game install or mod folder: ${at}`);
    }
    if (win32.dirname(at) === at) break;
  }
  check(existing, `--out is on no existing drive: ${path}`);
  const real = io.realpathSync.native(existing);
  check(real.toLowerCase() === existing.toLowerCase(), `--out runs through a reparse point: ${existing} is ${real}`);
  for (const [what, base] of forbidden) {
    if (!base) continue;
    const resolved = win32.resolve(base);
    let baseReal = resolved;
    try { baseReal = io.realpathSync.native(resolved); } catch { /* not there: compare as written */ }
    check(!within(resolved, path) && !within(baseReal, path), `--out is inside ${what}: ${path}`);
  }
  return path;
}
