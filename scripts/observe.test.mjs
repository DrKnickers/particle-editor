import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { join, resolve } from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { deflateSync } from "node:zlib";
import { checkCapture, buildConcatList, buildFilmstripArgs, buildManifest, checkQuota,
  compareRepeat, spawnWithDeadline, assembleBundle, runObservation, observe, sha256,
  playbackMetrics, checkPlaybackProbe } from "./observe.mjs";
import { PINNED, observationCases, checkCase, measuredValues, generateFixtures, predictedTelemetryRefusal } from "./observe-lane.mjs";
import { runObserveLane, prereq } from "./run-all-tests.mjs";
import { TEXTURES } from "./replay-proof.mjs";

const scratch = resolve("tests/.tmp/observe-unit");
fs.mkdirSync(scratch, { recursive: true });
function temporary(t) {
  const directory = fs.mkdtempSync(join(scratch, "case-"));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}
const json = (value) => JSON.stringify(value, null, 2) + "\n";
function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let i = 0; i < 8; ++i) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const name = Buffer.from(type), length = Buffer.alloc(4), crc = Buffer.alloc(4);
  length.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(Buffer.concat([name, data])));
  return Buffer.concat([length, name, data, crc]);
}
function png(red = 0) {
  const header = Buffer.alloc(13); header.writeUInt32BE(16, 0); header.writeUInt32BE(16, 4); header[8] = 8; header[9] = 6;
  const pixels = Buffer.alloc(16 * 65);
  for (let row = 0; row < 16; ++row) for (let column = 0; column < 16; ++column) {
    const at = row * 65 + 1 + column * 4; pixels[at] = red; pixels[at + 3] = 255;
  }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", header),
    chunk("IDAT", deflateSync(pixels)), chunk("IEND", Buffer.alloc(0))]);
}
function fixture(directory, { finalFrame = 31, strideInterval = 15, playbackStride = 0, zeroStride = false } = {}) {
  fs.mkdirSync(directory, { recursive: true });
  const write = (name, bytes) => { fs.mkdirSync(join(directory, name, ".."), { recursive: true }); fs.writeFileSync(join(directory, name), bytes); };
  const stride = zeroStride ? [] : Array.from({ length: Math.floor(finalFrame / strideInterval) + 1 }, (_, i) => i * strideInterval);
  if (!zeroStride && stride.at(-1) !== finalFrame) stride.push(finalFrame);
  const frames = [...new Set([0, ...stride])].map((frame) => {
    const bytes = png(frame), name = `selected-frames/frame-${String(frame).padStart(5, "0")}.png`;
    write(name, bytes);
    return { frame, reasons: stride.includes(frame) ? ["stride"] : ["frame-zero"], png: name, sha256: sha256(bytes) };
  });
  const playback = playbackStride ? Array.from({ length: Math.floor(finalFrame / playbackStride) + 1 }, (_, index) => {
    write(`playback-frames/frame_${String(index).padStart(5, "0")}.png`, png(index * playbackStride));
    return { index, frame: index * playbackStride };
  }) : [];
  const spec = { schemaVersion: 1, scenario: { maxFrames: finalFrame }, lifecycle: { type: "oneshot", quietTailFrames: 15 },
    output: { strideInterval, playbackStride, maxBytes: 200 * 1024 * 1024 } };
  const lifecycle = { status: "natural-end", finalFrame, reviewWindow: [0, finalFrame], endedBeforeReview: false,
    firstActive: 0, peak: 0, lastActive: Math.max(0, finalFrame - 15),
    limits: { runtimeGuard: { total: 0, firstFrame: null }, hardGuard: { total: 0, firstFrame: null }, indexCap: { total: 0, firstFrame: null } } };
  const capture = { schemaVersion: 1, tool: "test", spec, source: { path: "C:/effect.alo", sha256: "a".repeat(64), bytes: 1 },
    content: { gameRoots: [], selectedModPaths: [], sha256: "b".repeat(64) }, random: { seed: 1, algorithmVersion: 1 },
    profile: {}, geometry: { crop: [0, 0, 16, 16] }, preparation: [], lifecycle, frames, stride, playback };
  write("capture.json", json(capture)); write("telemetry/summary.json", json(lifecycle));
  write("telemetry/frames.jsonl", Array.from({ length: finalFrame + 1 }, (_, frame) => JSON.stringify({ frame, particles: 1, spawnerIdle: true }) + "\n").join(""));
  write("logs/observe.log", "native log with wall timings\n");
  return capture;
}
function mutateCapture(directory, mutate) {
  const path = join(directory, "capture.json"), capture = JSON.parse(fs.readFileSync(path, "utf8"));
  mutate(capture); fs.writeFileSync(path, json(capture));
}

