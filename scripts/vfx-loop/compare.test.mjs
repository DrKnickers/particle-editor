import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { checkAssembledBundle } from "../observe.mjs";
import { writeComparison, alignmentDiff, checkBudgets, pairFrames, placeholderPng, readIhdr,
  buildPairedFilmstripArgs, buildPairedClipArgs, randomSchedule, telemetryDelta, METRICS, TECHNICAL_BUDGETS } from "./compare.mjs";
import { bundle, spec, fakeFfmpeg, png, PROFILE } from "./testing.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const scratch = join(repoRoot, "tests/.tmp/vfx-loop-unit");
fs.mkdirSync(scratch, { recursive: true });
function temporary(t) {
  const directory = fs.mkdtempSync(join(scratch, "compare-"));
  t.after(() => fs.rmSync(directory, { recursive: true, force: true }));
  return directory;
}
const readJson = (path) => JSON.parse(fs.readFileSync(path, "utf8"));
const PEAK = [{ id: "peak-particles-not-increased", metric: "peakParticles", maxDelta: 0 }];

async function pair(t, candidateOptions = {}, baselineOptions = {}) {
  const root = temporary(t);
  const baseline = await bundle(join(root, "baseline"), spec(), baselineOptions);
  const candidate = await bundle(join(root, "candidate"), spec(candidateOptions.spec), candidateOptions);
  return { root, baseline, candidate, out: join(root, "comparison") };
}

// No metric may rank the effects (§13.3): no output carries a key that reads as one.
function rankingKeys(value, path = "") {
  if (!value || typeof value !== "object") return [];
  return Object.entries(value).flatMap(([key, child]) =>
    [...(/score|better|worse|winner|prefer|rank|best|quality/i.test(key) ? [`${path}${key}`] : []), ...rankingKeys(child, `${path}${key}.`)]);
}

test("an aligned pair writes every §13.2 output, pairs each frame, and ranks nothing", async (t) => {
  const { baseline, candidate, out } = await pair(t, { tint: (frame) => frame + 50 });
  const calls = [];
  const { metrics, review } = await writeComparison(baseline, candidate, out, { budgets: PEAK, run: fakeFfmpeg(calls) });
  for (const name of ["paired-filmstrip.png", "metrics.json", "telemetry-delta.json", "random-schedule-note.json", "review.json"])
    assert.ok(fs.existsSync(join(out, name)), name);
  assert.deepEqual(fs.readdirSync(join(out, "selected-frame-pairs")).sort(),
    ["00000", "00002", "00003"].flatMap((f) => [`frame-${f}.baseline.png`, `frame-${f}.candidate.png`]));
  assert.deepEqual(review.pairs.map((p) => p.frame), [0, 2, 3]);
  assert.equal(review.decision, null);
  assert.deepEqual(rankingKeys(metrics), []);
  assert.deepEqual(rankingKeys(review), []);
  assert.deepEqual(rankingKeys(readJson(join(out, "telemetry-delta.json"))), []);
  assert.equal(metrics.budgetsPass, true);
  assert.equal(metrics.drift.firstImageDivergence, 0);
  // The concat lists are temporary; the filmstrip pairs list entries one to one.
  assert.deepEqual(fs.readdirSync(out).filter((v) => v.endsWith(".txt")), []);
  assert.equal(calls.length, 1);
  assert.match(calls[0].args.join(" "), /vstack=inputs=2,format=rgb24,tile=8x1:nb_frames=3/);
});

test("identical runs report identical frames and no random divergence", async (t) => {
  const { baseline, candidate, out } = await pair(t);
  const { metrics, schedule } = await writeComparison(baseline, candidate, out, { run: fakeFfmpeg() });
  assert.deepEqual(metrics.drift.identicalSelectedFrames, [0, 2, 3]);
  assert.equal(metrics.drift.firstImageDivergence, null);
  assert.equal(schedule.firstDivergentFrame, null);
  assert.match(schedule.note, /match over the 4 frames/);
});

test("a prettier but over-budget candidate is a budget failure, not a win", async (t) => {
  // More particles everywhere: the kind of change a viewer may like better.
  const { baseline, candidate, out } = await pair(t, { particles: [9, 12, 6, 0], tint: (frame) => frame + 120 });
  const { metrics, review } = await writeComparison(baseline, candidate, out, { budgets: PEAK, run: fakeFfmpeg() });
  const peak = metrics.budgets.find((v) => v.id === "peak-particles-not-increased");
  assert.deepEqual({ baseline: peak.baseline, candidate: peak.candidate, delta: peak.delta, pass: peak.pass },
    { baseline: 8, candidate: 12, delta: 4, pass: false });
  assert.equal(metrics.budgetsPass, false);
  assert.deepEqual(review.technicalGate, { pass: false, failed: ["peak-particles-not-increased"] });
  assert.deepEqual(rankingKeys(metrics), []);
  assert.equal(review.decision, null);
});

