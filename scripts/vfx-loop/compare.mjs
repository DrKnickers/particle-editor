// [vfx-loop] Before/after comparison of two observation bundles (design §13).
//
// The metrics here enforce the task's explicit budgets and flag drift. None of
// them ranks one effect over the other (§13.3): that judgment is the human's,
// so no output carries a score, a winner, or a preference.
//
//   comparison/
//     paired-filmstrip.png       each tile = baseline frame over candidate frame
//     selected-frame-pairs/      frame-NNNNN.baseline.png / .candidate.png
//     metrics.json               budgets (task + technical) and drift flags
//     telemetry-delta.json       whole-effect and per-emitter series and lifecycle shifts
//     random-schedule-note.json  where the two random schedules diverge (§9.8)
//     review.json                the frame pairs and the technical gate, for a human
import * as fs from "node:fs";
import { join, resolve } from "node:path";
import { deflateSync } from "node:zlib";
import { isDeepStrictEqual } from "node:util";
import { checkAssembledBundle, spawnWithDeadline } from "../observe.mjs";

const PNG = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
const serialise = (value) => JSON.stringify(value, null, 2) + "\n";
const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });
const stem = (frame) => `frame-${String(frame).padStart(5, "0")}`;

// Budgetable values, each one number per run (see summarise).
export const METRICS = ["peakParticles", "finalFrame", "firstActive", "lastActive",
  "runtimeGuardRefusals", "hardGuardRefusals", "indexCapRefusals"];

// Always checked: a candidate that trips a guard the baseline did not is a
// technical regression (§25 "no technical regression"), whatever the task says.
export const TECHNICAL_BUDGETS = ["runtimeGuardRefusals", "hardGuardRefusals", "indexCapRefusals"]
  .map((metric) => ({ id: `no-new-${metric}`, metric, maxDelta: 0 }));

// Everything §13.1 requires to match, as the capture records it. The source
// path and hash differ on purpose; the camera, environment, wind and gravity
// are fields of the fixed observation profile.
export const ALIGNED = ["tool", "spec (except scenario.aloPath)", "random", "profile", "geometry", "render",
  "content stack", "assets both runs loaded"];

// An asset both runs loaded must have come from the same file with the same
// bytes; what each effect asks for may differ, so assets one run alone loaded
// and the emitter attestation are not compared.
function sharedAssets(baseline, candidate) {
  const index = (capture) => new Map((capture.content?.loadedAssets ?? [])
    .map((v) => [`${v.type}:${v.identity}`, { outcome: v.outcome, winner: v.winner, sha256: v.sha256 }]));
  const a = index(baseline), b = index(candidate);
  const shared = [...a.keys()].filter((key) => b.has(key)).sort();
  return [Object.fromEntries(shared.map((key) => [key, a.get(key)])), Object.fromEntries(shared.map((key) => [key, b.get(key)]))];
}

export function alignmentDiff(baseline, candidate) {
  const [baselineAssets, candidateAssets] = sharedAssets(baseline, candidate);
  const project = (capture, assets) => {
    const spec = structuredClone(capture.spec);
    if (spec?.scenario) delete spec.scenario.aloPath;
    const content = capture.content ?? {};
    return { tool: capture.tool, spec, random: capture.random, profile: capture.profile,
      geometry: capture.geometry, render: capture.render,
      content: { gameRoots: content.gameRoots, selectedModPaths: content.selectedModPaths, sha256: content.sha256 },
      loadedAssets: assets };
  };
  const differences = [];
  const plain = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
  (function walk(a, b, path) {
    if (plain(a) && plain(b)) {
      for (const key of [...new Set([...Object.keys(a), ...Object.keys(b)])].sort())
        walk(a[key], b[key], path ? `${path}.${key}` : key);
    } else if (!isDeepStrictEqual(a, b)) differences.push({ path, baseline: a ?? null, candidate: b ?? null });
  })(project(baseline, baselineAssets), project(candidate, candidateAssets), "");
  return differences;
}

export function summarise(capture, samples) {
  const life = capture.lifecycle;
  return { status: life.status, peakParticles: life.peak == null ? 0 : samples[life.peak].particles,
    finalFrame: life.finalFrame, firstActive: life.firstActive, lastActive: life.lastActive,
    runtimeGuardRefusals: life.limits.runtimeGuard.total, hardGuardRefusals: life.limits.hardGuard.total,
    indexCapRefusals: life.limits.indexCap.total };
}