test("capture checker accepts native fields and dense playback map without invented hash fields", (t) => {
  const dir = temporary(t); const capture = fixture(dir, { playbackStride: 3 });
  const checked = checkCapture(dir, { spec: capture.spec });
  assert.deepEqual(checked.errors, []);
  assert.equal(Object.keys(checked.pngHashes).length, capture.frames.length + capture.playback.length);
});
for (const [name, mutation, expected] of [
  ["signature", (dir, c) => { const bytes = fs.readFileSync(join(dir, c.frames[0].png)); bytes[0] ^= 1; fs.writeFileSync(join(dir, c.frames[0].png), bytes); }, /signature/],
  ["hash", (dir) => mutateCapture(dir, (c) => { c.frames[0].sha256 = "0".repeat(64); }), /hash/],
  ["escaping path", (dir) => mutateCapture(dir, (c) => { c.frames[0].png = "../outside.png"; }), /bundle-relative/],
  ["absolute Windows path", (dir) => mutateCapture(dir, (c) => { c.frames[0].png = "C:/outside.png"; }), /bundle-relative/],
  ["missing marker", (dir) => fs.unlinkSync(join(dir, "capture.json")), /marker missing/],
  ["failed status", (dir) => mutateCapture(dir, (c) => { c.lifecycle.status = "failed"; c.failure = { exit: 16 }; }), /failed capture/],
  ["schema", (dir) => mutateCapture(dir, (c) => { c.schemaVersion = 2; }), /schema/],
  ["missing field", (dir) => mutateCapture(dir, (c) => { delete c.geometry; }), /missing geometry/],
  ["missing limit total", (dir) => mutateCapture(dir, (c) => { delete c.lifecycle.limits.runtimeGuard.total; }), /activity \/ limits/],
  ["bad content hash", (dir) => mutateCapture(dir, (c) => { c.content.sha256 = "bad"; }), /content evidence/],
  ["playback numbering gap", (dir) => fs.unlinkSync(join(dir, "playback-frames/frame_00001.png")), /PNG.*ENOENT/],
  ["playback signature", (dir) => fs.writeFileSync(join(dir, "playback-frames/frame_00001.png"), "not PNG"), /signature/],
  ["playback map", (dir) => mutateCapture(dir, (c) => { c.playback[1].frame++; }), /dense playback map/],
  ["unlisted PNG", (dir) => fs.writeFileSync(join(dir, "selected-frames/extra.png"), png()), /unlisted PNG/],
  ["stride order", (dir) => mutateCapture(dir, (c) => { c.stride.reverse(); }), /stride/],
]) test(`capture checker rejects ${name}`, (t) => {
  const dir = temporary(t), capture = fixture(dir, { playbackStride: 2 }); mutation(dir, capture);
  assert.match(checkCapture(dir).errors.join("; "), expected);
});

// A bundle with per-emitter telemetry: emitter 0 holds the one particle until
// frame 10, then emitter 1 does. The render profile is the device the frames came from.
const RENDER = { adapter: { description: "Test Adapter", vendorId: 4318, deviceId: 7682, driverVersion: "32.0.15.6094" },
  displayFormat: 22, depthStencilFormat: 75, multisample: { type: 4, quality: 0 }, vertexProcessing: "HWVP" };
function perEmitterFixture(directory) {
  const capture = fixture(directory);
  const finalFrame = capture.lifecycle.finalFrame;
  const counts = (frame) => (frame < 10 ? [1, 0] : [0, 1]);
  capture.lifecycle.emitters = [
    { index: 0, name: "Core", firstActive: 0, peakFrame: 0, lastActive: 9, peakParticles: 1 },
    { index: 1, name: "Tail", firstActive: 10, peakFrame: 10, lastActive: finalFrame, peakParticles: 1 }];
  capture.render = RENDER;
  fs.writeFileSync(join(directory, "capture.json"), json(capture));
  fs.writeFileSync(join(directory, "telemetry/summary.json"), json(capture.lifecycle));
  fs.writeFileSync(join(directory, "telemetry/frames.jsonl"), Array.from({ length: finalFrame + 1 },
    (_, frame) => JSON.stringify({ frame, particles: 1, perEmitter: counts(frame), spawnerIdle: true }) + "\n").join(""));
  return capture;
}
function mutateTelemetry(directory, mutate) {
  const path = join(directory, "telemetry/frames.jsonl");
  const lines = fs.readFileSync(path, "utf8").trim().split("\n").map(JSON.parse);
  mutate(lines);
  fs.writeFileSync(path, lines.map((v) => JSON.stringify(v) + "\n").join(""));
}
function mutateLifecycle(directory, mutate) {
  mutateCapture(directory, (c) => mutate(c.lifecycle));
  const capture = JSON.parse(fs.readFileSync(join(directory, "capture.json"), "utf8"));
  fs.writeFileSync(join(directory, "telemetry/summary.json"), json(capture.lifecycle));
}

test("capture checker accepts per-emitter telemetry and a render profile, and a legacy bundle without either", (t) => {
  const dir = temporary(t); perEmitterFixture(dir);
  assert.deepEqual(checkCapture(dir).errors, []);
  const legacy = temporary(t); fixture(legacy);
  assert.deepEqual(checkCapture(legacy).errors, []);
});
for (const [name, mutation, expected] of [
  ["a sum that misses a particle", (dir) => mutateTelemetry(dir, (lines) => { lines[3].perEmitter = [0, 0]; }), /account for every particle/],
  ["a short per-emitter array", (dir) => mutateTelemetry(dir, (lines) => { lines[0].perEmitter = [1]; }), /account for every particle/],
  ["a line without perEmitter", (dir) => mutateTelemetry(dir, (lines) => { delete lines[5].perEmitter; }), /or in neither/],
  ["lines without lifecycle emitters", (dir) => mutateLifecycle(dir, (l) => { delete l.emitters; }), /or in neither/],
  ["a landmark the lines contradict", (dir) => mutateLifecycle(dir, (l) => { l.emitters[1].firstActive = 11; }), /landmarks differ/],
  ["a peak count the lines contradict", (dir) => mutateLifecycle(dir, (l) => { l.emitters[0].peakParticles = 2; }), /landmarks differ/],
  ["an emitter out of order", (dir) => mutateLifecycle(dir, (l) => { l.emitters.reverse(); }), /invalid lifecycle emitters/],
  ["an emitter without a name", (dir) => mutateLifecycle(dir, (l) => { delete l.emitters[0].name; }), /invalid lifecycle emitters/],
  ["a landmark past the final frame", (dir) => mutateLifecycle(dir, (l) => { l.emitters[1].lastActive = 99; }), /invalid lifecycle emitters/],
  ["a render profile without a driver version", (dir) => mutateCapture(dir, (c) => { c.render.adapter.driverVersion = "unknown"; }), /render profile/],
  ["an unknown vertex-processing mode", (dir) => mutateCapture(dir, (c) => { c.render.vertexProcessing = "MIXED"; }), /render profile/],
]) test(`capture checker rejects ${name}`, (t) => {
  const dir = temporary(t); perEmitterFixture(dir); mutation(dir);
  assert.match(checkCapture(dir).errors.join("; "), expected);
});

