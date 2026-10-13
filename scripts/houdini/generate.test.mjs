// [houdini] The generate pipeline with hython, the lock and the pins faked:
// staging, promotion, the cache, every exit code and the audit log.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { HOUDINI_DIR, generate, hythonFailure, parseArgs } from "./generate.mjs";
import { PINNED } from "./houdini.mjs";
import { ROOT_MARKER } from "./generate.mjs";
import { sha256 } from "./cache.mjs";

const windows = { skip: process.platform !== "win32" && "generated roots are Windows drive paths" };
const ARGS = { generator: "debug-atlas", seed: "7", tier: "preview" };

// Writes what run_generator.py would: frames, then a manifest, then the report line.
function writeOutput(cwd, count = 16) {
  mkdirSync(join(cwd, "frames"));
  const frames = [];
  for (let index = 0; index < count; index++) {
    const name = `frames/frame_${String(index + 1).padStart(4, "0")}.exr`;
    const bytes = Buffer.from(`colour ${index}`);
    writeFileSync(join(cwd, ...name.split("/")), bytes);
    frames.push({ index, name, sha256: sha256(bytes) });
  }
  writeFileSync(join(cwd, "frames.manifest.json"), JSON.stringify({ schemaVersion: 1, frames }));
}

function harness(t, overrides = {}) {
  const root = join(mkdtempSync(join(tmpdir(), "pe-houdini-gen-")), "generated");
  t.after(() => rmSync(join(root, ".."), { recursive: true, force: true }));
  const calls = { run: [], lock: 0, released: 0, verify: 0, logs: [] };
  const behave = overrides.behave ?? ((s) => { writeOutput(s.cwd); writeFileSync(s.stdout, '[houdini-run] {"ok": true, "frames": 16}\n'); return { outcome: "exited", code: 0, pid: 42 }; });
  const seams = {
    gameRoots: () => [], repos: () => [join(HOUDINI_DIR, "..", "..")], staleWaitMs: 0,
    verify: () => { calls.verify++; },
    lock: async () => { calls.lock++; return overrides.busy ? null : { release: async () => { calls.released++; } }; },
    run: async (s, options) => { calls.run.push({ s, options }); options.onLaunched(42); return behave(s, options); },
    log: (text) => calls.logs.push(text), clock: () => "2026-10-13T00:00:00.000Z",
    ...overrides.seams,
  };
  return { root, calls, run: (args = {}) => generate({ ...ARGS, out: root, ...args }, seams) };
}
const audit = (root) => readFileSync(join(root, "audit.log"), "utf8").trim().split("\n").map((line) => JSON.parse(line));

test("a miss runs hython contained, promotes the checked entry, and the next run is a verified hit", windows, async (t) => {
  const h = harness(t);
  const first = await h.run();
  assert.equal(first.exitCode, 0, first.error);
  assert.equal(first.hit, false);
  assert.equal(h.calls.run.length, 1);
  const { s, options } = h.calls.run[0];
  assert.equal(s.exe, PINNED.hython.path);
  assert.deepEqual(s.args, [join(HOUDINI_DIR, "hython", "run_generator.py")]);
  // The allowlist itself is pinned in houdini.test.mjs; here, that it is rooted in this stage.
  assert.deepEqual(Object.keys(s.env).sort(), ["HOME", "HOUDINI_MAXTHREADS", "HOUDINI_NO_ENV_FILE", "HOUDINI_PACKAGE_SKIP",
    "HOUDINI_TEMP_DIR", "HOUDINI_USER_PREF_DIR", "NUMBER_OF_PROCESSORS", "PATH", "PYTHONDONTWRITEBYTECODE",
    "PYTHONNOUSERSITE", "SystemDrive", "SystemRoot", "TEMP", "TMP"]);
  for (const name of ["TEMP", "TMP", "HOUDINI_TEMP_DIR", "HOME"]) assert.equal(s.env[name], s.cwd, name);
  assert.equal(s.env.HOUDINI_USER_PREF_DIR, join(s.cwd, "prefs", "houdini__HVER__"));
  assert.match(s.cwd, /\\\.staging\\[0-9a-f]{16}-[0-9a-f]{8}\\work$/);
  assert.equal(options.timeoutMs, 5 * 60000);
  const job = JSON.parse(s.input);
  assert.deepEqual(Object.keys(job).sort(), ["id", "params", "registrySha256", "schemaVersion", "seed", "threads", "tier"]);
  assert.equal(job.registrySha256, sha256(readFileSync(join(HOUDINI_DIR, "registry.json"))));
  assert.deepEqual([job.id, job.seed, job.tier, job.threads], ["debug-atlas", 7, "preview", 4]);

  const entry = join(h.root, "cache", first.cacheKey);
  assert.equal(first.entry, entry);
  assert.deepEqual(readdirSync(entry).sort(), ["frames", "frames.manifest.json", "provenance.json"]);
  assert.equal(readdirSync(join(entry, "frames")).length, 16);
  const provenance = JSON.parse(readFileSync(join(entry, "provenance.json"), "utf8"));
  assert.equal(provenance.contentHash, sha256(readFileSync(join(entry, "frames.manifest.json"))));
  assert.deepEqual(readdirSync(join(h.root, ".staging")), [], "the stage was removed");
  assert.deepEqual([h.calls.lock, h.calls.released, h.calls.verify], [1, 1, 1]);
  assert.deepEqual(h.calls.logs, ["[houdini-gen] launched 42"]);

  const second = await h.run();
  assert.deepEqual([second.exitCode, second.hit, second.cacheKey, second.contentHash],
    [0, true, first.cacheKey, first.contentHash]);
  assert.equal(h.calls.run.length, 1, "a hit does not run hython");
  assert.deepEqual(audit(h.root).map((v) => [v.exitCode, v.hit]), [[0, false], [0, true]]);
  assert.equal(audit(h.root)[0].time, "2026-10-13T00:00:00.000Z");
});