export function checkBudgets(budgets, baseline, candidate) {
  return [...budgets.map((v) => ({ ...v, source: "task" })), ...TECHNICAL_BUDGETS.map((v) => ({ ...v, source: "technical" }))]
    .map(({ id, metric, maxDelta, source }) => {
      const before = baseline[metric], after = candidate[metric];
      const known = Number.isFinite(before) && Number.isFinite(after);
      // A value one run lacks (no active frame) cannot prove the budget held.
      return { id, metric, source, baseline: before, candidate: after,
        delta: known ? after - before : null, maxDelta, pass: known && after - before <= maxDelta };
    });
}

const shift = (before, after) => ({ baseline: before, candidate: after,
  delta: Number.isFinite(before) && Number.isFinite(after) ? after - before : null });

// Per-emitter landmark shifts and series, by authored index, when both bundles
// carry per-emitter telemetry for the same emitter list. A patch edits emitters
// in place, so the lists normally match; anything else is reported, not guessed.
function emitterDelta(baselineSamples, candidateSamples, baselineEmitters, candidateEmitters) {
  if (!Array.isArray(baselineEmitters) || !Array.isArray(candidateEmitters))
    return { emitters: null, reason: "a bundle has no per-emitter telemetry" };
  if (baselineEmitters.length !== candidateEmitters.length ||
      baselineEmitters.some((v, i) => v.name !== candidateEmitters[i].name))
    return { emitters: null, reason: "the two bundles list different emitters" };
  return { emitters: baselineEmitters.map((before, index) => {
    const after = candidateEmitters[index];
    return { index, name: before.name,
      lifecycle: Object.fromEntries(["firstActive", "peakFrame", "lastActive", "peakParticles"]
        .map((key) => [key, shift(before[key], after[key])])),
      series: { baseline: baselineSamples.map((v) => v.perEmitter[index]), candidate: candidateSamples.map((v) => v.perEmitter[index]) } };
  }) };
}

export function telemetryDelta(baselineSamples, candidateSamples, baseline, candidate, baselineEmitters, candidateEmitters) {
  const series = {};
  for (const field of ["particles", "emitters", "instances", "rngDrawCount"])
    series[field] = { baseline: baselineSamples.map((v) => v[field]), candidate: candidateSamples.map((v) => v[field]) };
  const lifecycle = {};
  for (const key of METRICS) lifecycle[key] = shift(baseline[key], candidate[key]);
  lifecycle.status = { baseline: baseline.status, candidate: candidate.status };
  const { emitters, reason } = emitterDelta(baselineSamples, candidateSamples, baselineEmitters, candidateEmitters);
  const ends = "a series ends where its run ended.";
  return { schemaVersion: 1, scope: emitters ? "per-emitter" : "engine-wide",
    note: emitters
      ? `Whole-effect totals and each emitter's own series per simulated frame, by authored index; ${ends}`
      : `Whole-effect totals per simulated frame only, because ${reason}; ${ends}`,
    lifecycle, series, emitters };
}

export function randomSchedule(baselineCapture, candidateCapture, baselineSamples, candidateSamples) {
  const compared = Math.min(baselineSamples.length, candidateSamples.length);
  let first = null;
  for (let frame = 0; frame < compared && first === null; ++frame) {
    const a = baselineSamples[frame], b = candidateSamples[frame];
    if (a.rngDrawCount !== b.rngDrawCount || a.rngState !== b.rngState) first = frame;
  }
  const random = baselineCapture.random;
  return { schemaVersion: 1, algorithm: random.algorithm ?? null, algorithmVersion: random.algorithmVersion,
    seed: random.seed, firstDivergentFrame: first, comparedFrames: compared,
    baselineFrames: baselineSamples.length, candidateFrames: candidateSamples.length,
    baselineDrawsAtEnd: baselineSamples.at(-1)?.rngDrawCount ?? null,
    candidateDrawsAtEnd: candidateSamples.at(-1)?.rngDrawCount ?? null,
    note: first === null
      ? `The random schedules match over the ${compared} frames both runs simulated.`
      : `The random schedule diverges at frame ${first}. From there the shared seed gives no particle-for-particle correspondence (design §9.8): compare the runs as two samples, not as matched particles.` };
}