// Content evidence as the runner writes it: two resolved textures, one
// unresolved, a MEG shader, and the placeholder for the missing name.
const LOOSE = { kind: "loose", path: "C:/content/SOFT.TGA" };
const LEDGER = [
  { type: "shader", identity: "ENGINE\\PRIMALPHA.FX", requests: ["Engine\\PrimAlpha.fx"],
    attempts: [{ name: "ENGINE\\PRIMALPHA.FX", result: "not-found", winner: null, sha256: null },
      { name: "Data\\Art\\Shaders\\ENGINE\\PRIMALPHA.FX", result: "resolved", sha256: "c".repeat(64),
        winner: { kind: "archive", path: "C:/game/Data/Shaders.meg", member: "DATA\\ART\\SHADERS\\ENGINE\\PRIMALPHA.FX", start: 40, size: 9 } }],
    outcome: "resolved", winner: { kind: "archive", path: "C:/game/Data/Shaders.meg", member: "DATA\\ART\\SHADERS\\ENGINE\\PRIMALPHA.FX", start: 40, size: 9 },
    sha256: "c".repeat(64) },
  { type: "texture", identity: "ABSENT.TGA", requests: ["absent.tga"],
    attempts: [{ name: "ABSENT.TGA", result: "not-found", winner: null, sha256: null },
      { name: "Data\\Art\\Textures\\ABSENT.TGA", result: "read-rejected", winner: LOOSE, sha256: null, size: 0, reason: "empty" }],
    outcome: "default-substituted", winner: { kind: "embedded", resourceId: "IDB_MISSING" }, sha256: "d".repeat(64) },
  { type: "texture", identity: "SOFT.TGA", requests: ["soft.tga", "SOFT.TGA"],
    attempts: [{ name: "SOFT.TGA", result: "resolved", winner: LOOSE, sha256: "e".repeat(64) }],
    outcome: "resolved", winner: LOOSE, sha256: "e".repeat(64) },
];
function contentFixture(directory) {
  const capture = fixture(directory);
  mutateCapture(directory, (c) => {
    c.content.resolvedTextures = ["SOFT.TGA", "soft.tga"];
    c.content.unresolvedTextures = ["absent.tga"];
    c.content.resolvedTextureProvenance = {
      "SOFT.TGA": { identity: "SOFT.TGA", winner: LOOSE, sha256: "e".repeat(64) },
      "soft.tga": { identity: "SOFT.TGA", winner: LOOSE, sha256: "e".repeat(64) } };
    c.content.loadedAssets = structuredClone(LEDGER);
  });
  return capture;
}
test("capture checker accepts texture attestation with its asset ledger", (t) => {
  const dir = temporary(t); contentFixture(dir);
  assert.deepEqual(checkCapture(dir).errors, []);
});
for (const [name, mutate, expected] of [
  ["one attestation array without the rest", (c) => { delete c.unresolvedTextures; }, /present together/],
  ["a ledger without the attestation", (c) => { for (const k of ["resolvedTextures", "unresolvedTextures", "resolvedTextureProvenance"]) delete c[k]; }, /present together/],
  ["a non-string attested name", (c) => { c.unresolvedTextures = [7]; }, /must be names/],
  ["a name both resolved and unresolved", (c) => { c.unresolvedTextures.push("soft.tga"); }, /overlap/],
  ["a resolved name without provenance", (c) => { delete c.resolvedTextureProvenance["soft.tga"]; }, /cover exactly/],
  ["provenance for an unresolved name", (c) => { c.resolvedTextureProvenance["absent.tga"] = c.resolvedTextureProvenance["soft.tga"]; }, /cover exactly/],
  ["provenance on the placeholder", (c) => { c.resolvedTextureProvenance["soft.tga"].winner = { kind: "embedded", resourceId: "IDB_MISSING" }; }, /cover exactly/],
  ["a not-found attempt with a winner", (c) => { c.loadedAssets[0].attempts[0].winner = LOOSE; }, /invalid asset ledger/],
  ["a read-rejected attempt with a hash", (c) => { c.loadedAssets[1].attempts[1].sha256 = "f".repeat(64); }, /invalid asset ledger/],
  ["a read-rejected attempt without its size", (c) => { delete c.loadedAssets[1].attempts[1].size; }, /invalid asset ledger/],
  ["a resolved attempt without a hash", (c) => { c.loadedAssets[2].attempts[0].sha256 = null; }, /invalid asset ledger/],
  ["an archive winner without its range", (c) => { delete c.loadedAssets[0].winner.start; }, /invalid asset ledger/],
  ["a loose winner carrying a resource id", (c) => { c.loadedAssets[2].winner = { ...LOOSE, resourceId: "IDB_MISSING" }; }, /invalid asset ledger/],
  ["a resolved outcome on the embedded fallback", (c) => { c.loadedAssets[1].outcome = "resolved"; }, /invalid asset ledger/],
  ["a substituted outcome on a loose file", (c) => { c.loadedAssets[2].outcome = "default-substituted"; }, /invalid asset ledger/],
  ["a failed outcome with a winner", (c) => { c.loadedAssets[2].outcome = "failed"; }, /invalid asset ledger/],
]) test(`capture checker rejects ${name}`, (t) => {
  const dir = temporary(t); contentFixture(dir);
  mutateCapture(dir, (c) => mutate(c.content));
  assert.match(checkCapture(dir).errors.join("; "), expected);
});

