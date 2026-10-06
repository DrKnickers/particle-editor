// Pins measured 2026-10-06 on the owner's native build (node scripts/observe-lane.mjs --measure).
import * as fs from "node:fs";
import { dirname, join, resolve, relative, isAbsolute } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { observe, spawnWithDeadline, bundleFiles } from "./observe.mjs";
import { scenarioFor, resolveReplayGameRoots, TEXTURES, validateGeneratedFixture } from "./replay-proof.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const scratch = join(repoRoot, "tests/.tmp/observe");
export const PINNED = {
  "singleton-emitter": { status: "capped", finalFrame: 300, runtimeGuard: 0, hardGuard: 0, indexCap: 0 },
  "bump-cutout": { status: "capped", finalFrame: 300, runtimeGuard: 0, hardGuard: 0, indexCap: 0, firstActive: 30 },
  "replay-weather": { status: "capped", finalFrame: 300, runtimeGuard: 0, hardGuard: 0, indexCap: 0 },
  "replay-guard": { status: "capped", finalFrame: 300, runtimeGuard: 1, hardGuard: 0, indexCap: 0 },
  "guard-short-runtime": { status: "spawn-limited", finalFrame: 44, runtimeGuard: 1, hardGuard: 0, indexCap: 0 },
  "guard-short-hard": { status: "spawn-limited", finalFrame: 14, runtimeGuard: 0, hardGuard: 1, indexCap: 0, hardFirstFrame: 0 },
  "child-starve": { status: "spawn-limited", finalFrame: 59, runtimeGuard: 400, hardGuard: 0, indexCap: 0 },
  "budget-bailout": { status: "spawn-limited", finalFrame: 94, runtimeGuard: 31, hardGuard: 0, indexCap: 0 },
  "budget-bailout-skip": { status: "spawn-limited", finalFrame: 79, runtimeGuard: 32, hardGuard: 0, indexCap: 0 },
  "index-cap": { status: "spawn-limited", finalFrame: 44, runtimeGuard: 0, hardGuard: 0, indexCap: 1 },
  "death-child": { status: "natural-end", finalFrame: 76, runtimeGuard: 0, hardGuard: 0, indexCap: 0, lastActive: 61 },
  "late-flash": { status: "natural-end", finalFrame: 90, runtimeGuard: 0, hardGuard: 0, indexCap: 0, firstActive: 0, lastActive: 75, flashOnset: 46 },
  "sparse-playback": { status: "capped", finalFrame: 300, runtimeGuard: 0, hardGuard: 0, indexCap: 0, playbackFrames: 301, playbackDuration: 5.016666666666667 },
  "quota-guard": { exitCode: 16 },
};

export function observationCases(fixtureDirectory = join(scratch, "fixtures")) {
  const make = (name, file, changes = {}) => ({ name, spec: {
    schemaVersion: 1,
    scenario: { ...scenarioFor({ file }), ...changes.scenario },
    lifecycle: { type: "oneshot", quietTailFrames: 15, ...changes.lifecycle },
    output: { strideInterval: 15, playbackStride: 0, maxBytes: 200 * 1024 * 1024, ...changes.output },
  } });
  const generated = (kind) => join(fixtureDirectory, `${kind}.alo`);
  const runtime = { overloadGuardLimit: 1000 };
  return [
    make("singleton-emitter", "singleton-emitter.alo"),
    make("bump-cutout", "bump-asteroids.alo"),
    make("replay-weather", generated("weather"), { lifecycle: { type: "continuous", warmupFrames: 60, reviewFrames: 240 } }),
    make("replay-guard", generated("guard"), { scenario: runtime }),
    make("guard-short-runtime", generated("guard-short"), { scenario: runtime }),
    make("guard-short-hard", generated("guard-short"), { scenario: { ...runtime, estimatedParticlesPerInstance: 2048 } }),
    make("child-starve", generated("child-starve"), { scenario: runtime }),
    make("budget-bailout", generated("budget-bailout"), { scenario: runtime }),
    make("budget-bailout-skip", generated("budget-bailout-skip"), { scenario: runtime }),
    make("index-cap", generated("index-cap"), { scenario: { overloadGuardEnabled: false } }),
    make("death-child", generated("death-child")),
    make("late-flash", generated("late-flash")),
    // maxFrames counts advances; frame zero makes 301 mapped images over 300 frames.
    make("sparse-playback", "singleton-emitter.alo", { output: { playbackStride: 1 } }),
    make("quota-guard", generated("guard"), { scenario: runtime, output: { playbackStride: 1, maxBytes: 1024 * 1024 } }),
  ];
}