test("a different seed, tier or parameter set is a different key", windows, async (t) => {
  const h = harness(t);
  const keys = new Set();
  for (const args of [{}, { seed: "8" }, { tier: "validate" }]) keys.add((await h.run(args)).cacheKey);
  assert.equal(keys.size, 3);
});

test("a cache entry that fails its re-hash is discarded and regenerated", windows, async (t) => {
  const h = harness(t);
  const first = await h.run();
  writeFileSync(join(first.entry, "frames", "frame_0003.exr"), "poisoned");
  const second = await h.run();
  assert.deepEqual([second.exitCode, second.hit, second.discarded], [0, false, true]);
  assert.equal(h.calls.run.length, 2);
  assert.match(h.calls.logs.join("\n"), /discarding a cache entry.*frame 2 differs/);
});

test("stages left by a dead run are swept once the lock is held", windows, async (t) => {
  const h = harness(t);
  assert.equal((await h.run()).exitCode, 0);
  const stale = join(h.root, ".staging", "0123456789abcdef-deadbeef");
  mkdirSync(join(stale, "work"), { recursive: true });
  writeFileSync(join(stale, "work", "half.exr"), "x");
  // Anything not named like a stage is not the wrapper's to delete.
  const foreign = join(h.root, ".staging", "notes");
  writeFileSync(foreign, "keep");
  assert.equal((await h.run({ seed: "8" })).exitCode, 0);
  assert.ok(!existsSync(stale));
  assert.ok(existsSync(foreign));
});