test("capture checker rejects a directory junction escaping the bundle", (t) => {
  const root = temporary(t), dir = join(root, "bundle"), outside = join(root, "outside");
  const capture = fixture(dir); fs.renameSync(join(dir, "selected-frames"), outside);
  fs.symlinkSync(outside, join(dir, "selected-frames"), process.platform === "win32" ? "junction" : "dir");
  assert.match(checkCapture(dir).errors.join("; "), /escapes bundle/);
  assert.ok(fs.existsSync(join(outside, capture.frames[0].png.split("/").at(-1))));
});

test("capture checker checks supplied spec against normalized native defaults", (t) => {
  const dir = temporary(t); fixture(dir);
  assert.deepEqual(checkCapture(dir, { spec: { schemaVersion: 1, output: { playbackStride: 0 } } }).errors, []);
  assert.match(checkCapture(dir, { spec: { output: { playbackStride: 4 } } }).errors.join(), /spec differs/);
});

test("filmstrip concat list follows sparse stride order with appended final, including apostrophe escaping", (t) => {
  const root = temporary(t), dir = join(root, "owner's bundle"), capture = fixture(dir);
  assert.deepEqual(capture.stride, [0, 15, 30, 31]);
  const list = buildConcatList(capture, dir);
  assert.deepEqual([...list.matchAll(/frame-(\d+)\.png/g)].map((v) => Number(v[1])), [0, 15, 30, 31]);
  assert.ok(list.includes("owner'\\''s bundle"));
  assert.ok(!list.includes("\\frame"));
  const args = buildFilmstripArgs({ list: "list.txt", out: "filmstrip.png", frameCount: 4 });
  assert.ok(args.includes("concat")); assert.ok(args.includes("tile=8x1:nb_frames=4"));
  assert.ok(args.includes("png")); assert.ok(!args.some((v) => v.includes("%05d")));
});

test("quota permits the exact boundary, counts logs and temporaries, and cleans only owned outputs", (t) => {
  const dir = temporary(t); fs.writeFileSync(join(dir, "native"), "123");
  fs.mkdirSync(join(dir, "logs")); fs.writeFileSync(join(dir, "logs/observe.log"), "1234");
  fs.writeFileSync(join(dir, "filmstrip.concat.txt"), "12"); fs.writeFileSync(join(dir, "filmstrip.png"), "1");
  assert.equal(checkQuota(dir, 13, "123"), 10);
  assert.throws(() => checkQuota(dir, 12, "123", { owned: ["filmstrip.png", "filmstrip.concat.txt"] }), { exitCode: 16 });
  assert.deepEqual(fs.readdirSync(dir).sort(), ["logs", "native"]);
});

test("serialized manifest alone crosses quota; assembly removes its outputs and preserves native evidence", async (t) => {
  const dir = temporary(t); fixture(dir, { finalFrame: 0 });
  const nativeNames = fs.readdirSync(dir).sort();
  const size = () => checkQuota(dir, Number.MAX_SAFE_INTEGER);
  mutateCapture(dir, (c) => { c.spec.output.maxBytes = size() + 200; });
  const max = JSON.parse(fs.readFileSync(join(dir, "capture.json"))).spec.output.maxBytes;
  assert.ok(size() + png().length < max, "native files plus filmstrip fit");
  const original = fs.readFileSync(join(dir, "capture.json"));
  await assert.rejects(assembleBundle(dir), { exitCode: 16 });
  assert.deepEqual(fs.readdirSync(dir).sort(), nativeNames);
  assert.ok(original.equals(fs.readFileSync(join(dir, "capture.json"))));
  assert.ok(!fs.existsSync(join(dir, "manifest.json")));
});

test("one stride PNG copies through, and zero stride produces a null filmstrip", async (t) => {
  for (const zeroStride of [false, true]) {
    const dir = join(temporary(t), "bundle"), capture = fixture(dir, { finalFrame: 0, zeroStride });
    const manifest = await assembleBundle(dir, { run: () => assert.fail("no child needed") });
    assert.equal(manifest.deterministic.filmstripSha256, zeroStride ? null : capture.frames[0].sha256);
    assert.equal(fs.existsSync(join(dir, "filmstrip.png")), !zeroStride);
    if (!zeroStride) assert.ok(fs.readFileSync(join(dir, "filmstrip.png")).equals(fs.readFileSync(join(dir, capture.frames[0].png))));
    assert.equal(manifest.nondeterministic.manifestBytes, fs.statSync(join(dir, "manifest.json")).size);
    assert.equal(manifest.nondeterministic.totalBytes, checkQuota(dir, Number.MAX_SAFE_INTEGER));
  }
});

