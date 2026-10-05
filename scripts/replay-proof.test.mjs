import test from "node:test";
import assert from "node:assert/strict";
import { join, resolve } from "node:path";
import { checkReport, checkCaptureFiles, checkedFrames, SCENARIOS, scenarioFor, validateGeneratedFixture, TEXTURES, resolveReplayGameRoots, findMegAsset } from "./replay-proof.mjs";
import { createHash } from "node:crypto";

function fixture(definition = SCENARIOS[0]) {
  const scenario = scenarioFor(definition);
  const geometry = { rt: [100, 100], viewport: [0, 0, 100, 100], crop: [0, 0, 100, 100] };
  const profile = {
    camera: { position: [0, -250, 125], target: [0, 0, 0], up: [0, 0, 1] },
    background: 0, ambient: [0, 0, 0, 1], lights: Array.from({ length: 3 }, () => ({
      diffuse: [1, 1, 1, 1], specular: [0, 0, 0, 0], position: [0, 0, 1, 0], direction: [0, 0, -1, 0],
    })), wind: [0, 0, 0], gravity: [0, 0, -1], ground: false, bloom: false, bloomStrength: 0,
    bloomCutoff: Math.fround(0.9), bloomSize: Math.fround(0.1), heatDebug: false, msaa: 0,
    skydomeSlot: 1, embeddedSkydome: true, primaryDome: "", secondaryDome: "",
    primaryDomeGpu: false, secondaryDomeGpu: false, skydomeMeshGpu: true,
  };
  const frames = checkedFrames(scenario.maxFrames);
  const firstEmission = definition.file === "bump-asteroids.alo" ? 30 : 0;
  function sample(frame, limit = 10000, stride = 1, reseeded = false) {
    const particles = frame < firstEmission ? 0 : limit === 1000 ? 1000 : definition.guard ? 2048 : 2;
    return {
      frame, png: frame % stride === 0 ? `f${frame}.png` : "", sha256: frame % stride === 0 ? "a".repeat(64) : "",
      draws: limit === 1000 ? 1000 + frame : 10000 + frame, state: 100 + frame + (reseeded ? 1 : 0),
      instances: 1, particles, emitters: 1, guard: { enabled: true, limit, estimate: 0 },
      geometry: structuredClone(geometry), profile: structuredClone(profile),
      instance: { position: scenario.spawnPosition.map((v, i) => v + scenario.spawnVelocity[i] * frame / 60), age: Math.fround(frame / 60) },
      completedUpdates: frame + 1,
    };
  }
  function run(limit = 10000, stride = 1, reseeded = false) {
    const particles = limit === 1000 ? 1000 : definition.guard ? 2048 : 2;
    return {
      frames: frames.map((frame) => sample(frame, limit, stride, reseeded)),
      particles: Array.from({ length: scenario.maxFrames + 1 }, (_, frame) => frame < firstEmission ? 0 : particles),
      peak: particles, peakFrame: firstEmission,
      estimateRefusals: 0, runtimeSuppressionUpdates: limit === 1000 ? 1 : 0,
    };
  }
  const E1 = run();
  const sought = () => ({ frames: structuredClone(E1.frames) });
  const experiments = {
    E1, E2: sought(), E3: run(), E4: { ...sought(), xmlParses: 7, catalogBuilds: 7, catalogObjects: 7, crtCalls: 700 },
    E5: { ...sought(), editResponse: { ok: true, data: { applied: ["lifetime"], skipped: [] } },
      undoResponse: { ok: true, data: { applied: true } }, documentChanged: true, documentRestored: true },
    E6: { ...sought(), newSeed: (scenario.seed + 1) >>> 0, newSeedRun: run(10000, 1, true) },
    E7: { runs: [1, 2, 3, 4].map((stride) => ({ ...run(10000, stride), stride })) },
    E8: { ...sought(), equivalent: true }, E9: { applicable: !!definition.guard },
    E10: { timings: frames.flatMap((frame) => [true, false].map((renderIntermediate) => ({
      frame, renderIntermediate, samplesMs: [10, 11, 12, 13, 14], medianMs: 12, maximumMs: 14,
    }))) }, E11: { applicable: !!definition.weather, ...(definition.weather ? sought() : {}) },
  };
  if (definition.guard) Object.assign(experiments.E9, { high: run(), highAgain: run(), low: run(1000), lowRepeat: run(1000) });
  const report = {
    schemaVersion: 1, exitCode: 0, failure: null, scenario, checkedFrames: frames,
    wallMs: 1000, limits: "one spawner-owned instance; fixed golden camera/environment; default wind/gravity",
    geometry, profile, experiments,
    preparation: Array.from({ length: 2 }, () => ({ settled: true, passes: [{ applyCount: 1, pending: false }, { applyCount: 1, pending: false }] })),
    resize: { before: 0, after: 0, unchanged: true },
    ignoredSettingsPushes: { overloadGuard: 1, msaaLevel: 1, estimatedLoad: 3 },
    positive: { empty: { png: "empty.png", sha256: "b".repeat(64) }, peakCapture: sample(E1.peakFrame),
      firstParticleFrame: E1.frames.find((v) => v.particles > 0).frame, firstParticleDifferentPixels: 1, peakDifferentPixels: 1 },
    acceptance: { latencyRequired: !!definition.latencyRequired, thresholdMs: 250, accepted: true },
  };
  return { report, scenario, definition };
}