test("a stale stage still held by a dying run is exit 7, and the lock is released", windows, async (t) => {
  const stale = "0123456789abcdef-deadbeef";
  const io = { ...fs, rmSync: (path, options) => {
    if (path.endsWith(stale)) throw Object.assign(new Error("EBUSY: resource busy"), { code: "EBUSY" });
    return fs.rmSync(path, options);
  } };
  const h = harness(t, { seams: { io } });
  assert.equal((await h.run()).exitCode, 0);
  mkdirSync(join(h.root, ".staging", stale));
  const result = await h.run({ seed: "8" });
  assert.equal(result.exitCode, 7);
  assert.match(result.error, /previous run's stage is still in use \(EBUSY\)/);
  assert.equal(h.calls.run.length, 1, "no second tree was started");
  assert.equal(h.calls.released, h.calls.lock);
});

test("a stage that can't be removed still releases the lock and keeps the real outcome", windows, async (t) => {
  const io = { ...fs, rmSync: (path, options) => {
    if (/\\\.staging\\[0-9a-f]{16}-[0-9a-f]{8}$/.test(path)) throw Object.assign(new Error("EBUSY"), { code: "EBUSY" });
    return fs.rmSync(path, options);
  } };
  const h = harness(t, { seams: { io }, behave: () => ({ outcome: "deadline", code: null, pid: 42 }) });
  const result = await h.run();
  assert.equal(result.exitCode, 5, result.error);
  assert.equal(h.calls.released, 1);
  assert.match(h.calls.logs.join("\n"), /could not remove the stage/);
});

test("the root must be one the wrapper made: new, empty, or marked", windows, async (t) => {
  const h = harness(t);
  mkdirSync(join(h.root, "cache"), { recursive: true });
  writeFileSync(join(h.root, "cache", "someone-elses"), "x");
  const refused = await h.run();
  assert.equal(refused.exitCode, 2);
  assert.match(refused.error, /already holds files the generator did not make/);
  assert.equal(h.calls.lock, 0);
  assert.ok(existsSync(join(h.root, "cache", "someone-elses")));
  rmSync(join(h.root, "cache"), { recursive: true });
  assert.equal((await h.run()).exitCode, 0);
  assert.ok(existsSync(join(h.root, ROOT_MARKER)), "an empty root is claimed with a marker");
  assert.equal((await h.run({ seed: "8" })).exitCode, 0, "a marked root is reused");
});

test("a pinned binary that doesn't verify prevents the spawn", windows, async (t) => {
  const h = harness(t, { seams: { verify: () => { throw Object.assign(new Error("hython differs from the pinned build"), { exitCode: 1 }); } } });
  const result = await h.run();
  assert.equal(result.exitCode, 1);
  assert.match(result.error, /differs from the pinned/);
  assert.equal(h.calls.run.length, 0);
  assert.equal(h.calls.released, 1);
});

test("hython exiting 0 without a manifest is a validator failure (exit 4)", windows, async (t) => {
  const h = harness(t, { behave: () => ({ outcome: "exited", code: 0, pid: 42 }) });
  const result = await h.run();
  assert.equal(result.exitCode, 4);
  assert.match(result.error, /no frames.manifest.json/);
});

test("a held machine lock is exit 7 and nothing runs", windows, async (t) => {
  const h = harness(t, { busy: true });
  const result = await h.run();
  assert.equal(result.exitCode, 7);
  assert.equal(h.calls.run.length, 0);
  assert.equal(audit(h.root)[0].exitCode, 7);
});

const failing = [
  ["a deadline", { outcome: "deadline", code: null }, 5, /deadline of 5 min passed/],
  ["the stage quota", { outcome: "stopped", code: null, detail: "stage exceeded 64 MiB" }, 6, /stage exceeded 64 MiB/],
  ["a launcher failure", { outcome: "failed", code: null, detail: "boom" }, 1, /launcher failed: boom/],
  // A failure worded without "licence" may still name a server: hython's text is printed, never stored.
  ["a generator crash", { outcome: "exited", code: 1, stderr: "Traceback: hou.OperationFailed: lic-host.example:1715" }, 8,
    /^the generator failed \(hython exit 1\)$/],
  ["a job run_generator rejects", { outcome: "exited", code: 2, stdout: '[houdini-run] {"ok": false, "rejected": "seed is out of range"}\n' }, 2, /rejected the job: seed is out of range/],
  ["a licence failure", { outcome: "exited", code: 1, stderr: "No licenses could be found. Server: lic-host.example:1715" }, 3, /^hython could not get a licence$/],
];
for (const [what, outcome, exitCode, pattern] of failing) {
  test(`${what} is exit ${exitCode}, leaves no stage and no cache entry`, windows, async (t) => {
    const h = harness(t, { behave: (s) => {
      writeOutput(s.cwd);
      writeFileSync(s.stdout, outcome.stdout ?? ""); writeFileSync(s.stderr, outcome.stderr ?? "");
      return { pid: 42, ...outcome };
    } });
    const result = await h.run();
    assert.equal(result.exitCode, exitCode);
    assert.match(result.error, pattern);
    assert.deepEqual(readdirSync(join(h.root, ".staging")), []);
    assert.ok(!existsSync(join(h.root, "cache", result.cacheKey)));
    assert.equal(h.calls.released, 1, "the lock was released");
    assert.equal(audit(h.root)[0].exitCode, exitCode);
    assert.doesNotMatch(readFileSync(join(h.root, "audit.log"), "utf8"), /lic-host/);
    assert.doesNotMatch(JSON.stringify(result), /lic-host/);
    if (exitCode === 8) assert.match(h.calls.logs.join("\n"), /printed, not stored[\s\S]*OperationFailed/);
    if (exitCode === 3) assert.doesNotMatch(h.calls.logs.join("\n"), /lic-host/);
  });
}

test("frames that don't match the manifest are a validator failure (exit 4)", windows, async (t) => {
  for (const damage of [
    (cwd) => rmSync(join(cwd, "frames", "frame_0016.exr")),
    (cwd) => writeFileSync(join(cwd, "frames", "frame_0001.exr"), "altered"),
    (cwd) => writeFileSync(join(cwd, "frames", "stray.exr"), "x"),
    (cwd) => writeFileSync(join(cwd, "frames.manifest.json"), "{}"),
  ]) {
    const h = harness(t, { behave: (s) => { writeOutput(s.cwd); damage(s.cwd); return { outcome: "exited", code: 0, pid: 42 }; } });
    const result = await h.run();
    assert.equal(result.exitCode, 4, result.error);
    assert.ok(!existsSync(join(h.root, "cache", result.cacheKey)));
  }
});

test("too little free space is exit 6 before anything runs", windows, async (t) => {
  const h = harness(t, { seams: { io: { ...fs, statfsSync: () => ({ bavail: 10, bsize: 4096 }) } } });
  const result = await h.run();
  assert.equal(result.exitCode, 6);
  assert.match(result.error, /less than 64 MiB free/);
  assert.equal(h.calls.run.length, 0);
});

test("rejected inputs are exit 2 and never take the lock", windows, async (t) => {
  const h = harness(t);
  const params = join(h.root, "..", "p.json");
  writeFileSync(params, JSON.stringify({ extra: 1 }));
  for (const [args, reason] of [
    [{ generator: "__proto__" }, /id is malformed/], [{ generator: "constructor" }, /unknown generator/],
    [{ seed: "1e400" }, /seed/], [{ seed: "2147483648" }, /seed/], [{ tier: "ultra" }, /tier/],
    [{ params }, /unknown parameters: extra/], [{ params: join(h.root, "..") }, /--params is not a file/],
    [{ out: join(HOUDINI_DIR, "out") }, /inside the repository/], [{ out: "C:\\gen\\a$b" }, /may hold only/],
  ]) {
    const result = await h.run(args);
    assert.equal(result.exitCode, 2, `${JSON.stringify(args)}: ${result.error}`);
    assert.match(result.error, reason, JSON.stringify(args));
  }
  assert.equal(h.calls.lock, 0);
  assert.ok(!existsSync(h.root), "nothing was created for a rejected input");
});

test("a builder that no longer matches its pin fails before any spawn", windows, async (t) => {
  const copy = mkdtempSync(join(tmpdir(), "pe-houdini-pin-"));
  t.after(() => rmSync(copy, { recursive: true, force: true }));
  cpSync(HOUDINI_DIR, copy, { recursive: true });
  writeFileSync(join(copy, "generators", "debug_atlas.py"), "def generate(hou, ctx):\n    return []\n");
  const h = harness(t, { seams: { houdiniDir: copy } });
  const result = await h.run();
  assert.equal(result.exitCode, 1);
  assert.match(result.error, /does not match its pinned builderSha256/);
  assert.equal(h.calls.lock, 0);
});

test("hythonFailure never repeats licence text", () => {
  const error = hythonFailure(1, "", "license server lic-host.example refused checkout");
  assert.equal(error.exitCode, 3);
  assert.doesNotMatch(error.message, /lic-host/);
  assert.equal(hythonFailure(2, "garbage", "").exitCode, 8, "exit 2 without a report is not a rejection");
});

test("parseArgs needs --generator, --seed and --tier, once each", () => {
  assert.deepEqual(parseArgs(["--generator", "debug-atlas", "--seed", "7", "--tier", "preview", "--out", "C:\\gen"]),
    { generator: "debug-atlas", seed: "7", tier: "preview", out: "C:\\gen" });
  for (const argv of [[], ["--generator", "a", "--seed", "1"], ["--generator", "a", "--seed", "1", "--tier", "preview", "--seed", "2"],
    ["--generator", "a", "--seed", "1", "--tier", "preview", "--exe", "x"], ["--generator"],
    ["__proto__", "x", "--generator", "a", "--seed", "1", "--tier", "preview"], ["constructor", "x"]])
    assert.throws(() => parseArgs(argv), (error) => error.exitCode === 2);
});