test("manifest deterministic and nondeterministic fields stay separate", () => {
  const manifest = buildManifest({ captureHash: "a", filmstripHash: "b", playback: { frameCount: 12, duration: 0.8 },
    mp4Hash: "c", mp4Bytes: 10, bundleBytes: 20, temporaryBytes: 3, manifestBytes: 5 });
  assert.deepEqual(manifest, { schemaVersion: 1,
    deterministic: { captureSha256: "a", filmstripSha256: "b", playback: { frameCount: 12, duration: 0.8 } },
    nondeterministic: { mp4Sha256: "c", mp4Bytes: 10, bundleBytes: 20, temporaryBytes: 3, manifestBytes: 5, totalBytes: 25 } });
});

for (const name of ["capture.json", "telemetry/summary.json", "telemetry/frames.jsonl", "selected-frames/frame-00000.png",
  "playback-frames/frame_00000.png", "filmstrip.png", "manifest.json"]) {
  test(`repeat catches one changed byte in ${name}`, async (t) => {
    const root = temporary(t), first = join(root, "first"), second = join(root, "second");
    fixture(first, { finalFrame: 0 }); await assembleBundle(first);
    fs.mkdirSync(join(first, "playback-frames")); fs.writeFileSync(join(first, "playback-frames/frame_00000.png"), png());
    fs.cpSync(first, second, { recursive: true }); assert.deepEqual(compareRepeat(first, second), []);
    const path = join(second, name), bytes = fs.readFileSync(path);
    if (name === "manifest.json") {
      const at = bytes.indexOf(Buffer.from('"captureSha256": "')) + Buffer.byteLength('"captureSha256": "'); bytes[at] = bytes[at] === 97 ? 98 : 97;
    } else bytes[0] ^= 1;
    fs.writeFileSync(path, bytes);
    assert.match(compareRepeat(first, second).join("; "), new RegExp(name.replaceAll(".", "\\.")));
  });
}

test("repeat ignores mp4 bytes, log bytes, and nondeterministic section, but checks playback count and duration", async (t) => {
  const root = temporary(t), first = join(root, "first"), second = join(root, "second");
  fixture(first, { finalFrame: 0 }); await assembleBundle(first); fs.cpSync(first, second, { recursive: true });
  fs.mkdirSync(join(first, "playback")); fs.mkdirSync(join(second, "playback"));
  fs.writeFileSync(join(first, "playback/preview.mp4"), "first video"); fs.writeFileSync(join(second, "playback/preview.mp4"), "different video");
  fs.writeFileSync(join(second, "logs/observe.log"), "different timing\n");
  const path = join(second, "manifest.json"), manifest = JSON.parse(fs.readFileSync(path));
  manifest.nondeterministic = { any: "different sizes, log totals, mp4 hash" }; fs.writeFileSync(path, json(manifest));
  assert.deepEqual(compareRepeat(first, second), []);
  manifest.deterministic.playback.frameCount++; fs.writeFileSync(path, json(manifest));
  assert.match(compareRepeat(first, second).join(), /playback count \/ duration/);
  manifest.deterministic.playback.frameCount--; manifest.deterministic.playback.duration++; fs.writeFileSync(path, json(manifest));
  assert.match(compareRepeat(first, second).join(), /playback count \/ duration/);
  fs.unlinkSync(join(second, "filmstrip.png")); assert.match(compareRepeat(first, second).join(), /file sets differ/);
});

const mediaAvailable = ["ffmpeg", "ffprobe"].every((bin) => spawnSync(bin, ["-version"], { timeout: 10000, windowsHide: true, stdio: "ignore" }).status === 0);
// File-backed child output works on restricted Windows desktops where anonymous
// pipes cannot be created. Keep actual encoding/probing in these integration tests.
function mediaRun(root) {
  return async (bin, args, { timeoutMs }) => {
    const out = join(root, "child-stdout"), err = join(root, "child-stderr");
    const stdout = fs.openSync(out, "w"), stderr = fs.openSync(err, "w");
    let result;
    try { result = spawnSync(bin, args, { timeout: timeoutMs, windowsHide: true, stdio: ["ignore", stdout, stderr] }); }
    finally { fs.closeSync(stdout); fs.closeSync(stderr); }
    const value = { code: result.status, stdout: fs.readFileSync(out, "utf8"), stderr: fs.readFileSync(err, "utf8") };
    fs.unlinkSync(out); fs.unlinkSync(err);
    if (result.error) throw result.error;
    return value;
  };
}
test("real ffmpeg tile keeps sparse final frame and concat apostrophe path", { skip: !mediaAvailable && "ffmpeg / ffprobe absent" }, async (t) => {
  const root = temporary(t), dir = join(root, "owner's bundle"); fixture(dir);
  const run = mediaRun(root);
  await assembleBundle(dir, { run });
  const out = join(root, "pixels.rgba");
  const raw = await run("ffmpeg", ["-y", "-v", "error", "-i", join(dir, "filmstrip.png"), "-f", "rawvideo", "-pix_fmt", "rgba", out], { timeoutMs: 10000 });
  assert.equal(raw.code, 0, raw.stderr);
  const pixels = fs.readFileSync(out);
  assert.equal(pixels.length, 128 * 16 * 4);
  assert.deepEqual([0, 1, 2, 3].map((index) => pixels[index * 16 * 4]), [0, 15, 30, 31]);
  assert.ok(!fs.existsSync(join(dir, "filmstrip.concat.txt")));
});