for (const definition of SCENARIOS) test(`good report passes: ${definition.name}`, () => {
  const f = fixture(definition);
  assert.deepEqual(checkReport(f.report, f.definition, f.scenario), { errors: [], acceptanceBlockers: [] });
});

const mutations = [
  ["missing wall time", (r) => delete r.wallMs],
  ["missing proof limits", (r) => delete r.limits],
  ["one-frame hash mismatch", (r) => r.experiments.E2.frames[1].sha256 = "c".repeat(64)],
  ["draw-count mismatch", (r) => ++r.experiments.E2.frames[1].draws],
  ["RNG state mismatch", (r) => ++r.experiments.E3.frames[1].state],
  ["missing experiment", (r) => delete r.experiments.E4],
  ["wrong guard", (r) => r.experiments.E1.frames[0].guard.limit = 2000],
  ["wrong estimate", (r) => r.experiments.E1.frames[0].guard.estimate = 1],
  ["zero spawn", (r) => r.experiments.E1.frames[0].instances = 0],
  ["no particles by frame 60", (r) => {
    r.experiments.E1.frames.filter((v) => v.frame <= 60).forEach((v) => v.particles = 0);
    r.experiments.E1.particles.fill(0, 0, 61);
    r.experiments.E1.peakFrame = 61;
    r.positive.firstParticleFrame = 300;
    r.positive.peakCapture.frame = 61;
  }],
  ["zero draws", (r) => r.experiments.E1.frames[1].draws = 0],
  ["missing empty capture", (r) => delete r.positive.empty],
  ["missing peak capture", (r) => delete r.positive.peakCapture],
  ["missing first-particle frame", (r) => delete r.positive.firstParticleFrame],
  ["wrong first-particle frame", (r) => r.positive.firstParticleFrame = 1],
  ["missing first-particle pixel count", (r) => delete r.positive.firstParticleDifferentPixels],
  ["zero first-particle pixel count", (r) => r.positive.firstParticleDifferentPixels = 0],
  ["missing ignored push counts", (r) => delete r.ignoredSettingsPushes],
  ...["overloadGuard", "msaaLevel", "estimatedLoad"].flatMap((kind) => [
    [`missing ignored ${kind} count`, (r) => delete r.ignoredSettingsPushes[kind]],
    [`negative ignored ${kind} count`, (r) => r.ignoredSettingsPushes[kind] = -1],
    [`fractional ignored ${kind} count`, (r) => r.ignoredSettingsPushes[kind] = 0.5],
  ]),
  ["missing peak pixel count", (r) => delete r.positive.peakDifferentPixels],
  ["zero peak pixel count", (r) => r.positive.peakDifferentPixels = 0],
  ["zero XML work", (r) => r.experiments.E4.xmlParses = 0],
  ["zero catalog work", (r) => r.experiments.E4.catalogBuilds = 0],
  ["empty catalog", (r) => r.experiments.E4.catalogObjects = 0],
  ["zero CRT work", (r) => r.experiments.E4.crtCalls = 0],
  ["E9 skipped", (r) => r.experiments.E9.applicable = false, "guard"],
  ["E9 missing suppression", (r) => delete r.experiments.E9.low.runtimeSuppressionUpdates, "guard"],
  ["E9 zero suppression", (r) => r.experiments.E9.low.runtimeSuppressionUpdates = 0, "guard"],
  ["E9 missing peak", (r) => delete r.experiments.E9.high.peak, "guard"],
  ["E9 high peak too low", (r) => r.experiments.E9.high.peak = 1000, "guard"],
  ["E9 estimate refusal", (r) => r.experiments.E9.low.estimateRefusals = 1, "guard"],
  ["E9 restoration differs", (r) => ++r.experiments.E9.highAgain.frames[1].draws, "guard"],
  ["E11 missing", (r) => delete r.experiments.E11, "weather"],
  ["E11 skipped", (r) => r.experiments.E11.applicable = false, "weather"],
  ["E11 wrong restored camera", (r) => r.experiments.E11.frames[1].profile.camera.position[0] = 100, "weather"],
  ["failed hook", (r) => { r.exitCode = 10; r.failure = { hook: "update", frame: 1, message: "failed" }; }],
  ["geometry drift", (r) => ++r.experiments.E2.frames[1].geometry.crop[0]],
  ["missing resize measurement", (r) => delete r.resize],
  ["resize incidental update", (r) => ++r.resize.after],
  ["incomplete update", (r) => --r.experiments.E2.frames[1].completedUpdates],
  ["unsettled preparation", (r) => r.preparation[0].passes[1].pending = true],
  ["wrong phase acceptance", (r) => r.acceptance.accepted = false],
  ["missing frame-300 measurement claimed accepted", (r) => r.experiments.E10.timings = r.experiments.E10.timings.filter((t) => t.frame !== 300)],
  ["bad latency statistic", (r) => r.experiments.E10.timings[0].medianMs = 900],
  ["no per-frame population", (r) => delete r.experiments.E1.particles],
  ["wrong velocity frame 0", (r) => r.experiments.E1.frames[0].instance.age = 1, "velocity"],
  ["wrong velocity frame 1", (r) => r.experiments.E1.frames[1].instance.position[0] = 0, "velocity"],
  ["missing stride", (r) => r.experiments.E7.runs.pop()],
  ["missing captured stride image", (r) => r.experiments.E7.runs[1].frames[0].png = ""],
  ["false E8 finding", (r) => r.experiments.E8.equivalent = false],
];
for (const [name, mutate, variant] of mutations) test(`checker rejects ${name}`, () => {
  const definition = variant ? SCENARIOS.find((s) => s[variant]) : SCENARIOS[0];
  const f = fixture(definition);
  mutate(f.report);
  const errors = checkReport(f.report, f.definition, f.scenario).errors;
  assert.ok(errors.length > 0);
  if (name === "no particles by frame 60")
    assert.ok(errors.includes("missing or incorrect first checked particle frame by frame 60"));
  if (name === "zero first-particle pixel count")
    assert.ok(errors.includes("missing or zero first-particle / peak differing pixels"));
});