test("technical budgets are always checked: a new guard refusal fails the gate", () => {
  const before = Object.fromEntries(METRICS.map((m) => [m, 0])), after = { ...before, runtimeGuardRefusals: 3 };
  const checked = checkBudgets([], before, after);
  assert.deepEqual(checked.map((v) => v.id), TECHNICAL_BUDGETS.map((v) => v.id));
  assert.deepEqual(checked.filter((v) => !v.pass).map((v) => v.id), ["no-new-runtimeGuardRefusals"]);
});

test("a budget on a value one run lacks cannot pass", () => {
  const [result] = checkBudgets([{ id: "first", metric: "firstActive", maxDelta: 5 }], { firstActive: 0 }, { firstActive: null });
  assert.equal(result.pass, false);
  assert.equal(result.delta, null);
});

test("a shorter candidate keeps the baseline's late frames, paired with ended tiles", async (t) => {
  const { baseline, candidate, out } = await pair(t, { particles: [5, 8, 0] }, { particles: [5, 8, 3, 3, 0] });
  const calls = [];
  const { review } = await writeComparison(baseline, candidate, out, { run: fakeFfmpeg(calls) });
  assert.deepEqual(review.pairs.map((p) => `${p.frame}:${p.baseline.state}/${p.candidate.state}`),
    ["0:captured/captured", "2:captured/captured", "4:captured/ended"]);
  const tile = fs.readFileSync(join(out, "selected-frame-pairs/frame-00004.candidate.png"));
  assert.deepEqual(readIhdr(tile), readIhdr(png()));
  const lists = calls[0].args.filter((_, i, a) => a[i - 1] === "-i");
  assert.equal(lists.length, 2);
});

test("a frame one run selected and the other only simulated is not-selected, not ended", () => {
  const capture = (frames, finalFrame) => ({ frames: frames.map((frame) => ({ frame, png: `f${frame}`, reasons: ["stride"] })),
    lifecycle: { finalFrame } });
  const pairs = pairFrames(capture([0, 7, 9], 9), capture([0, 9], 9));
  assert.deepEqual(pairs.map((p) => p.candidate.state), ["captured", "not-selected", "captured"]);
});

for (const [field, options, path] of [
  ["seed", { spec: { scenario: { seed: 8 } } }, /spec\.scenario\.seed: 7 vs 8/],
  ["maxFrames", { spec: { scenario: { maxFrames: 200 } } }, /spec\.scenario\.maxFrames/],
  ["camera", { profile: { ...PROFILE, camera: { ...PROFILE.camera, position: [0, -300, 125] } } }, /profile\.camera\.position/],
  ["gravity", { profile: { ...PROFILE, gravity: [0, 0, -2] } }, /profile\.gravity/],
  ["wind", { profile: { ...PROFILE, wind: [1, 0, 0] } }, /profile\.wind/],
  ["resolution", { crop: [0, 0, 16, 32] }, /geometry\.crop/],
  ["content stack", { capture: (c) => { c.content.selectedModPaths = ["Mods/X"]; } }, /content\.selectedModPaths/],
  ["RNG algorithm", { capture: (c) => { c.random.algorithm = "other"; } }, /random\.algorithm/],
  ["tool", { capture: (c) => { c.tool = "0.4.3"; } }, /tool: "0\.4\.2" vs "0\.4\.3"/],
]) test(`misaligned ${field} is refused with exit 7 and nothing is written`, async (t) => {
  const { baseline, candidate, out } = await pair(t, options);
  await assert.rejects(writeComparison(baseline, candidate, out, { run: fakeFfmpeg() }),
    (error) => error.exitCode === 7 && path.test(error.message));
  assert.equal(fs.existsSync(out), false);
});