for (const stride of [1, 2, 3, 4]) {
  test(`playback stride ${stride} maps and really encodes the exact frame count and duration`, { skip: !mediaAvailable && "ffmpeg / ffprobe absent" }, async (t) => {
    const root = temporary(t), dir = join(root, "bundle"), capture = fixture(dir, { finalFrame: 31, playbackStride: stride });
    const count = Math.floor(31 / stride) + 1, duration = count * stride / 60;
    assert.deepEqual(playbackMetrics(capture), { frameCount: count, duration, fps: 60 / stride });
    const manifest = await assembleBundle(dir, { run: mediaRun(root) });
    assert.deepEqual(manifest.deterministic.playback, { frameCount: count, duration });
    assert.equal(manifest.nondeterministic.mp4Bytes, fs.statSync(join(dir, "playback/preview.mp4")).size);
    assert.equal(manifest.nondeterministic.mp4Sha256, sha256(fs.readFileSync(join(dir, "playback/preview.mp4"))));
    assert.equal(manifest.nondeterministic.totalBytes, checkQuota(dir, Number.MAX_SAFE_INTEGER));
    assert.ok(!fs.existsSync(join(dir, "playback/preview.tmp.mp4")));
    assert.throws(() => checkPlaybackProbe({ streams: [{ nb_read_frames: count - 1, duration }] }, capture), /differs from map/);
    assert.throws(() => checkPlaybackProbe({ streams: [{ nb_read_frames: count, duration: duration + 0.02 }] }, capture), /differs from map/);
  });
}

test("a real stalled child is killed at its deadline", async () => {
  let child;
  const start = Date.now();
  await assert.rejects(spawnWithDeadline(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {
    timeoutMs: 100, spawnImpl: (bin, args, options) => { child = spawn(bin, args, { ...options, stdio: "ignore" }); return child; },
  }), /deadline.*child killed/);
  assert.ok(Date.now() - start < 5000);
  assert.equal(child.killed, true);
  assert.throws(() => process.kill(child.pid, 0), { code: "ESRCH" });
});

test("missing executable reports an actionable child start error", async () => {
  await assert.rejects(spawnWithDeadline("observe-no-such-executable", [], { timeoutMs: 1000,
    spawnImpl: (bin, args, options) => spawn(bin, args, { ...options, stdio: "ignore" }),
  }), /could not start.*ENOENT/);
});

test("exe failure preserves its exact exit code and never assembles stale evidence", async (t) => {
  const dir = join(temporary(t), "absent");
  await assert.rejects(runObservation("spec.json", dir, { run: async () => ({ code: 16, stderr: "quota", stdout: "" }) }),
    (error) => error.exitCode === 16 && /exe exit 16.*quota/.test(error.message));
  assert.ok(!fs.existsSync(dir));
});

test("nonempty output directory is refused without launching or overwriting user work", async (t) => {
  const dir = temporary(t); fs.writeFileSync(join(dir, "user.txt"), "keep me");
  await assert.rejects(runObservation("spec.json", dir, { run: () => assert.fail("must not launch") }), { exitCode: 13 });
  assert.equal(fs.readFileSync(join(dir, "user.txt"), "utf8"), "keep me");
});

test("observe repeat runs into siblings and verifies both final bundles", async (t) => {
  const root = temporary(t), specPath = join(root, "spec.json"), directory = join(root, "run"), calls = [];
  const template = join(root, "template"), capture = fixture(template, { finalFrame: 0 }); fs.writeFileSync(specPath, json(capture.spec));
  const run = async (exe, args, options) => {
    calls.push({ exe, args, options }); fs.cpSync(template, args[2], { recursive: true }); return { code: 0 };
  };
  await observe(specPath, directory, { repeat: true, exe: "test-exe", roots: ["game-a", "game-b"], run });
  assert.deepEqual(calls.map((v) => v.args[2]), [directory, directory + "-repeat"]);
  assert.ok(calls.every((v) => v.options.timeoutMs === 660000 && v.args.slice(-2).join() === "game-a,game-b"));
  assert.deepEqual(compareRepeat(directory, directory + "-repeat"), []);
});

test("ffmpeg absent gives a visible lane SKIP only with allow-missing, otherwise FAIL", () => {
  const run = (allowed) => runObserveLane({ desktop: () => ({ ok: true }), exists: () => true, gameRoots: () => ["game"],
    probe: () => ({ status: null, error: new Error("ENOENT") }), runObserve: () => assert.fail("must not run"),
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowed ? ["observe"] : [])) });
  assert.equal(run(false).status, "FAIL"); assert.equal(run(true).status, "SKIP"); assert.match(run(true).note, /ffmpeg/);
});