export async function generateFixtures(exe, roots, directory, { run = spawnWithDeadline, fixtureContract, io = fs } = {}) {
  // The other PR C job owns this module. Keep imports lazy so pure script tests
  // can inject its contract before that job has landed.
  const contract = fixtureContract ?? await import("./observe-fixtures.mjs");
  const kinds = ["late-flash", "guard-short", "death-child", "child-starve", "budget-bailout", "budget-bailout-skip", "index-cap"];
  if (!Array.isArray(contract.OBSERVE_FIXTURE_KINDS) ||
      !isDeepStrictEqual([...contract.OBSERVE_FIXTURE_KINDS].sort(), [...kinds].sort()))
    throw new Error("observation fixture kinds differ from the PR C contract");
  io.mkdirSync(directory, { recursive: true });
  for (const kind of ["weather", "guard", ...kinds]) {
    const path = join(directory, `${kind}.alo`);
    // Refuse stale evidence even if a broken generator returns zero.
    io.rmSync(path, { force: true });
    const result = await run(exe, ["--gen-replay-proof-fixture", kind, path, ...roots], { timeoutMs: 660000 });
    if (result.code !== 0) throw new Error(`fixture ${kind}: exe exit ${result.code}: ${result.stderr}`);
    const line = result.stdout.match(/^\[replay-fixture\] (.+)\r?$/m)?.[1];
    const fields = line ? JSON.parse(line) : null;
    const problems = kinds.includes(kind) ? contract.validateObserveFixture(fields, kind) : validateGeneratedFixture(fields, kind);
    if (!io.existsSync(path) || problems.length) throw new Error(problems.join("; ") || `fixture ${kind}: missing output`);
  }
}

export function measuredValues(name, capture, samples, manifest) {
  if (name === "quota-guard") return { exitCode: capture.failure?.exit };
  const life = capture.lifecycle;
  const values = { status: life.status, finalFrame: life.finalFrame,
    runtimeGuard: life.limits.runtimeGuard.total, hardGuard: life.limits.hardGuard.total, indexCap: life.limits.indexCap.total };
  if (["bump-cutout", "late-flash"].includes(name)) values.firstActive = life.firstActive;
  if (["death-child", "late-flash"].includes(name)) values.lastActive = life.lastActive;
  if (name === "late-flash") values.flashOnset = samples.find((v, i) => i > 0 && v.particles > samples[i - 1].particles)?.frame ?? null;
  if (name === "guard-short-hard") values.hardFirstFrame = life.limits.hardGuard.firstFrame;
  if (name === "sparse-playback") {
    values.playbackFrames = manifest.deterministic.playback.frameCount;
    values.playbackDuration = manifest.deterministic.playback.duration;
  }
  return values;
}

export function checkCase(name, values, { pins = PINNED, measure = false } = {}) {
  const errors = [];
  const require = (ok, message) => { if (!ok) errors.push(`${name}: ${message}`); };
  const expectedStatus = ["replay-weather", "replay-guard"].includes(name) ? "capped"
    : ["death-child", "late-flash"].includes(name) ? "natural-end"
    : ["singleton-emitter", "bump-cutout", "sparse-playback", "quota-guard"].includes(name) ? null : "spawn-limited";
  if (expectedStatus) require(values.status === expectedStatus, `expected ${expectedStatus}, got ${values.status}`);
  const runtimeCases = ["replay-guard", "guard-short-runtime", "child-starve", "budget-bailout", "budget-bailout-skip"];
  if (runtimeCases.includes(name)) require(values.runtimeGuard > 0 && values.hardGuard === 0 && values.indexCap === 0, "expected runtime-only refusals");
  if (name === "guard-short-hard") require(values.hardGuard > 0 && values.hardFirstFrame === 0 && values.runtimeGuard === 0 && values.indexCap === 0, "expected frame-0 hard-only refusal");
  if (name === "index-cap") require(values.indexCap > 0 && values.runtimeGuard === 0 && values.hardGuard === 0, "expected index-only refusals with guard disabled");
  if (name === "death-child") require(values.lastActive >= 59, "death child must survive through frame 59");
  if (name === "late-flash") require(values.firstActive === 0 && Number.isInteger(values.flashOnset) && values.flashOnset > 0, "missing core at zero or later flash onset");
  if (name === "sparse-playback") require(values.finalFrame === 300 && values.playbackFrames === 301 && values.playbackDuration === 301 / 60, "stride-1 playback must span the 300-frame cap");
  if (name === "quota-guard") require(values.exitCode === 16, "expected quota exit 16");
  require(!!pins[name], "missing PINNED row; run node scripts/observe-lane.mjs --measure");
  for (const key of Object.keys(PINNED[name] ?? {})) {
    require(Object.hasOwn(values, key) && values[key] != null, `missing measurement ${key}`);
    if (!measure) {
      require(pins[name]?.[key] != null, `null PINNED.${name}.${key}; owner must run node scripts/observe-lane.mjs --measure`);
      if (pins[name]?.[key] != null) require(isDeepStrictEqual(values[key], pins[name][key]), `${key} differs: measured ${values[key]}, pinned ${pins[name][key]}`);
    }
  }
  return errors;
}