test("alignment compares the content stack and the assets both runs loaded, not what each effect asked for", () => {
  const asset = (identity, sha256) => ({ type: "texture", identity, requests: [identity], attempts: [], outcome: "resolved",
    winner: { kind: "loose", path: `C:/c/${identity}` }, sha256 });
  const capture = (assets, extra = {}) => ({ tool: "t", spec: {}, random: {}, profile: {}, geometry: {}, render: { vertexProcessing: "HWVP" },
    content: { gameRoots: ["C:/g"], selectedModPaths: [], sha256: "a".repeat(64), loadedAssets: assets,
      resolvedTextures: assets.map((v) => v.identity), unresolvedTextures: [], resolvedTextureProvenance: {}, ...extra } });
  const baseline = capture([asset("SMOKE.TGA", "1".repeat(64)), asset("FIRE.TGA", "2".repeat(64))]);
  // The candidate swapped FIRE for EMBER: each run's own textures are not compared.
  assert.deepEqual(alignmentDiff(baseline, capture([asset("SMOKE.TGA", "1".repeat(64)), asset("EMBER.TGA", "3".repeat(64))])), []);
  // The same file with other bytes between the two runs is drift, not the edit.
  const drifted = alignmentDiff(baseline, capture([asset("SMOKE.TGA", "9".repeat(64)), asset("FIRE.TGA", "2".repeat(64))]));
  assert.deepEqual(drifted.map((v) => v.path), ["loadedAssets.texture:SMOKE.TGA.sha256"]);
  const device = capture([asset("SMOKE.TGA", "1".repeat(64)), asset("FIRE.TGA", "2".repeat(64))]);
  device.render.vertexProcessing = "SOFTWARE_VP";
  assert.deepEqual(alignmentDiff(baseline, device).map((v) => v.path), ["render.vertexProcessing"]);
});

test("the source path is the one intended difference", () => {
  const capture = (aloPath) => ({ tool: "t", spec: { scenario: { aloPath, seed: 1 } }, random: {}, profile: {}, geometry: {}, content: {} });
  assert.deepEqual(alignmentDiff(capture("C:/a/source.alo"), capture("C:/a/candidate.alo")), []);
});

test("an incomplete or tampered bundle is untrustworthy, not compared", async (t) => {
  const { baseline, candidate, out } = await pair(t);
  fs.writeFileSync(join(candidate, "filmstrip.png"), png(9));
  await assert.rejects(writeComparison(baseline, candidate, out, { run: fakeFfmpeg() }),
    (error) => error.exitCode === 1 && /candidate observation is not trustworthy: .*filmstrip hash/.test(error.message));
  fs.rmSync(join(candidate, "manifest.json"));
  assert.match(checkAssembledBundle(candidate).errors.join("; "), /bundle incomplete/);
});

test("an assembled bundle passes the finished-bundle check", async (t) => {
  const directory = await bundle(join(temporary(t), "b"));
  assert.deepEqual(checkAssembledBundle(directory).errors, []);
});

test("a non-empty comparison directory is refused", async (t) => {
  const { baseline, candidate, out } = await pair(t);
  fs.mkdirSync(out); fs.writeFileSync(join(out, "stale.json"), "{}");
  await assert.rejects(writeComparison(baseline, candidate, out, { run: fakeFfmpeg() }), (error) => error.exitCode === 13);
});

test("per-emitter telemetry attributes a change the whole-effect totals hide", () => {
  // Shortening "Smoke Slow" leaves lastActive alone while "Sparks" lives just as long.
  const sample = (frame, perEmitter) => ({ frame, particles: perEmitter[0] + perEmitter[1], perEmitter, emitters: 2, instances: 1, rngDrawCount: frame });
  const before = [sample(0, [2, 2]), sample(1, [2, 2]), sample(2, [2, 2])];
  const after = [sample(0, [2, 2]), sample(1, [0, 2]), sample(2, [0, 2])];
  const life = { status: "natural-end", finalFrame: 2, firstActive: 0, lastActive: 2, peakParticles: 4 };
  const emitters = (slowLast) => [
    { index: 0, name: "Smoke Slow", firstActive: 0, peakFrame: 0, lastActive: slowLast, peakParticles: 2 },
    { index: 1, name: "Sparks", firstActive: 0, peakFrame: 0, lastActive: 2, peakParticles: 2 }];
  const delta = telemetryDelta(before, after, life, life, emitters(2), emitters(0));
  assert.equal(delta.scope, "per-emitter");
  assert.equal(delta.lifecycle.lastActive.delta, 0);
  assert.deepEqual(delta.emitters[0].lifecycle.lastActive, { baseline: 2, candidate: 0, delta: -2 });
  assert.equal(delta.emitters[1].lifecycle.lastActive.delta, 0);
  assert.deepEqual(delta.emitters[0].series, { baseline: [2, 2, 2], candidate: [2, 0, 0] });

  const legacy = telemetryDelta(before, after, life, life, undefined, emitters(0));
  assert.deepEqual([legacy.scope, legacy.emitters], ["engine-wide", null]);
  assert.match(legacy.note, /no per-emitter telemetry/);
  const renamed = emitters(0); renamed[1].name = "Embers";
  assert.match(telemetryDelta(before, after, life, life, emitters(2), renamed).note, /different emitters/);
});