test("failed ffmpeg assembly removes only its outputs, and carries a deadline", async (t) => {
  const dir = temporary(t); fixture(dir);
  const capture = fs.readFileSync(join(dir, "capture.json"));
  await assert.rejects(assembleBundle(dir, { run: async (bin, args, options) => {
    assert.equal(bin, "ffmpeg"); assert.equal(options.timeoutMs, 120000);
    fs.writeFileSync(args.at(-1), "partial"); return { code: 1, stderr: "missing codec" };
  } }), /ffmpeg exit 1.*missing codec/);
  assert.ok(capture.equals(fs.readFileSync(join(dir, "capture.json"))));
  assert.ok(!fs.existsSync(join(dir, "filmstrip.png"))); assert.ok(!fs.existsSync(join(dir, "filmstrip.concat.txt")));
});

test("quota counts a temporary mp4 and removes it on breach, retaining native frames", async (t) => {
  const dir = temporary(t); const capture = fixture(dir, { finalFrame: 0, playbackStride: 1 });
  capture.spec.output.maxBytes = 1024 * 1024;
  fs.writeFileSync(join(dir, "capture.json"), json(capture));
  const native = fs.readFileSync(join(dir, "capture.json"));
  await assert.rejects(assembleBundle(dir, { run: async (bin, args, options) => {
    assert.equal(bin, "ffmpeg"); assert.equal(options.timeoutMs, 300000);
    fs.writeFileSync(args.at(-1), Buffer.alloc(capture.spec.output.maxBytes + 1));
    return { code: 0 };
  } }), { exitCode: 16 });
  assert.ok(!fs.existsSync(join(dir, "playback")));
  assert.ok(!fs.existsSync(join(dir, "filmstrip.png")));
  assert.ok(native.equals(fs.readFileSync(join(dir, "capture.json"))));
});

test("ffprobe failure cleans assembled outputs and cannot publish a marker", async (t) => {
  const dir = temporary(t); fixture(dir, { finalFrame: 0, playbackStride: 1 });
  await assert.rejects(assembleBundle(dir, { run: async (bin, args, options) => {
    if (bin === "ffmpeg") {
      assert.equal(options.timeoutMs, 300000); fs.writeFileSync(args.at(-1), "mp4"); return { code: 0 };
    }
    assert.equal(bin, "ffprobe"); assert.equal(options.timeoutMs, 30000); return { code: 1, stderr: "probe failed" };
  } }), /probe failed/);
  assert.ok(!fs.existsSync(join(dir, "manifest.json")));
  assert.ok(!fs.existsSync(join(dir, "playback")));
  assert.ok(fs.existsSync(join(dir, "selected-frames/frame-00000.png")));
});

test("manifest publication is the last write, using a checked temporary marker then rename", async (t) => {
  const dir = temporary(t); fixture(dir, { finalFrame: 0 });
  const operations = [];
  const io = { ...fs, writeFileSync: (path, text) => {
    assert.ok(!fs.existsSync(join(dir, "manifest.json")));
    if (path.endsWith("manifest.json.tmp")) {
      const max = JSON.parse(fs.readFileSync(join(dir, "capture.json"))).spec.output.maxBytes;
      checkQuota(dir, max, text);
    }
    operations.push(`write ${path}`); fs.writeFileSync(path, text);
  }, renameSync: (from, to) => { operations.push(`rename ${to}`); fs.renameSync(from, to); } };
  await assembleBundle(dir, { io });
  assert.equal(operations.at(-1), `rename ${join(dir, "manifest.json")}`);
  assert.ok(!fs.existsSync(join(dir, "manifest.json.tmp")));
});

test("all seventeen lane rows are pinned, and a null pin still fails until measured", () => {
  const cases = observationCases(); assert.equal(cases.length, 17);
  assert.deepEqual(cases.map((v) => v.name), Object.keys(PINNED));
  for (const [name, row] of Object.entries(PINNED)) {
    assert.ok(Object.values(row).every((v) => v !== null), `${name} has an unmeasured pin`);
    // The measured values themselves pass; the same values against a null pin fail.
    assert.deepEqual(checkCase(name, row), []);
    const unpinned = Object.fromEntries(Object.keys(row).map((key) => [key, null]));
    assert.match(checkCase(name, row, { pins: { [name]: unpinned } }).join("; "), /--measure/);
  }
  assert.equal(cases.find((v) => v.name === "index-cap").spec.scenario.overloadGuardEnabled, false);
  assert.equal(cases.find((v) => v.name === "guard-short-hard").spec.scenario.estimatedParticlesPerInstance, 2048);
  assert.equal(cases.find((v) => v.name === "child-starve").spec.scenario.overloadGuardLimit, 1000);
  assert.equal(cases.find((v) => v.name === "sparse-playback").spec.output.maxBytes, 200 * 1024 * 1024);
});

test("late flash measures the first increase after zero and pins onset, final and last-active", () => {
  const capture = { lifecycle: { status: "natural-end", finalFrame: 91, firstActive: 0, lastActive: 76,
    limits: { runtimeGuard: { total: 0 }, hardGuard: { total: 0 }, indexCap: { total: 0 } },
    emitters: [{ firstActive: 0 }, { firstActive: 2 }] },
    content: { resolvedTextures: ["a.tga", "b.tga"], unresolvedTextures: [] } };
  const samples = [{ frame: 0, particles: 1, perEmitter: [1, 0] }, { frame: 1, particles: 1, perEmitter: [1, 0] },
    { frame: 2, particles: 9, perEmitter: [1, 8] }, { frame: 3, particles: 10, perEmitter: [2, 8] }];
  const values = measuredValues("late-flash", capture, samples);
  assert.equal(values.flashOnset, 2);
  assert.deepEqual(values.emitterFirstActive, [0, 2]);
  assert.deepEqual(values.perEmitterAtOnset, [1, 8]);
  assert.deepEqual(checkCase("late-flash", values, { pins: { "late-flash": values } }), []);
  assert.deepEqual(checkCase("late-flash", values, { measure: true }), []);
  assert.match(checkCase("late-flash", { ...values, flashOnset: 3 }, { pins: { "late-flash": values } }).join(), /flashOnset differs/);
  // The core grows at the onset instead: the flash emitter no longer explains it.
  assert.match(checkCase("late-flash", { ...values, perEmitterAtOnset: [9, 0] }, { measure: true }).join(), /one burst of 8/);
});

