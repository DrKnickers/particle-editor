// [vfx-loop] The vfx-loop gate lane: the canned §18.2 loop end to end on the
// Release exe, ffmpeg and the game install, against the guide's example effect:
// one round, the four-seed review, the outcome and the promotion package.
// The unit tests cover the logic with fakes; this proves the real chain holds.
//
//   node scripts/vfx-loop/lane.mjs [--exe path] [--game-root path ...] [--measure]
//
// Observation is deterministic, so the run's numbers are pinned. A pin that
// moves means the exe, the patch or the effect changed: re-measure on purpose.
import * as fs from "node:fs";
import { dirname, isAbsolute, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { sha256, spawnWithDeadline } from "../observe.mjs";
import { resolveReplayGameRoots } from "../replay-proof.mjs";
import { runLoop } from "./loop.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const scratch = join(repoRoot, "tests/.tmp/vfx-loop");
const FIXTURE = join(repoRoot, "site/guide/downloads/P_EXPLOSION_EXAMPLE.ALO");
const here = (name) => join(repoRoot, "scripts/vfx-loop/fixtures", name);

// Pins measured 2026-10-11 on the owner's native build (node scripts/vfx-loop/lane.mjs --measure).
export const PINNED = {
  outcome: "PASS_FOR_HUMAN_REVIEW",
  stop: "author-finished",
  candidateHash: "3780b1b8474c3e6aeef897cf62212a881e7f147ed6093b63d2979b8acb1a1509",
  pairs: 13,
  firstRandomDivergence: 2,
  identicalSelectedFrames: [0, 105, 120, 122, 135, 137],
  budgetsPass: true,
  seeds: [
    { seed: 305419896, peakParticles: [171, 171], budgetsPass: true },
    { seed: 2959855665, peakParticles: [168, 168], budgetsPass: true },
    { seed: 1319324138, peakParticles: [167, 167], budgetsPass: true },
    { seed: 3973759907, peakParticles: [172, 172], budgetsPass: true },
  ],
};

export function measuredValues(summary, metrics, review, schedule, matrix) {
  return { outcome: summary.outcome, stop: summary.stop.condition, candidateHash: summary.candidate?.sha256 ?? null,
    pairs: review.pairs.length, firstRandomDivergence: schedule.firstDivergentFrame,
    identicalSelectedFrames: metrics.drift.identicalSelectedFrames, budgetsPass: metrics.budgetsPass,
    seeds: matrix.seeds.map((v) => ({ seed: v.seed, peakParticles: v.peakParticles, budgetsPass: v.budgetsPass })) };
}

function cleanScratch(path) {
  const rel = relative(scratch, resolve(path));
  if (!rel || rel.startsWith("..") || isAbsolute(rel)) throw new Error(`scratch cleanup escaped the vfx-loop root: ${path}`);
  fs.rmSync(path, { recursive: true, force: true });
}

export async function runLane({ exe = join(repoRoot, "x64/Release/ParticleEditor.exe"), roots = [], measure = false,
  run = spawnWithDeadline, pins = PINNED, say = (line) => console.log(line) } = {}) {
  fs.mkdirSync(scratch, { recursive: true });
  // The verbs warn when the file name differs from the effect's internal name.
  const source = join(scratch, "p_explosion_example.alo"), out = join(scratch, "run");
  cleanScratch(source); cleanScratch(out);
  fs.copyFileSync(FIXTURE, source);
  const original = fs.readFileSync(FIXTURE);
  const readJson = (path) => JSON.parse(fs.readFileSync(path, "utf8"));
  const summary = await runLoop({ task: readJson(here("task-18-2.json")), source, patchSets: [readJson(here("canned-18-2.patch.json"))],
    out, exe, roots, run, say });
  const errors = [];
  const require = (ok, message) => { if (!ok) errors.push(message); };
  require(summary.failure === null, `loop failed: ${JSON.stringify(summary.failure)}`);
  require(summary.steps.filter((v) => /^round-01-patch-0/.test(v.step)).length === 3, "expected three chained patch steps");
  require(fs.readFileSync(FIXTURE).equals(original) && sha256(fs.readFileSync(source)) === sha256(original), "a source changed");
  const comparison = join(out, "rounds/01/comparison");
  for (const name of ["paired-filmstrip.png", "metrics.json", "telemetry-delta.json", "random-schedule-note.json", "review.json"])
    require(fs.existsSync(join(comparison, name)), `missing rounds/01/comparison/${name}`);
  for (const name of ["four-seed/matrix.json", "package/package.json", "package/README.md"])
    require(fs.existsSync(join(out, name)), `missing ${name}`);
  const metrics = readJson(join(comparison, "metrics.json")), review = readJson(join(comparison, "review.json"));
  const values = measuredValues(summary, metrics, review, readJson(join(comparison, "random-schedule-note.json")),
    readJson(join(out, "four-seed/matrix.json")));
  if (measure) say(`export const PINNED = ${JSON.stringify(values, null, 2)};`);
  else if (!pins) errors.push("PINNED is null; the owner must run node scripts/vfx-loop/lane.mjs --measure");
  else for (const key of Object.keys(pins))
    require(isDeepStrictEqual(values[key], pins[key]), `${key} differs: measured ${JSON.stringify(values[key])}, pinned ${JSON.stringify(pins[key])}`);
  for (const error of errors) say(`[vfx-loop] FAIL ${error}`);
  if (!errors.length) say(`[vfx-loop] lane ${measure ? "MEASURED" : "PASS"} ${JSON.stringify(values)}`);
  return errors.length ? 1 : 0;
}

export async function main(argv = process.argv.slice(2)) {
  let exe = join(repoRoot, "x64/Release/ParticleEditor.exe"), measure = false;
  const overrides = [];
  for (let i = 0; i < argv.length; ++i) {
    if (argv[i] === "--measure") { measure = true; continue; }
    if (!["--exe", "--game-root"].includes(argv[i]) || !argv[i + 1] || argv[i + 1].startsWith("--")) throw new Error(`bad vfx-loop lane argument ${argv[i]}`);
    const flag = argv[i], value = resolve(argv[++i]);
    if (flag === "--exe") exe = value; else overrides.push(value);
  }
  if (!fs.existsSync(exe)) throw new Error(`missing executable ${exe}; this lane builds nothing`);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw new Error("missing game data: host lookup found no usable MEG archive");
  return runLane({ exe, roots, measure });
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = await main(); }
  catch (error) { console.error(`[vfx-loop] ${error.message}`); process.exitCode = error.exitCode ?? 1; }
}
