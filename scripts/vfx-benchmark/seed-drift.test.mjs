import { test } from "node:test";
import assert from "node:assert/strict";
import { labelTrio, prng, seedSearch } from "./seed.mjs";
import { alignedImageFrames, driftFloor, projection, telemetryDiff, windowErrors } from "./drift.mjs";

const ok = (seed, peakParticles, lastActive) => ({ seed, status: "natural-end", refusals: { runtimeGuard: 0, hardGuard: 0, indexCap: 0 }, peakParticles, lastActive });

test("seed search: eligibility filter and the minimum population", () => {
  const runs = Array.from({ length: 12 }, (_, i) => ok(i + 1, 100, 60));
  runs[0] = { ...runs[0], status: "frame-cap" };
  runs[1] = { ...runs[1], refusals: { runtimeGuard: 0, hardGuard: 1, indexCap: 0 } };
  runs[2] = { ...runs[2], lastActive: null };
  const r = seedSearch(runs, { drawSeed: 7 });
  assert.deepEqual(r.excluded.map((x) => x.seed), [1, 2, 3]);
  assert.equal(r.absence, "a");
});

test("seed search: zero variance scores 0, no candidate is absence b", () => {
  const r = seedSearch(Array.from({ length: 20 }, (_, i) => ok(i + 1, 100, 60)), { drawSeed: 7 });
  assert.equal(r.absence, "b");
  assert.ok(r.scores.every((s) => s.score === 0));
});

test("seed search: the outlier clears 2.5, ties keep the lower seed, trios are recorded draws", () => {
  const runs = Array.from({ length: 20 }, (_, i) => ok(i + 1, 100 + (i % 3), 60));
  runs[13] = ok(14, 400, 60);
  const r = seedSearch(runs, { drawSeed: 7 });
  assert.equal(r.outlier, 14);
  assert.equal(r.defectTrio[0], 14);
  const all = [...r.defectTrio, ...r.controlTrio];
  assert.equal(new Set(all).size, 6);
  assert.deepEqual(seedSearch(runs, { drawSeed: 7 }), r);
  assert.notDeepEqual(seedSearch(runs, { drawSeed: 8 }).controlTrio, r.controlTrio);
  // Two equal extremes: the lower seed wins.
  const tie = Array.from({ length: 20 }, (_, i) => ok(i + 1, 100, 60));
  tie[4] = ok(5, 400, 60); tie[9] = ok(10, 400, 60);
  tie.push(ok(21, 100, 60), ok(22, 100, 60));
  const t = seedSearch(tie, { drawSeed: 1, threshold: 2 });
  assert.equal(t.outlier, 5);
});

test("labelTrio is a recorded permutation of bundle-a/b/c", () => {
  const labels = labelTrio([14, 3, 9], prng(5));
  assert.deepEqual(Object.keys(labels).sort(), ["bundle-a", "bundle-b", "bundle-c"]);
  assert.deepEqual(Object.values(labels).sort((a, b) => a - b), [3, 9, 14]);
  assert.deepEqual(labelTrio([14, 3, 9], prng(5)), labels);
});

const line = (frame, per) => ({ frame, instances: 1, emitters: per.length, particles: per.reduce((s, v) => s + v, 0), perEmitter: per, rngState: frame, rngDrawCount: frame });
const capture = (frames, window, emitters = []) => ({ frames: frames.map((frame) => ({ frame })), lifecycle: { reviewWindow: window, emitters } });

test("telemetry aligns over the union window, zero-padding the shorter run, without RNG fields", () => {
  const base = [line(0, [1, 0]), line(1, [2, 0]), line(2, [0, 0])];
  const kase = [line(0, [1, 0]), line(1, [2, 1]), line(2, [0, 0]), line(3, [0, 1])].map((l) => ({ ...l, rngState: 99 }));
  assert.deepEqual(telemetryDiff(base, kase, { baseWindow: [0, 2], caseWindow: [0, 3] }), [1, 3]);
  assert.deepEqual(projection([line(4, [3, 5])], [1, 0]).get(4), [1, 2, 8, [5, 3]]);
});

test("drift floor: eligibility, candidates, and the delay and draw-order assertions", () => {
  const base = { capture: capture([0, 15, 30], [0, 30], [{ firstActive: 0, peakParticles: 4 }, { firstActive: 10, peakParticles: 4 }]), lines: [line(0, [1, 0]), line(15, [2, 1])] };
  const kase = { capture: capture([0, 15, 30, 31], [0, 31], [{ firstActive: 0, peakParticles: 4 }, { firstActive: 20, peakParticles: 4 }]), lines: [line(0, [1, 0]), line(15, [2, 0]), line(31, [0, 1])] };
  assert.deepEqual(alignedImageFrames(base.capture, kase.capture), [0, 15, 30]);
  const d = driftFloor({ recipe: { kind: "field" }, base, case: kase, ssim: { 0: 1, 15: 0.97, 30: 1 }, inspectDiffEmpty: false, delayTarget: 1 });
  assert.equal(d.eligible, true);
  // 15: SSIM below 1; 31: a case-only selected frame whose telemetry differs.
  assert.deepEqual(d.candidateImage, [15, 31]);
  assert.deepEqual(d.candidateTelemetry, [15, 31]);
  const vanished = { ...kase, capture: capture([0, 15, 30], [0, 31], [{ firstActive: 0, peakParticles: 4 }, { firstActive: 20, peakParticles: 0 }]) };
  assert.match(driftFloor({ recipe: { kind: "field" }, base, case: vanished, ssim: {}, inspectDiffEmpty: false, delayTarget: 1 }).refusals.join(), /vanished/);
  const none = driftFloor({ recipe: { kind: "seed" }, base, case: base, ssim: { 0: 1, 15: 1, 30: 1 }, inspectDiffEmpty: true });
  assert.match(none.refusals.join(), /not eligible/);
  const sub = driftFloor({ recipe: { kind: "field" }, base, case: kase, ssim: { 0: 1, 15: 1, 30: 1 }, inspectDiffEmpty: false, subjective: true });
  assert.match(sub.warnings.join(), /blind pass decides/);
});

test("draw order: identical mapped telemetry, image candidates only where both roots are live", () => {
  const recipe = { kind: "permutation", map: [1, 0], relation: { front: { index: 0, name: "B" }, back: { index: 1, name: "A" } } };
  const base = { capture: capture([0, 10, 20], [0, 20]), lines: [line(0, [1, 0]), line(10, [2, 3]), line(20, [0, 2])] };
  const kase = { capture: capture([0, 10, 20], [0, 20]), lines: [line(0, [0, 1]), line(10, [3, 2]), line(20, [2, 0])] };
  const d = driftFloor({ recipe, base, case: kase, ssim: { 0: 1, 10: 0.9, 20: 1 }, inspectDiffEmpty: false });
  assert.equal(d.eligible, true);
  assert.deepEqual(d.candidateImage, [10]);
  assert.deepEqual(d.candidateTelemetry, []);
  const stochastic = { ...kase, lines: [line(0, [0, 1]), line(10, [3, 1]), line(20, [2, 0])] };
  assert.match(driftFloor({ recipe, base, case: stochastic, ssim: {}, inspectDiffEmpty: false }).refusals.join(), /mapped telemetry differs/);
});

test("a frozen window frame must come from its candidate", () => {
  assert.deepEqual(windowErrors({ image: [15], telemetry: [31] }, { image: [15], telemetry: [15, 31] }), []);
  assert.match(windowErrors({ image: [30] }, { image: [15], telemetry: [] }).join(), /image frame 30/);
});