// Pair by frame number over both runs' selected frames. A frame one run never
// captured is shown as ended (past its final frame) or not-selected, never dropped.
export function pairFrames(baselineCapture, candidateCapture) {
  const side = (capture, frame) => {
    const found = capture.frames.find((v) => v.frame === frame);
    if (found) return { state: "captured", png: found.png, reasons: found.reasons };
    return { state: frame > capture.lifecycle.finalFrame ? "ended" : "not-selected" };
  };
  const frames = [...new Set([...baselineCapture.frames, ...candidateCapture.frames].map((v) => v.frame))].sort((a, b) => a - b);
  return frames.map((frame) => ({ frame, baseline: side(baselineCapture, frame), candidate: side(candidateCapture, frame) }));
}

export function readIhdr(bytes) {
  if (!bytes.subarray(0, 8).equals(PNG) || bytes.toString("latin1", 12, 16) !== "IHDR") throw fail("not a PNG with a leading IHDR");
  return { width: bytes.readUInt32BE(16), height: bytes.readUInt32BE(20), bitDepth: bytes[24], colorType: bytes[25] };
}

const CRC = Array.from({ length: 256 }, (_, n) => {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c >>> 0;
});
function chunk(type, data) {
  const body = Buffer.concat([Buffer.from(type, "latin1"), data]);
  let crc = 0xffffffff;
  for (const byte of body) crc = CRC[(crc ^ byte) & 0xff] ^ (crc >>> 8);
  const length = Buffer.alloc(4), tail = Buffer.alloc(4);
  length.writeUInt32BE(data.length); tail.writeUInt32BE((crc ^ 0xffffffff) >>> 0);
  return Buffer.concat([length, body, tail]);
}

// A flat dark tile with the real frames' exact IHDR: the concat demuxer and
// tile filter need every input frame to share size and pixel format.
export function placeholderPng({ width, height, bitDepth, colorType }) {
  const channels = { 0: 1, 2: 3, 4: 2, 6: 4 }[colorType];
  if (bitDepth !== 8 || !channels) throw fail(`unsupported frame PNG format: bit depth ${bitDepth}, colour type ${colorType}`);
  const pixel = Buffer.alloc(channels, 0x30);
  if (colorType === 4 || colorType === 6) pixel[channels - 1] = 255;
  const row = Buffer.concat([Buffer.from([0]), Buffer.concat(Array(width).fill(pixel))]);
  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0); header.writeUInt32BE(height, 4); header[8] = bitDepth; header[9] = colorType;
  return Buffer.concat([PNG, chunk("IHDR", header), chunk("IDAT", deflateSync(Buffer.concat(Array(height).fill(row)))),
    chunk("IEND", Buffer.alloc(0))]);
}

const concatLine = (path) => `file '${resolve(path).replaceAll("\\", "/").replaceAll("'", "'\\''")}'\n`;

export function buildPairedFilmstripArgs({ baselineList, candidateList, out, count }) {
  if (!Number.isInteger(count) || count < 1) throw fail("a paired filmstrip needs at least one frame pair");
  const filter = count < 2 ? "[0:v][1:v]vstack=inputs=2"
    : `[0:v][1:v]vstack=inputs=2,format=rgb24,tile=8x${Math.ceil(count / 8)}:nb_frames=${count}`;
  return ["-y", "-f", "concat", "-safe", "0", "-i", baselineList, "-f", "concat", "-safe", "0", "-i", candidateList,
    "-filter_complex", filter, "-frames:v", "1", "-c:v", "png", out];
}