test("random schedule divergence names the first differing frame (§9.8)", () => {
  const samples = (draws) => draws.map((rngDrawCount, frame) => ({ frame, rngDrawCount, rngState: rngDrawCount }));
  const capture = { random: { algorithm: "legacy-msvc", algorithmVersion: 1, seed: 7 } };
  const note = randomSchedule(capture, capture, samples([1, 2, 3, 4]), samples([1, 2, 5, 6, 7]));
  assert.equal(note.firstDivergentFrame, 2);
  assert.equal(note.comparedFrames, 4);
  assert.match(note.note, /diverges at frame 2/);
});

test("placeholder tiles copy the frame format and refuse ones they cannot make", () => {
  for (const colorType of [0, 2, 4, 6]) {
    const header = { width: 3, height: 2, bitDepth: 8, colorType };
    assert.deepEqual(readIhdr(placeholderPng(header)), header);
  }
  assert.throws(() => placeholderPng({ width: 1, height: 1, bitDepth: 16, colorType: 6 }), /unsupported/);
  assert.throws(() => readIhdr(Buffer.from("not a png")), /not a PNG/);
});

test("with playback on both sides, a paired clip runs as long as the longer run", async (t) => {
  const root = temporary(t), playing = spec();
  playing.output.playbackStride = 1;
  const baseline = await bundle(join(root, "baseline"), playing, { particles: [5, 8, 3, 3, 0] });
  const candidate = await bundle(join(root, "candidate"), playing, { particles: [5, 8, 0] });
  const calls = [], out = join(root, "comparison");
  const { review, clip } = await writeComparison(baseline, candidate, out, { run: fakeFfmpeg(calls) });
  assert.deepEqual(clip, { path: "paired-preview.mp4", frames: 5, fps: 60,
    layout: "baseline left, candidate right; the shorter run holds its last frame" });
  assert.deepEqual(review.clip, clip);
  assert.ok(fs.statSync(join(out, "paired-preview.mp4")).size > 0);
  const args = calls.at(-1).args, inputs = args.filter((_, i, a) => a[i - 1] === "-i");
  assert.deepEqual(inputs, [join(baseline, "playback-frames", "frame_%05d.png"), join(candidate, "playback-frames", "frame_%05d.png")]);
  assert.equal(args[args.indexOf("-frames:v") + 1], "5");
});

test("without playback there is no paired clip", async (t) => {
  const { baseline, candidate, out } = await pair(t);
  const { review, clip } = await writeComparison(baseline, candidate, out, { run: fakeFfmpeg() });
  assert.equal(clip, null);
  assert.equal(review.clip, null);
  assert.equal(fs.existsSync(join(out, "paired-preview.mp4")), false);
});

test("the paired clip holds the shorter run's last frame, side by side, in even dimensions", () => {
  const args = buildPairedClipArgs({ baselineFrames: "b", candidateFrames: "c", fps: 30, frameCount: 9, out: "o.mp4" });
  assert.equal(args[args.indexOf("-filter_complex") + 1], "[0:v]tpad=stop_mode=clone:stop=-1[a];[1:v]tpad=stop_mode=clone:stop=-1[b];" +
    "[a][b]hstack=inputs=2,scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p[v]");
  assert.deepEqual(args.filter((_, i, a) => a[i - 1] === "-framerate"), ["30", "30"]);
  assert.equal(args.at(-1), "o.mp4");
  assert.throws(() => buildPairedClipArgs({ baselineFrames: "b", candidateFrames: "c", fps: 60, frameCount: 0, out: "o" }), /at least one/);
});

test("the paired filmstrip skips tiling for a single pair", () => {
  const args = buildPairedFilmstripArgs({ baselineList: "b.txt", candidateList: "c.txt", out: "o.png", count: 1 });
  assert.equal(args[args.indexOf("-filter_complex") + 1], "[0:v][1:v]vstack=inputs=2");
  assert.throws(() => buildPairedFilmstripArgs({ baselineList: "b", candidateList: "c", out: "o", count: 0 }), /at least one/);
});