test("death child is measured per emitter: it starts the frame after its parent's last", () => {
  const capture = { lifecycle: { status: "natural-end", finalFrame: 6, firstActive: 0, lastActive: 4,
    limits: { runtimeGuard: { total: 0 }, hardGuard: { total: 0 }, indexCap: { total: 0 } },
    emitters: [{ firstActive: 0, lastActive: 1 }, { firstActive: 2, lastActive: 4 }] } };
  const samples = [[2, 0], [2, 0], [0, 2], [0, 2], [0, 2], [0, 0], [0, 0]].map((perEmitter, frame) =>
    ({ frame, particles: perEmitter[0] + perEmitter[1], perEmitter }));
  const values = measuredValues("death-child", capture, samples);
  assert.deepEqual([values.emitterFirstActive, values.emitterLastActive, values.perEmitterAtParentLast], [[0, 2], [1, 4], [2, 0]]);
  const semantic = (changes) => checkCase("death-child", { ...values, lastActive: 59, ...changes }, { measure: true }).join();
  assert.equal(semantic({ emitterLastActive: [1, 59] }), "");
  assert.match(semantic({ emitterLastActive: [1, 59], emitterFirstActive: [0, 3] }), /frame after its parent's last/);
  assert.match(semantic({ emitterLastActive: [1, 58] }), /end the effect/);
});

test("the telemetry refusal is predicted from the wide case's line bytes, reserve included", () => {
  // 100 bytes a line with its newline; a total landing exactly on the budget is admitted.
  const lines = ["x".repeat(99), "y".repeat(99), "z".repeat(99)];
  assert.equal(predictedTelemetryRefusal(lines, 1024 * 1024 + 150), 1);
  assert.equal(predictedTelemetryRefusal(lines, 1024 * 1024 + 200), 2);
  assert.equal(predictedTelemetryRefusal(lines, 1024 * 1024 + 300), null);
  const values = { exitCode: 16, failureHook: "quota-telemetry", failureFrame: 7, predictedFrame: 7 };
  assert.deepEqual(checkCase("quota-telemetry", values, { measure: true }), []);
  assert.match(checkCase("quota-telemetry", { ...values, failureFrame: 8 }, { measure: true }).join(), /line bytes predict/);
  assert.match(checkCase("quota-telemetry", { ...values, failureHook: "quota" }, { measure: true }).join(), /line bytes predict/);
});

test("lane checks exact counters and independent semantic assertions even with matching pins", () => {
  const values = { status: "spawn-limited", finalFrame: 50, runtimeGuard: 2, hardGuard: 0, indexCap: 0 };
  assert.deepEqual(checkCase("child-starve", values, { pins: { "child-starve": values } }), []);
  assert.match(checkCase("child-starve", { ...values, runtimeGuard: 3 }, { pins: { "child-starve": values } }).join(), /runtimeGuard differs/);
  assert.match(checkCase("child-starve", { ...values, hardGuard: 1 }, { measure: true }).join(), /runtime-only/);
  assert.match(checkCase("death-child", { ...values, status: "natural-end", lastActive: 58 }, { measure: true }).join(), /frame 59/);
});

test("fixture generator lazily accepts an injected attestation contract for all nine kinds", async (t) => {
  const dir = temporary(t), calls = [], validated = [];
  const kinds = ["late-flash", "guard-short", "death-child", "child-starve", "budget-bailout", "budget-bailout-skip", "index-cap", "wide",
    "missing-texture"];
  await generateFixtures("exe", ["game"], dir, {
    fixtureContract: { OBSERVE_FIXTURE_KINDS: kinds, validateObserveFixture: (fields, kind) => { validated.push(kind); return fields?.reloaded ? [] : ["not reloaded"]; } },
    run: async (exe, args, options) => {
      const kind = args[1]; calls.push(kind); assert.ok(options.timeoutMs > 0); fs.writeFileSync(args[2], "fixture");
      const weather = kind === "weather";
      const fields = ["weather", "guard"].includes(kind) ? {
        kind, reloaded: true, colorTexture: TEXTURES[0], normalTexture: TEXTURES[1], useBursts: true, nBursts: 1,
        nParticlesPerBurst: weather ? 64 : 2048, nParticlesPerSecond: weather ? 64 : 0, initialDelay: 0, skipTime: 0,
        burstDelay: 10000, lifetime: 600, gravity: 0, isWeatherParticle: weather, weatherCubeSize: weather ? 128 : 0,
        weatherCubeDistance: weather ? 200 : 0, positionGroup: 1, scale: 8, alpha: 1,
      } : { reloaded: true };
      return { code: 0, stdout: `[replay-fixture] ${JSON.stringify(fields)}\n` };
    },
  });
  assert.deepEqual(validated, kinds); assert.deepEqual(calls, ["weather", "guard", ...kinds]);
});
