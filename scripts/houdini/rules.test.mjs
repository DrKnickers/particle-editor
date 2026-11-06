// [houdini] Input rules: every class of hostile or malformed input is refused
// with exit 2.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { mkdtempSync, mkdirSync, rmSync, symlinkSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { checkParams, checkRoot, checkSourceRoot, checkTier, defaultParams, lookup, parseRegistry, parseSeed, readRegistry } from "./rules.mjs";
import { sha256 } from "./cache.mjs";

const SCHEMA = {
  heat: { type: "float", min: 0.5, max: 2, default: 1 },
  count: { type: "int", min: 1, max: 8, default: 4 },
  palette: { type: "enum", values: ["orange", "blue"], default: "orange" },
};
const TIER = { cell: 32, timeoutMin: 5, maxStageMiB: 64 };
const entry = (overrides = {}) => ({
  version: 1, builder: "generators/thing.py", builderSha256: "a".repeat(64), renderer: "cop", blend: "additive", grid: 4,
  params: SCHEMA, tiers: { preview: TIER, validate: TIER, final: TIER }, ...overrides,
});
const registryBytes = (generators) => Buffer.from(JSON.stringify({ schemaVersion: 1, generators }));
const rejects = (fn, pattern, exitCode = 2) =>
  assert.throws(fn, (error) => error.exitCode === exitCode && (!pattern || pattern.test(error.message)));

test("the shipped registry parses", () => {
  const registry = parseRegistry(fs.readFileSync(new URL("./registry.json", import.meta.url)));
  assert.equal(lookup(registry, "debug-atlas").grid, 4);
});