test("delayed emission uses checked frame 60 for pixels while retaining the whole-run peak", () => {
  const f = fixture(SCENARIOS.find((s) => s.file === "bump-asteroids.alo"));
  assert.equal(f.report.experiments.E1.frames[1].particles, 0);
  assert.equal(f.report.positive.firstParticleFrame, 60);
  assert.equal(f.report.positive.peakCapture.frame, 30);
  assert.deepEqual(checkReport(f.report, f.definition, f.scenario).errors, []);
});

test("proof need not receive any settings pushes", () => {
  const f = fixture();
  f.report.ignoredSettingsPushes = { overloadGuard: 0, msaaLevel: 0, estimatedLoad: 0 };
  assert.deepEqual(checkReport(f.report, f.definition, f.scenario).errors, []);
});

test("E8 differences are findings when equivalence is correctly recorded", () => {
  const f = fixture();
  f.report.experiments.E8.frames[1].sha256 = "c".repeat(64);
  f.report.experiments.E8.equivalent = false;
  assert.deepEqual(checkReport(f.report, f.definition, f.scenario).errors, []);
});

test("a naturally expired single spawn may be absent at later checked frames", () => {
  const f = fixture();
  function expire(value) {
    if (!value || typeof value !== "object") return;
    if (value.frame >= 60 && "instances" in value) {
      value.instances = 0; value.emitters = 0; value.particles = 0; value.instance = null;
    }
    if (Array.isArray(value.particles)) value.particles.fill(0, 60);
    for (const child of Object.values(value)) expire(child);
  }
  expire(f.report);
  assert.deepEqual(checkReport(f.report, f.definition, f.scenario).errors, []);
});

test("missing frame-300 timing blocks acceptance but does not fail the deterministic lane", () => {
  const f = fixture();
  f.report.experiments.E10.timings = f.report.experiments.E10.timings.filter((t) => t.frame !== 300);
  f.report.acceptance.accepted = false;
  const result = checkReport(f.report, f.definition, f.scenario);
  assert.deepEqual(result.errors, []);
  assert.ok(result.acceptanceBlockers.length > 0);
});

test("250 ms threshold is inclusive; a miss blocks phase acceptance only", () => {
  for (const ms of [250, 251]) {
    const f = fixture();
    const t = f.report.experiments.E10.timings.find((v) => v.frame === 300 && v.renderIntermediate);
    t.samplesMs = Array(5).fill(ms); t.medianMs = ms; t.maximumMs = ms;
    f.report.acceptance.accepted = ms <= 250;
    const result = checkReport(f.report, f.definition, f.scenario);
    assert.deepEqual(result.errors, []);
    assert.equal(result.acceptanceBlockers.length > 0, ms > 250);
  }
});

