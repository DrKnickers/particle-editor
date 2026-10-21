// [houdini] The generate pipeline with hython, the lock and the pins faked:
// staging, promotion, the cache, every exit code and the audit log.
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { cpSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { HOUDINI_DIR, generate, hythonFailure, parseArgs } from "./generate.mjs";
import { PINNED, TEXCONV } from "./houdini.mjs";
import { ROOT_MARKER } from "./generate.mjs";
import { sha256 } from "./cache.mjs";
import { readRegistry } from "./rules.mjs";
import { decodeTga } from "./images.mjs";
import { texconvArgs } from "./profiles.mjs";
import { encodeDds, frameStatsText, syntheticAtlas } from "./test-atlas.mjs";

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

// hoiiotool for the debug-atlas preview tier (grid 4, cell 32, 28-texel
// frames): the stats it prints, then the packed and encoded files it writes.
// `texture` replaces the encoded bytes; `stats` the printed stats.
const ATLAS = syntheticAtlas({ grid: 4, cell: 32 });
function fakeTool(calls, { stats = frameStatsText(16, 28), texture = ATLAS.tga(), code = 0, timedOut = false } = {}) {
  return (args, options) => {
    calls.tool.push({ args, options });
    if (timedOut) return { code: null, stdout: "", stderr: "", error: "spawnSync hoiiotool.exe ETIMEDOUT", timedOut };
    if (code !== 0) return { code, stdout: "", stderr: "hoiiotool ERROR: could not open", error: null };
    if (args.includes("--stats")) return { code: 0, stdout: stats, stderr: "", error: null };
    writeFileSync(join(options.cwd, args.at(-1)), args.at(-1) === "packed.exr" ? "packed" : texture);
    return { code: 0, stdout: "", stderr: "", error: null };
  };
}

// texconv: the DDS of whatever TGA hoiiotool left, or `dds(tga)` instead;
// `write: false` is texconv exiting 0 without writing, as it does.
function fakeCompress(calls, { code = 0, write = true, dds = null } = {}) {
  return (args, options) => {
    calls.compress.push({ args, options });
    if (code !== 0) return { code, stdout: "", stderr: "texconv: failed", error: null };
    if (write) {
      const tga = readFileSync(join(options.cwd, args.at(-1))), image = decodeTga(tga);
      writeFileSync(join(options.cwd, "texture.dds"), dds ? dds(image) : encodeDds(image.width, image.height, image.rgba, args[2]));
    }
    return { code: 0, stdout: "writing .\\texture.dds", stderr: "", error: null };
  };
}

function harness(t, overrides = {}) {
  const root = join(mkdtempSync(join(tmpdir(), "pe-houdini-gen-")), "generated");
  t.after(() => rmSync(join(root, ".."), { recursive: true, force: true }));
  const calls = { run: [], tool: [], compress: [], lock: 0, released: 0, verify: 0, verifyCompressor: 0, logs: [] };
  const behave = overrides.behave ?? ((s) => { writeOutput(s.cwd); writeFileSync(s.stdout, '[houdini-run] {"ok": true, "frames": 16}\n'); return { outcome: "exited", code: 0, pid: 42 }; });
  const seams = {
    gameRoots: () => [], repos: () => [join(HOUDINI_DIR, "..", "..")], staleWaitMs: 0,
    verify: () => { calls.verify++; },
    lock: async () => { calls.lock++; return overrides.busy ? null : { release: async () => { calls.released++; } }; },
    run: async (s, options) => { calls.run.push({ s, options }); options.onLaunched(42); return behave(s, options); },
    tool: fakeTool(calls, overrides.tool),
    compress: fakeCompress(calls, overrides.compress), verifyCompressor: () => { calls.verifyCompressor++; },
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
  assert.equal(job.registrySha256, readRegistry(HOUDINI_DIR).digest);
  assert.deepEqual([job.id, job.seed, job.tier, job.threads], ["debug-atlas", 7, "preview", 4]);

  // hoiiotool: stats, pack, encode, each in the stage with the allowlisted environment.
  assert.deepEqual(h.calls.tool.map((c) => c.args.at(-1)), ["frames/frame_0016.exr", "packed.exr", "texture.tga"]);
  for (const { args, options } of h.calls.tool) {
    assert.deepEqual(args.slice(0, 2), ["--evaloff", "--wildcardoff"]);
    assert.equal(options.cwd, s.cwd);
    assert.deepEqual(Object.keys(options.env).sort(), Object.keys(s.env).sort());
  }

  const entry = join(h.root, "cache", first.cacheKey);
  const textureBytes = ATLAS.tga(), name = `pe_gen_debug-atlas_${sha256(textureBytes).slice(0, 8)}.tga`;
  assert.equal(first.entry, entry);
  assert.deepEqual([first.texture, first.profile, first.contentHash], [name, "eaw-flipbook", sha256(textureBytes)]);
  assert.deepEqual(readdirSync(entry).sort(), ["frames", "frames.manifest.json", name, "provenance.json", "published"]);
  assert.equal(readdirSync(join(entry, "frames")).length, 16);
  const provenance = JSON.parse(readFileSync(join(entry, "provenance.json"), "utf8"));
  assert.equal(provenance.contentHash, sha256(textureBytes));
  assert.equal(provenance.framesManifestSha256, sha256(readFileSync(join(entry, "frames.manifest.json"))));
  assert.deepEqual(provenance.texture, { name, format: "tga", blend: "additive", grid: 4, cell: 32, pad: 2, size: 128,
    bytes: textureBytes.length });
  assert.equal(provenance.texconv, null);
  // The cell steps of the synthetic atlas: red +12 and green -8 a cell, blue still: 20/3 levels.
  assert.deepEqual([provenance.cellSteps.steps.length, provenance.cellSteps.mean, provenance.cellSteps.max], [15, 6.67, 6.67]);
  assert.equal(provenance.cellSpan, null, "a builder that returns names alone records no spans");
  // Published where a content layer keeps textures, byte for byte.
  const published = join(h.root, "Data", "Art", "Textures", name);
  assert.equal(first.published, published);
  assert.deepEqual(readFileSync(published), textureBytes);
  assert.deepEqual(readdirSync(join(h.root, "Data", "Art", "Textures")), [name]);
  assert.deepEqual(readdirSync(join(h.root, ".staging")), [], "the stage was removed");
  assert.deepEqual([h.calls.lock, h.calls.released, h.calls.verify], [1, 1, 1]);
  assert.deepEqual([h.calls.compress.length, h.calls.verifyCompressor], [0, 0], "a TGA entry never runs texconv");
  assert.deepEqual(h.calls.logs, ["[houdini-gen] launched 42"]);

  // A hit republishes a missing texture without running anything.
  rmSync(published);
  const second = await h.run();
  assert.deepEqual([second.exitCode, second.hit, second.cacheKey, second.contentHash, second.texture],
    [0, true, first.cacheKey, first.contentHash, name]);
  assert.deepEqual(readFileSync(published), textureBytes);
  assert.equal(h.calls.run.length, 1, "a hit does not run hython");
  assert.equal(h.calls.tool.length, 3, "a hit does not run hoiiotool");
  // A torn or altered copy under the name is replaced, not trusted.
  writeFileSync(published, "torn");
  assert.equal((await h.run()).exitCode, 0);
  assert.deepEqual(readFileSync(published), textureBytes);
  assert.deepEqual(readdirSync(join(h.root, ".staging")), [], "the publish left no temporary file");
  assert.deepEqual(audit(h.root).map((v) => [v.exitCode, v.hit]), [[0, false], [0, true], [0, true]]);
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

test("the profile step fails closed: bad frames, a hoiiotool failure, a texture off its profile (exit 4)", windows, async (t) => {
  const offProfile = syntheticAtlas({ grid: 4, cell: 32 });
  offProfile.rgba[0] = 200; // one guard-band texel no longer black
  for (const [tool, pattern] of [
    [{ stats: frameStatsText(16, 28, { nan: 3 }) }, /frames failed the profile: .*NaN/],
    [{ stats: frameStatsText(16, 32) }, /frames failed the profile: .*32x32, not 28x28/],
    [{ stats: frameStatsText(16, 28, { channels: 3 }) }, /frames failed the profile: .*3 channels/],
    [{ stats: frameStatsText(15, 28) }, /frames failed the profile/],
    [{ code: 1 }, /^hoiiotool could not read the frames \(exit 1\)$/],
    [{ texture: Buffer.from("not a tga") }, /texture is not a TGA/],
    [{ texture: offProfile.tga() }, /texture failed the eaw-flipbook profile: 1 guard-band texels are not the black border/],
  ]) {
    const h = harness(t, { tool });
    const result = await h.run();
    assert.equal(result.exitCode, 4, `${pattern}: ${result.error}`);
    assert.match(result.error, pattern);
    // hoiiotool's own words are printed for debugging, never stored.
    if (tool.code) assert.match(h.calls.logs.join("\n"), /hoiiotool output \(printed, not stored\):\nhoiiotool ERROR/);
    assert.doesNotMatch(readFileSync(join(h.root, "audit.log"), "utf8"), /hoiiotool ERROR/);
    assert.ok(!existsSync(join(h.root, "cache", result.cacheKey)));
    assert.ok(!existsSync(join(h.root, "Data")), "nothing was published");
    assert.deepEqual(readdirSync(join(h.root, ".staging")), []);
  }
});

test("a DDS entry: texconv in the stage, the DDS validated, cached, published and named for its bytes", windows, async (t) => {
  const h = harness(t);
  const first = await h.run({ generator: "debug-atlas-dds" });
  assert.equal(first.exitCode, 0, first.error);
  assert.equal(h.calls.verifyCompressor, 1);
  assert.equal(h.calls.compress.length, 1);
  const { args, options } = h.calls.compress[0];
  assert.deepEqual(args, texconvArgs("dxt5", "texture.tga"));
  assert.equal(options.cwd, h.calls.run[0].s.cwd);
  assert.deepEqual(Object.keys(options.env).sort(), Object.keys(h.calls.run[0].s.env).sort());
  const dds = encodeDds(128, 128, ATLAS.rgba, "DXT5"), name = `pe_gen_debug-atlas-dds_${sha256(dds).slice(0, 8)}.dds`;
  assert.deepEqual([first.texture, first.contentHash], [name, sha256(dds)]);
  const entry = join(h.root, "cache", first.cacheKey);
  assert.deepEqual(readdirSync(entry).sort(), ["frames", "frames.manifest.json", name, "provenance.json", "published"]);
  const provenance = JSON.parse(readFileSync(join(entry, "provenance.json"), "utf8"));
  assert.deepEqual(provenance.texture, { name, format: "dxt5", blend: "additive", grid: 4, cell: 32, pad: 2, size: 128,
    bytes: dds.length });
  assert.deepEqual(provenance.texconv, { texconv: TEXCONV.texconv.sha256, d3dx9: TEXCONV.d3dx9.sha256 });
  assert.deepEqual(readFileSync(join(h.root, "Data", "Art", "Textures", name)), dds);
  // A hit runs neither hython nor texconv.
  const second = await h.run({ generator: "debug-atlas-dds" });
  assert.deepEqual([second.exitCode, second.hit, second.texture], [0, true, name]);
  assert.deepEqual([h.calls.run.length, h.calls.compress.length], [1, 1]);
  // The TGA entry with the same builder is another key.
  assert.notEqual((await h.run()).cacheKey, first.cacheKey);
});

test("the DDS step fails closed: an unpinned texconv, no file, a failed run, a DDS off its TGA", windows, async (t) => {
  const pinFailure = harness(t, { seams: { verifyCompressor: () => { throw Object.assign(new Error("texconv differs from the pinned DirectX SDK (June 2010) build"), { exitCode: 1 }); } } });
  const unpinned = await pinFailure.run({ generator: "debug-atlas-dds" });
  assert.deepEqual([unpinned.exitCode, pinFailure.calls.run.length], [1, 0]);
  assert.match(unpinned.error, /texconv differs from the pinned/);
  for (const [compress, pattern] of [
    [{ write: false }, /^texconv wrote no DXT5 file$/],
    [{ code: 1 }, /^texconv could not write DXT5 \(exit 1\)$/],
    [{ dds: (image) => encodeDds(image.width, image.height, image.rgba, "DXT1") }, /not a DXT5 DDS \(it is DXT1\)/],
    [{ dds: (image) => encodeDds(image.width, image.height, image.rgba, "DXT5", { mips: 1 }) }, /1 mip levels, not the full chain of 8/],
    [{ dds: (image) => encodeDds(image.width, image.height, Buffer.alloc(image.rgba.length, 255), "DXT5") }, /colour differs from the TGA/],
  ]) {
    const h = harness(t, { compress });
    const result = await h.run({ generator: "debug-atlas-dds" });
    assert.equal(result.exitCode, 4, `${pattern}: ${result.error}`);
    assert.match(result.error, pattern);
    if (compress.code) assert.match(h.calls.logs.join("\n"), /texconv output \(printed, not stored\):\ntexconv: failed/);
    assert.ok(!existsSync(join(h.root, "cache", result.cacheKey)));
    assert.ok(!existsSync(join(h.root, "Data")), "nothing was published");
  }
});

test("hoiiotool shares the tier's deadline: running past it is exit 5", windows, async (t) => {
  const h = harness(t, { tool: { timedOut: true } });
  const result = await h.run();
  assert.equal(result.exitCode, 5, result.error);
  assert.match(result.error, /preview deadline of 5 min passed while hoiiotool ran/);
  assert.ok(h.calls.tool[0].options.timeoutMs <= 5 * 60000 && h.calls.tool[0].options.timeoutMs > 0);
  assert.ok(!existsSync(join(h.root, "cache", result.cacheKey)));
  // Past the deadline already, hoiiotool isn't started at all.
  let clock = 0;
  const late = harness(t, { seams: { now: () => clock }, behave: (s) => {
    writeOutput(s.cwd); clock += 5 * 60000; return { outcome: "exited", code: 0, pid: 42 };
  } });
  assert.equal((await late.run()).exitCode, 5);
  assert.equal(late.calls.tool.length, 0);
});

test("a link planted under the published name is replaced, never read through", windows, async (t) => {
  let planted = false;
  const renames = [];
  const io = { ...fs,
    lstatSync: (path, options) => (planted && /\\Textures\\pe_gen_/.test(path) ? { isSymbolicLink: () => true, isDirectory: () => false } : fs.lstatSync(path, options)),
    renameSync: (from, to) => { renames.push(to); return fs.renameSync(from, to); } };
  const h = harness(t, { seams: { io } });
  const first = await h.run();
  assert.equal(first.exitCode, 0, first.error);
  planted = true;
  renames.length = 0;
  assert.equal((await h.run()).exitCode, 0);
  assert.deepEqual(renames, [first.published], "the hit rewrote the name rather than trusting the link");
});

test("a root whose Data folder the wrapper did not make is refused", windows, async (t) => {
  const h = harness(t);
  mkdirSync(join(h.root, "Data", "Art"), { recursive: true });
  const refused = await h.run();
  assert.equal(refused.exitCode, 2);
  assert.match(refused.error, /already holds files the generator did not make/);
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

test("a generator in the private registry runs, and the job carries both files' digest", windows, async (t) => {
  const copy = mkdtempSync(join(tmpdir(), "pe-houdini-private-"));
  t.after(() => rmSync(copy, { recursive: true, force: true }));
  cpSync(HOUDINI_DIR, copy, { recursive: true });
  rmSync(join(copy, "registry-private.json"), { force: true });
  const main = readFileSync(join(copy, "registry.json"));
  const extra = Buffer.from(JSON.stringify({ schemaVersion: 1,
    generators: { "private-atlas": JSON.parse(main).generators["debug-atlas"] } }));
  writeFileSync(join(copy, "registry-private.json"), extra);
  const h = harness(t, { seams: { houdiniDir: copy } });
  const result = await h.run({ generator: "private-atlas" });
  assert.equal(result.exitCode, 0, result.error);
  const job = JSON.parse(h.calls.run[0].s.input);
  assert.equal(job.id, "private-atlas");
  assert.equal(job.registrySha256, sha256(sha256(main) + sha256(extra)));
});

test("hythonFailure never repeats licence text", () => {
  const error = hythonFailure(1, "", "license server lic-host.example refused checkout");
  assert.equal(error.exitCode, 3);
  assert.doesNotMatch(error.message, /lic-host/);
  assert.equal(hythonFailure(2, "garbage", "").exitCode, 8, "exit 2 without a report is not a rejection");
});

// Another repository's generators root: debug_atlas.py and its entry, copied verbatim.
function otherRoot(t) {
  const root = mkdtempSync(join(tmpdir(), "pe-houdini-other-"));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  mkdirSync(join(root, "generators"));
  cpSync(join(HOUDINI_DIR, "generators", "debug_atlas.py"), join(root, "generators", "debug_atlas.py"));
  const entry = JSON.parse(readFileSync(join(HOUDINI_DIR, "registry.json"))).generators["debug-atlas"];
  writeFileSync(join(root, "registry.json"), JSON.stringify({ schemaVersion: 1, generators: { "debug-atlas": entry } }));
  return root;
}
// What hython would find in <stage>\src when the fake run starts.
const snapshotOf = (s) => {
  const src = join(s.cwd, "..", "src");
  return Object.fromEntries(readdirSync(src, { recursive: true }).filter((name) => !fs.statSync(join(src, name)).isDirectory())
    .map((name) => [name.replaceAll("\\", "/"), readFileSync(join(src, name))]));
};

test("hython runs from a snapshot of exactly the files Node checked, beside its working folder", windows, async (t) => {
  let seen = null;
  const h = harness(t, { behave: (s) => { seen = snapshotOf(s); writeOutput(s.cwd); return { outcome: "exited", code: 0, pid: 42 }; } });
  assert.equal((await h.run()).exitCode, 0);
  const expected = { "registry.json": readFileSync(join(HOUDINI_DIR, "registry.json")),
    "generators/debug_atlas.py": readFileSync(join(HOUDINI_DIR, "generators", "debug_atlas.py")) };
  if (existsSync(join(HOUDINI_DIR, "registry-private.json")))
    expected["registry-private.json"] = readFileSync(join(HOUDINI_DIR, "registry-private.json"));
  assert.deepEqual(seen, expected);
});

test("--generators-root runs another root's generator under the same key as the same bytes here", windows, async (t) => {
  const root = otherRoot(t);
  let seen = null;
  const h = harness(t, { behave: (s) => { seen = snapshotOf(s); writeOutput(s.cwd); return { outcome: "exited", code: 0, pid: 42 }; } });
  const other = await h.run({ generatorsRoot: root });
  assert.equal(other.exitCode, 0, other.error);
  assert.deepEqual(Object.keys(seen).sort(), ["generators/debug_atlas.py", "registry.json"]);
  assert.deepEqual(seen["registry.json"], readFileSync(join(root, "registry.json")));
  assert.equal(JSON.parse(h.calls.run[0].s.input).registrySha256, sha256(readFileSync(join(root, "registry.json"))));
  const home = await harness(t).run();
  assert.equal(other.cacheKey, home.cacheKey, "the key holds the pinned bytes, not the root's path");
});

test("a file changed after Node read it never reaches hython", windows, async (t) => {
  const root = otherRoot(t), builder = join(root, "generators", "debug_atlas.py");
  const checked = readFileSync(builder);
  const io = { ...fs, readFileSync: (path, ...rest) => {
    const bytes = fs.readFileSync(path, ...rest);
    if (String(path).toLowerCase() === builder.toLowerCase()) fs.writeFileSync(builder, "import os\nos.system('calc')\n");
    return bytes;
  } };
  let seen = null;
  const h = harness(t, { seams: { io }, behave: (s) => { seen = snapshotOf(s); writeOutput(s.cwd); return { outcome: "exited", code: 0, pid: 42 }; } });
  assert.equal((await h.run({ generatorsRoot: root })).exitCode, 0);
  assert.deepEqual(seen["generators/debug_atlas.py"], checked);
});

test("--out and the generators root may not sit inside each other", windows, async (t) => {
  const root = otherRoot(t);
  for (const out of [join(root, "out"), join(root, "..")]) {
    const h = harness(t);
    const result = await h.run({ generatorsRoot: root, out });
    assert.equal(result.exitCode, 2, out);
    assert.match(result.error, /generators root/);
    assert.equal(h.calls.lock, 0);
  }
  const h = harness(t);
  const relative = await h.run({ generatorsRoot: "houdini" });
  assert.equal(relative.exitCode, 2);
  assert.match(relative.error, /--generators-root must be an absolute drive path/);
});

test("parseArgs needs --generator, --seed and --tier, once each", () => {
  assert.deepEqual(parseArgs(["--generator", "debug-atlas", "--seed", "7", "--tier", "preview", "--out", "C:\\gen"]),
    { generator: "debug-atlas", seed: "7", tier: "preview", out: "C:\\gen" });
  assert.deepEqual(parseArgs(["--generator", "a", "--seed", "1", "--tier", "final", "--generators-root", "C:\\fe\\houdini"]),
    { generator: "a", seed: "1", tier: "final", generatorsRoot: "C:\\fe\\houdini" });
  for (const argv of [[], ["--generator", "a", "--seed", "1"], ["--generator", "a", "--seed", "1", "--tier", "preview", "--seed", "2"],
    ["--generator", "a", "--seed", "1", "--tier", "preview", "--exe", "x"], ["--generator"],
    ["__proto__", "x", "--generator", "a", "--seed", "1", "--tier", "preview"], ["constructor", "x"]])
    assert.throws(() => parseArgs(argv), (error) => error.exitCode === 2);
});