function cleanCase(path) {
  const rel = relative(scratch, resolve(path));
  if (!rel || rel.startsWith("..") || isAbsolute(rel)) throw new Error(`scratch cleanup escaped observe root: ${path}`);
  fs.rmSync(path, { recursive: true, force: true });
}

export async function runLane({ exe = join(repoRoot, "x64/Release/ParticleEditor.exe"), roots = [], measure = false,
  run = spawnWithDeadline, fixtureContract } = {}) {
  for (const texture of TEXTURES) if (!fs.existsSync(join(repoRoot, ...texture.split("\\")))) throw new Error(`missing committed texture ${texture}`);
  const fixtures = join(scratch, "fixtures"), cases = join(scratch, "cases");
  fs.mkdirSync(cases, { recursive: true });
  await generateFixtures(exe, roots, fixtures, { run, fixtureContract });
  const measured = {}, failures = [];
  for (const definition of observationCases(fixtures)) {
    const { name, spec } = definition;
    const directory = join(cases, name), specPath = join(cases, `${name}.observation.json`);
    cleanCase(directory); cleanCase(directory + "-repeat");
    fs.writeFileSync(specPath, JSON.stringify(spec, null, 2) + "\n");
    try {
      let manifest;
      if (name === "quota-guard") {
        // --repeat's success path cannot assemble a failed capture. Repeat the
        // native refusal explicitly, check its code, cleanup, and marker bytes.
        for (const out of [directory, directory + "-repeat"]) {
          const result = await run(exe, ["--observe", specPath, out, ...roots], { timeoutMs: 660000 });
          if (result.code !== 16) throw new Error(`quota-guard expected exe exit 16, got ${result.code}`);
          if (!isDeepStrictEqual(bundleFiles(out), ["capture.json", "logs/observe.log"])) throw new Error("quota-guard left outputs beyond capture.json and logs/observe.log");
          const failed = JSON.parse(fs.readFileSync(join(out, "capture.json"), "utf8"));
          if (failed.lifecycle?.status !== "failed" || failed.failure?.exit !== 16) throw new Error("quota-guard missing failed quota marker");
        }
        if (!fs.readFileSync(join(directory, "capture.json")).equals(fs.readFileSync(join(directory + "-repeat", "capture.json")))) throw new Error("quota-guard repeat capture differs");
      } else manifest = await observe(specPath, directory, { exe, roots, run, repeat: true });
      const capture = JSON.parse(fs.readFileSync(join(directory, "capture.json"), "utf8"));
      const samples = name === "quota-guard" ? [] : fs.readFileSync(join(directory, "telemetry/frames.jsonl"), "utf8").trim().split("\n").map(JSON.parse);
      const values = measuredValues(name, capture, samples, manifest);
      measured[name] = values;
      const errors = checkCase(name, values, { measure });
      if (name !== "quota-guard" && samples[1]?.spawnerIdle !== true) errors.push(`${name}: frame 1 spawner not idle`);
      if (name === "singleton-emitter") {
        const zero = capture.frames.find((v) => v.frame === 0);
        if (!fs.readFileSync(join(directory, zero.png)).equals(fs.readFileSync(join(repoRoot, "tests/goldens/render/singleton-emitter.png")))) errors.push("singleton frame 0 differs from render golden");
      }
      if (errors.length) throw new Error(errors.join("; "));
      console.log(`[observe] ${name}: ${measure ? "MEASURED" : "PASS"} ${JSON.stringify(values)}`);
    } catch (error) { failures.push(`${name}: ${error.message}`); console.error(`[observe] FAIL ${failures.at(-1)}`); }
  }
  if (measure) console.log(`export const PINNED = ${JSON.stringify(measured, null, 2)};`);
  return failures.length ? 1 : 0;
}

export async function main(argv = process.argv.slice(2)) {
  let exe = join(repoRoot, "x64/Release/ParticleEditor.exe"), measure = false;
  const overrides = [];
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === "--measure") { measure = true; continue; }
    if (!["--exe", "--game-root"].includes(argv[i]) || !argv[i + 1] || argv[i + 1].startsWith("--")) throw new Error(`bad observe-lane argument ${argv[i]}`);
    const flag = argv[i], value = resolve(argv[++i]);
    if (flag === "--exe") exe = value; else overrides.push(value);
  }
  if (!fs.existsSync(exe)) throw new Error(`missing executable ${exe}; this lane builds nothing`);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw new Error("missing game data: host lookup found no usable MEG archive");
  for (const bin of ["ffmpeg", "ffprobe"]) {
    const result = await spawnWithDeadline(bin, ["-version"], { timeoutMs: 10000 });
    if (result.code !== 0) throw new Error(`${bin} -version failed: ${result.stderr}`);
  }
  return runLane({ exe, roots, measure });
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = await main(); }
  catch (error) { console.error(`[observe] ${error.message}`); process.exitCode = error.exitCode ?? 1; }
}