test("the registry reads its main file alone, or with the private file beside it", (t) => {
  const dir = mkdtempSync(join(tmpdir(), "pe-registry-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const main = registryBytes({ "thing": entry() }), extra = registryBytes({ "other": entry({ grid: 2 }) });
  fs.writeFileSync(join(dir, "registry.json"), main);
  const alone = readRegistry(dir);
  assert.deepEqual([Object.keys(alone.registry.generators), alone.digest], [["thing"], sha256(main)]);
  fs.writeFileSync(join(dir, "registry-private.json"), extra);
  const both = readRegistry(dir);
  assert.deepEqual(Object.keys(both.registry.generators), ["thing", "other"]);
  assert.equal(lookup(both.registry, "other").grid, 2);
  assert.equal(both.digest, sha256(sha256(main) + sha256(extra)));
  fs.writeFileSync(join(dir, "registry-private.json"), registryBytes({ "thing": entry() }));
  rejects(() => readRegistry(dir), /both registry files/, 1);
  fs.writeFileSync(join(dir, "registry-private.json"), "{");
  rejects(() => readRegistry(dir), /not JSON/, 1);
});

test("unknown, prototype, drive and uppercase ids are rejected", () => {
  const registry = parseRegistry(registryBytes({ "thing": entry() }));
  assert.equal(lookup(registry, "thing").version, 1);
  for (const id of ["nope", "__proto__", "constructor", "toString", "D:evil", "Thing", "", "a".repeat(65), 7, null])
    rejects(() => lookup(registry, id));
  // 40 characters is the longest id whose texture name fits 64 bytes.
  assert.equal(lookup(parseRegistry(registryBytes({ ["a".repeat(40)]: entry() })), "a".repeat(40)).grid, 4);
});

test("a malformed registry is a failure (exit 1), not a rejected input", () => {
  for (const bad of [
    { "x": entry({ builder: "../evil.py" }) },
    { "x": entry({ builder: "generators/D:evil.py" }) },
    { "x": entry({ builderSha256: "zz" }) },
    { "x": entry({ grid: 3 }) },
    { "x": entry({ renderer: "cmd" }) },
    { "x": entry({ blend: "multiply" }) },
    { "x": entry({ blend: undefined }) },
    { ["a".repeat(41)]: entry() },
    { "x": entry({ extra: 1 }) },
    { "x": entry({ tiers: { preview: TIER, validate: TIER } }) },
    { "x": entry({ tiers: { preview: { ...TIER, cell: 48 }, validate: TIER, final: TIER } }) },
    { "x": entry({ params: { p: { type: "string", default: "x" } } }) },
    { "x": entry({ params: { p: { type: "float", min: 2, max: 1, default: 1 } } }) },
    { "x": entry({ params: { p: { type: "enum", values: ["$HOME"], default: "$HOME" } } }) },
    { "Bad": entry() },
    { "x": entry({ builder: "generators/con.py" }) },
    { "x": entry({ builder: "generators/NUL.py" }) },
    { "x": entry({ modules: { com1: "a".repeat(64) } }) },
  ]) rejects(() => parseRegistry(registryBytes(bad)), null, 1);
  rejects(() => parseRegistry(Buffer.from("{")), /not JSON/, 1);
});

test("format, shared modules and cell seconds: optional, and checked when present", () => {
  const seconds = Array.from({ length: 16 }, (_, i) => i * 0.05);
  const good = { a: entry({ format: "dxt1" }), b: entry({ format: "dxt5", blend: "transparent" }), c: entry({ format: "tga" }),
    d: entry({ modules: { explosion_sim: "c".repeat(64) }, cellSeconds: seconds }) };
  assert.deepEqual(Object.keys(parseRegistry(registryBytes(good)).generators), ["a", "b", "c", "d"]);
  for (const [bad, pattern] of [
    [entry({ format: "png" }), /format is not one of tga, dxt1, dxt5/],
    [entry({ format: "dxt1", blend: "transparent" }), /DXT1 but not additive/],
    [entry({ modules: {} }), /modules is not a non-empty object/],
    [entry({ modules: ["x"] }), /modules is not a non-empty object/],
    [entry({ modules: { "../evil": "c".repeat(64) } }), /module name is malformed/],
    [entry({ modules: { Sim: "c".repeat(64) } }), /module name is malformed/],
    [entry({ modules: { thing: "c".repeat(64) } }), /module name is malformed: thing/],
    [entry({ modules: { sim: "zz" } }), /module sim hash is malformed/],
    [entry({ cellSeconds: seconds.slice(1) }), /one time per cell/],
    [entry({ cellSeconds: [...seconds.slice(0, 15), 0.7] }), /not increasing/],
    [entry({ cellSeconds: [-0.1, ...seconds.slice(1)] }), /not increasing/],
    [entry({ cellSeconds: [...seconds.slice(0, 15), Infinity] }), /not increasing/],
  ]) rejects(() => parseRegistry(registryBytes({ x: bad })), pattern, 1);
});

test("parameters: defaults pass; unknown, extra, out-of-range, non-finite and non-enum are rejected", () => {
  assert.deepEqual(checkParams(defaultParams(SCHEMA), SCHEMA), { count: 4, heat: 1, palette: "orange" });
  const ok = defaultParams(SCHEMA);
  for (const [params, pattern] of [
    [{ ...ok, extra: 1 }, /unknown parameters: extra/],
    [{ heat: 1, count: 4 }, /palette is missing/],
    [{ ...ok, heat: 2.5 }, /out of range/],
    [{ ...ok, heat: Infinity }, /not finite/],
    [{ ...ok, heat: NaN }, /not finite/],
    [{ ...ok, heat: "1" }, /not a number/],
    [{ ...ok, heat: true }, /not a number/],
    [{ ...ok, count: 2.5 }, /not an integer/],
    [{ ...ok, palette: "red" }, /not one of its values/],
    [{ ...ok, palette: "`whoami`" }, /not one of its values/],
    [[], /not an object/],
    [null, /not an object/],
  ]) rejects(() => checkParams(params, SCHEMA), pattern);
  // JSON can't carry 1e400 as a number; it parses to Infinity.
  rejects(() => checkParams({ ...ok, heat: JSON.parse("1e400") }, SCHEMA), /not finite/);
  rejects(() => checkParams(JSON.parse('{"__proto__": {"heat": 1}}'), SCHEMA), /unknown parameters: __proto__/);
});

test("seeds are 0..2^31-1 in plain digits", () => {
  assert.equal(parseSeed("0"), 0);
  assert.equal(parseSeed("2147483647"), 2147483647);
  for (const seed of ["2147483648", "9999999999", "1e400", "-1", "1.5", "0x10", " 7", "", "12345678901", 7])
    rejects(() => parseSeed(seed), /seed/);
});

test("tiers are preview, validate or final", () => {
  assert.equal(checkTier("validate"), "validate");
  for (const tier of ["Final", "ultra", "", "__proto__", undefined]) rejects(() => checkTier(tier), /tier/);
});

// A fake filesystem: `dirs` maps a lower-case path to its kind.
// `contents` maps a lower-case folder to the names readdirSync lists.
function fakeIo(dirs, real = {}, contents = {}) {
  const at = (path) => dirs[path.toLowerCase()];
  return {
    readdirSync(path) {
      if (!at(path) && !contents[path.toLowerCase()]) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" });
      return contents[path.toLowerCase()] ?? [];
    },
    lstatSync(path) {
      const kind = at(path);
      if (!kind) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" });
      return { isSymbolicLink: () => kind === "link", isDirectory: () => kind !== "file" };
    },
    realpathSync: { native: (path) => {
      if (!at(path)) throw Object.assign(new Error("ENOENT"), { code: "ENOENT" });
      return real[path.toLowerCase()] ?? path;
    } },
  };
}
const DIRS = { "c:\\": "dir", "c:\\gen": "dir", "c:\\repo": "dir", "d:\\": "dir", "d:\\game": "dir" };
const options = (overrides = {}) => ({
  repoRoot: "C:\\repo", gameRoots: ["D:\\game\\GameData"], env: { OneDrive: "C:\\Users\\me\\OneDrive" },
  io: fakeIo(DIRS), ...overrides,
});

test("roots: absolute, plain characters, may not exist yet", () => {
  assert.equal(checkRoot("C:\\gen\\out", options()), "C:\\gen\\out");
  assert.equal(checkRoot("C:\\gen\\a b (2)\\out-1.x", options()), "C:\\gen\\a b (2)\\out-1.x");
  assert.equal(checkRoot("C:\\gen\\", options()), "C:\\gen");
  for (const root of ["gen", "\\gen", "\\\\server\\share\\gen", "C:gen", undefined])
    rejects(() => checkRoot(root, options()), /absolute/);
});

test("roots holding a character Houdini, HScript, cmd or hoiiotool expands are rejected", () => {
  for (const ch of ["`", "$", "%", "{", "}", "#", "'", '"', "*", "?", "&", ";", "!", "~", "\u00e9"])
    rejects(() => checkRoot(`C:\\gen\\a${ch}b`, options()), /may hold only/);
  for (const root of ["C:\\gen\\con", "C:\\gen\\NUL.txt", "C:\\gen\\dot."])
    rejects(() => checkRoot(root, options()), /reserved or trailing-dot/);
});

test("roots inside the repo, a game install or OneDrive are rejected", () => {
  rejects(() => checkRoot("C:\\repo", options()), /inside the repository/);
  rejects(() => checkRoot("C:\\Repo\\x", options()), /inside the repository/);
  rejects(() => checkRoot("D:\\game\\GameData\\Data", options()), /inside a game install/);
  rejects(() => checkRoot("C:\\Users\\me\\OneDrive\\gen", options({ io: fakeIo({ ...DIRS, "c:\\users": "dir" }) })), /OneDrive/);
  rejects(() => checkRoot("C:\\Users\\me\\OneDrive - Work\\gen", options()), /OneDrive/);
  // A sibling whose name only starts like the repo is fine.
  assert.equal(checkRoot("C:\\repo2", options()), "C:\\repo2");
});

test("OneDrive is found by its environment variable even under another folder name", () => {
  const cloud = options({ env: { OneDriveCommercial: "C:\\Users\\me\\Cloud" } });
  rejects(() => checkRoot("C:\\Users\\me\\Cloud\\gen", cloud), /inside OneDrive/);
  assert.equal(checkRoot("C:\\Users\\me\\Cloudy\\gen", cloud), "C:\\Users\\me\\Cloudy\\gen");
});

test("a game install or mod folder is found by its contents, without the editor's settings", () => {
  const dirs = { ...DIRS, "c:\\games": "dir", "c:\\games\\foc": "dir", "c:\\mods": "dir", "c:\\mods\\m": "dir" };
  const io = fakeIo(dirs, {}, { "c:\\games\\foc": ["swfoc.exe", "Data"], "c:\\mods\\m\\data": ["Config.meg"] });
  const bare = options({ gameRoots: [], io });
  rejects(() => checkRoot("C:\\games\\foc\\gen", bare), /game install or mod folder: C:\\games\\foc$/);
  rejects(() => checkRoot("C:\\mods\\m\\Data\\Art\\gen", bare), /game install or mod folder: C:\\mods\\m$/);
  assert.equal(checkRoot("C:\\games\\gen", bare), "C:\\games\\gen");
  rejects(() => checkRoot("D:\\Steam\\steamapps\\workshop\\content\\32470\\gen", bare), /steamapps/);
});

test("every repository root given is refused: the worktree and its checkout", () => {
  const both = options({ repoRoot: ["C:\\main\\worktrees\\wt", "C:\\main"] });
  rejects(() => checkRoot("C:\\main\\gen", both), /inside the repository/);
  rejects(() => checkRoot("C:\\main\\worktrees\\wt\\gen", both), /inside the repository/);
});

test("roots reached through a link, a file or another reparse point are rejected", () => {
  rejects(() => checkRoot("C:\\gen\\out", options({ io: fakeIo({ ...DIRS, "c:\\gen": "link" }) })), /link or junction/);
  rejects(() => checkRoot("C:\\gen\\out", options({ io: fakeIo({ ...DIRS, "c:\\gen": "file" }) })), /through a file/);
  rejects(() => checkRoot("C:\\gen\\out", options({ io: fakeIo(DIRS, { "c:\\gen": "E:\\elsewhere" }) })), /reparse point/);
  // A junction elsewhere that lands in the repo is caught by its real path too.
  rejects(() => checkRoot("C:\\gen\\out", options({ io: fakeIo(DIRS, { "c:\\gen": "C:\\repo\\gen" }) })), /reparse point/);
});

test("a real junction in the root's path is rejected", { skip: process.platform !== "win32" && "Windows junctions" }, () => {
  const base = mkdtempSync(join(tmpdir(), "pe-houdini-rules-"));
  try {
    mkdirSync(join(base, "target"));
    symlinkSync(join(base, "target"), join(base, "junction"), "junction");
    const real = { repoRoot: "C:\\no-repo-here", gameRoots: [], env: {} };
    assert.equal(checkRoot(join(base, "target", "out"), real), join(base, "target", "out"));
    rejects(() => checkRoot(join(base, "junction", "out"), real), /link or junction/);
  } finally { rmSync(base, { recursive: true, force: true }); }
});

// A generators root (--generators-root): the same shape rules as --out, and it must already be
// a plain folder reached through no link or reparse point.
test("a generators root is a plain existing folder with a plain path", () => {
  const io = fakeIo({ ...DIRS, "c:\\gen\\src": "dir", "c:\\gen\\file": "file", "c:\\gen\\link": "link" });
  const source = (root, more = {}) => checkSourceRoot(root, { env: {}, io, ...more });
  assert.equal(source("C:\\gen\\src"), "C:\\gen\\src");
  assert.equal(source("C:\\gen\\src\\"), "C:\\gen\\src");
  for (const root of ["houdini", "\\\\server\\share\\gen", "\\\\?\\C:\\gen\\src", "C:gen", undefined])
    rejects(() => source(root), /--generators-root must be an absolute drive path/);
  rejects(() => source("C:\\gen\\a:stream"), /may hold only/);
  rejects(() => source("C:\\gen\\con"), /reserved or trailing-dot/);
  rejects(() => source("C:\\gen\\missing"), /--generators-root is not a folder/);
  rejects(() => source("C:\\gen\\file"), /through a file/);
  rejects(() => source("C:\\gen\\link"), /--generators-root runs through a link or junction/);
  rejects(() => source("C:\\Users\\me\\OneDrive\\gen"), /OneDrive/);
  const moved = fakeIo({ ...DIRS, "c:\\gen\\src": "dir" }, { "c:\\gen\\src": "E:\\else" });
  rejects(() => source("C:\\gen\\src", { io: moved }), /reparse point/);
});

test("--out and a generators root may not sit inside each other, whatever the case or dots", () => {
  const nested = options({ sourceRoots: ["C:\\gen\\src", "C:\\other"] });
  rejects(() => checkRoot("C:\\GEN\\SRC\\out", nested), /--out is inside the generators root/);
  rejects(() => checkRoot("C:\\gen\\src", nested), /--out is inside the generators root/);
  rejects(() => checkRoot("C:\\gen\\x\\..\\src\\out", nested), /--out is inside the generators root/);
  rejects(() => checkRoot("C:\\other\\out", nested), /--out is inside the generators root/);
  rejects(() => checkRoot("C:\\gen", nested), /--out holds the generators root/);
  rejects(() => checkRoot("C:\\Gen\\", nested), /--out holds the generators root/);
  assert.equal(checkRoot("C:\\gen\\src2", nested), "C:\\gen\\src2");
  assert.equal(checkRoot("C:\\gen\\src\\..\\out", nested), "C:\\gen\\out");
});