test("generated fixture attestation validates every persisted field", () => {
  for (const kind of ["weather", "guard"]) {
    const weather = kind === "weather";
    const fields = { kind, reloaded: true, colorTexture: TEXTURES[0], normalTexture: TEXTURES[1],
      useBursts: true, nBursts: 1, nParticlesPerBurst: weather ? 64 : 2048, nParticlesPerSecond: weather ? 64 : 0,
      initialDelay: 0, skipTime: 0, burstDelay: 10000, lifetime: 600, gravity: 0, isWeatherParticle: weather,
      weatherCubeSize: weather ? 128 : 0, weatherCubeDistance: weather ? 200 : 0, positionGroup: 1, scale: 8, alpha: 1 };
    assert.deepEqual(validateGeneratedFixture(fields, kind), []);
    for (const key of Object.keys(fields)) {
      const bad = { ...fields }; delete bad[key];
      assert.deepEqual(validateGeneratedFixture(bad, kind), [`fixture ${kind}: ${key} differs after reload`]);
    }
  }
});

test("game lookup uses saved GameDataPath, sibling, cwd, and available MegaFiles entries", () => {
  const game = resolve("game", "GameData"), sibling = resolve("game", "corruption"), cwd = resolve("empty");
  const options = { savedPath: game, cwd, exists: (p) => p === sibling || p.endsWith("config.meg"), size: () => 8,
    read: (p) => { if (p.startsWith(cwd)) throw new Error("absent"); return Buffer.from("<Mega_Files><File>Data\\config.meg</File></Mega_Files>"); } };
  assert.deepEqual(resolveReplayGameRoots(options), [game, sibling]);
  assert.deepEqual(resolveReplayGameRoots({ ...options, overrides: [sibling] }), [sibling]);
  assert.deepEqual(resolveReplayGameRoots({ ...options, exists: () => false }), []);
  assert.deepEqual(resolveReplayGameRoots({ ...options, size: () => 0 }), []);
  assert.deepEqual(resolveReplayGameRoots({ ...options, read: () => Buffer.from("<wrong/>") }), []);
  assert.equal(join(game, "Data", "MegaFiles.xml").includes("MegaFiles.xml"), true);
});

test("actual capture files must exist and match the measured PNG hash", () => {
  const bytes = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10, 1, 2, 3]);
  const report = { positive: { empty: { png: "empty.png", sha256: createHash("sha256").update(bytes).digest("hex") } } };
  assert.deepEqual(checkCaptureFiles(report, () => bytes), []);
  assert.equal(checkCaptureFiles(report, () => { throw new Error("missing"); }).length, 1);
  assert.equal(checkCaptureFiles(report, () => Buffer.from("wrong image")).length, 1);
  const bad = structuredClone(report); bad.positive.empty.sha256 = "a".repeat(64);
  assert.equal(checkCaptureFiles(bad, () => bytes).length, 1);
});

test("optional stock lookup validates the existing MEG layout and bounds", () => {
  // Metadata only, in memory: this is not a fixture content root or an archive.
  const name = "DATA\\ART\\MODELS\\P_LARGE_EXPLOSION.ALO";
  const at = 8 + 2 + name.length;
  const metadata = Buffer.alloc(at + 40);
  metadata.writeUInt32LE(1, 0); metadata.writeUInt32LE(2, 4);
  metadata.writeUInt16LE(name.length, 8); metadata.write(name, 10, "latin1");
  for (const offset of [at, at + 20]) {
    metadata.writeUInt32LE(0x1a72b833, offset);
    metadata.writeUInt32LE(12, offset + 8);
    metadata.writeUInt32LE(metadata.length, offset + 12);
  }
  const size = metadata.length + 12;
  assert.deepEqual(findMegAsset(metadata, name.toLowerCase(), size), { start: metadata.length, size: 12 });
  assert.equal(findMegAsset(metadata, "ABSENT", size), null);
  assert.equal(findMegAsset(metadata.subarray(0, 7), name, size), null);
  assert.equal(findMegAsset(metadata.subarray(0, at + 39), name, size), null);
  assert.equal(findMegAsset(metadata, name, size - 1), null);
  for (const [offset, value] of [[0, 1048577], [4, 1048577], [at + 20 + 16, 1], [at + 20 + 12, size + 1]]) {
    const bad = Buffer.from(metadata); bad.writeUInt32LE(value, offset);
    assert.equal(findMegAsset(bad, name, size), null);
  }
  const bad = Buffer.from(metadata); bad.writeUInt16LE(32769, 8);
  assert.equal(findMegAsset(bad, name, size), null);
  const wrongCrc = Buffer.from(metadata);
  wrongCrc.writeUInt32LE(0, at); wrongCrc.writeUInt32LE(0, at + 20);
  assert.equal(findMegAsset(wrongCrc, name, size), null);
});