export async function writeComparison(baselineDir, candidateDir, outDir, { budgets = [], run = spawnWithDeadline, io = fs } = {}) {
  const read = (directory, label) => {
    const checked = checkAssembledBundle(directory, { io });
    if (checked.errors.length) throw fail(`${label} observation is not trustworthy: ${checked.errors.join("; ")}`);
    const samples = io.readFileSync(join(directory, "telemetry/frames.jsonl"), "utf8").trim().split("\n").map(JSON.parse);
    return { capture: checked.capture, samples };
  };
  const baseline = read(baselineDir, "baseline"), candidate = read(candidateDir, "candidate");
  const differences = alignmentDiff(baseline.capture, candidate.capture);
  if (differences.length) throw fail(`observations are misaligned (§13.1); nothing compared: ${differences.map((d) =>
    `${d.path}: ${JSON.stringify(d.baseline)} vs ${JSON.stringify(d.candidate)}`).join("; ")}`, 7);
  if (io.existsSync(outDir) && io.readdirSync(outDir).length) throw fail(`comparison directory must be absent or empty: ${outDir}`, 13);
  const pairDir = join(outDir, "selected-frame-pairs");
  io.mkdirSync(pairDir, { recursive: true });

  const pairs = pairFrames(baseline.capture, candidate.capture);
  const firstFrame = baseline.capture.frames.find((v) => v.frame === 0) ?? baseline.capture.frames[0];
  const placeholder = placeholderPng(readIhdr(io.readFileSync(join(baselineDir, firstFrame.png))));
  const lists = { baseline: "", candidate: "" };
  for (const pair of pairs) {
    for (const [label, directory] of [["baseline", baselineDir], ["candidate", candidateDir]]) {
      const target = join(pairDir, `${stem(pair.frame)}.${label}.png`);
      if (pair[label].state === "captured") io.copyFileSync(join(directory, pair[label].png), target);
      else io.writeFileSync(target, placeholder);
      pair[label].pair = `selected-frame-pairs/${stem(pair.frame)}.${label}.png`;
      lists[label] += concatLine(target);
    }
  }
  const baselineList = join(outDir, "baseline.concat.txt"), candidateList = join(outDir, "candidate.concat.txt");
  io.writeFileSync(baselineList, lists.baseline); io.writeFileSync(candidateList, lists.candidate);
  const filmstrip = join(outDir, "paired-filmstrip.png");
  try {
    const result = await run("ffmpeg", buildPairedFilmstripArgs({ baselineList, candidateList, out: filmstrip, count: pairs.length }),
      { timeoutMs: 120000 });
    if (result.code !== 0) throw fail(`ffmpeg exit ${result.code}: ${result.stderr}`);
  } finally {
    io.rmSync(baselineList, { force: true }); io.rmSync(candidateList, { force: true });
  }
  if (!io.readFileSync(filmstrip).subarray(0, 8).equals(PNG)) throw fail("paired filmstrip has a bad PNG signature");

  const before = summarise(baseline.capture, baseline.samples), after = summarise(candidate.capture, candidate.samples);
  const checked = checkBudgets(budgets, before, after);
  const images = pairs.filter((p) => p.baseline.state === "captured" && p.candidate.state === "captured");
  const hash = (capture, frame) => capture.frames.find((v) => v.frame === frame).sha256;
  const identical = images.filter((p) => hash(baseline.capture, p.frame) === hash(candidate.capture, p.frame)).map((p) => p.frame);
  const schedule = randomSchedule(baseline.capture, candidate.capture, baseline.samples, candidate.samples);
  const technicalGate = { pass: checked.every((v) => v.pass), failed: checked.filter((v) => !v.pass).map((v) => v.id) };
  const metrics = { schemaVersion: 1,
    boundary: "Budgets and drift flags only (design §13.3). No value here ranks the two effects; the human decides which to keep.",
    budgets: checked, budgetsPass: technicalGate.pass,
    drift: { lifecycleStatus: { baseline: before.status, candidate: after.status, changed: before.status !== after.status },
      identicalSelectedFrames: identical, firstImageDivergence: images.find((p) => !identical.includes(p.frame))?.frame ?? null,
      firstRandomDivergence: schedule.firstDivergentFrame } };
  const review = { schemaVersion: 1,
    advisory: "Evidence for a human decision. Nothing here ranks the two effects (design §13.3); the human decides which to keep.",
    alignment: { aligned: true, compared: ALIGNED }, technicalGate, pairs, decision: null };
  io.writeFileSync(join(outDir, "metrics.json"), serialise(metrics));
  io.writeFileSync(join(outDir, "telemetry-delta.json"), serialise(telemetryDelta(baseline.samples, candidate.samples, before, after,
    baseline.capture.lifecycle.emitters, candidate.capture.lifecycle.emitters)));
  io.writeFileSync(join(outDir, "random-schedule-note.json"), serialise(schedule));
  io.writeFileSync(join(outDir, "review.json"), serialise(review));
  return { metrics, review, schedule, summaries: { baseline: before, candidate: after } };
}
