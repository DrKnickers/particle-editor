import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { sha256 } from "../observe.mjs";
import { runLoop, checkTask, checkPatchSet, taskPath, decideOutcome, EXIT, OUTCOME } from "./loop.mjs";
import { variationSeeds } from "./four-seed.mjs";
import { fakeExe } from "./testing.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const scratch = join(repoRoot, "tests/.tmp/vfx-loop-unit");
fs.mkdirSync(scratch, { recursive: true });
const fixtures = join(repoRoot, "scripts/vfx-loop/fixtures");
const readJson = (path) => JSON.parse(fs.readFileSync(path, "utf8"));
const quiet = () => {};
const SOURCE = "fake effect bytes";
// The fake --tool patch appends "|<emitter>" per request.
const after = (bytes, ...names) => bytes + names.map((n) => `|${n}`).join("");

function setup(t) {
  const root = fs.mkdtempSync(join(scratch, "loop-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const source = join(root, "p_effect.alo");
  fs.writeFileSync(source, SOURCE);
  const task = structuredClone(readJson(join(fixtures, "task-18-2.json")));
  task.observation.output.strideInterval = 2;
  return { root, source, task, out: join(root, "task") };
}
const request = (hash, name) => ({ schema: 1, selector: { sourceDocumentHash: hash, rolePath: [{ role: "root", name }], expected: { name } },
  mutations: [{ op: "setProperty", field: "lifetime", value: 1 }] });
const patchSet = (input, ...names) => ({ schema: 1, requests: names.map((n) => request(sha256(Buffer.from(input)), n)) });
const ROUND_1 = () => patchSet(SOURCE, "Fire", "Smoke", "Smoke Slow");

test("one accepted round runs the four-seed review and passes for human review", async (t) => {
  const { source, task, out } = setup(t);
  const summary = await runLoop({ task, source, patchSets: [ROUND_1()], out, run: fakeExe(), say: quiet });
  assert.equal(summary.outcome, OUTCOME.pass);
  assert.deepEqual(summary.stop.condition, "author-finished");
  assert.equal(summary.failure, null);
  for (const name of ["loop.json", "source/p_effect.alo", "evidence/inspect.json", "evidence/validate.json",
    "observations/baseline/manifest.json", "rounds/01/patch-set.json", "rounds/01/candidate/p_effect.alo",
    "rounds/01/candidate/validate.json", "rounds/01/candidate/inspect.json", "rounds/01/observation/manifest.json",
    "rounds/01/comparison/review.json", "candidate/p_effect.alo", "four-seed/matrix.json",
    ...[2, 3, 4].flatMap((k) => [`four-seed/seed-${k}/baseline/manifest.json`, `four-seed/seed-${k}/candidate/manifest.json`,
      `four-seed/seed-${k}/comparison/paired-filmstrip.png`]),
    "package/package.json", "package/README.md",
    ...["01", "02", "03"].flatMap((s) => [`rounds/01/steps/${s}/request.json`, `rounds/01/steps/${s}/report.json`])])
    assert.ok(fs.existsSync(join(out, name)), name);
  assert.equal(fs.readFileSync(source, "utf8"), SOURCE, "source bytes unchanged");
  // Each later request is rebound to the step before it.
  for (const [step, previous] of [["02", "01"], ["03", "02"]])
    assert.equal(readJson(join(out, `rounds/01/steps/${step}/request.json`)).selector.sourceDocumentHash,
      readJson(join(out, `rounds/01/steps/${previous}/report.json`)).candidateHash);
  assert.equal(summary.candidate.sha256, sha256(Buffer.from(after(SOURCE, "Fire", "Smoke", "Smoke Slow"))));
  // The four seeds: the task's own first, then the derived ones, each observed for both effects.
  const seeds = variationSeeds(task.observation.scenario.seed);
  const matrix = readJson(join(out, "four-seed/matrix.json"));
  assert.deepEqual(matrix.seeds.map((v) => v.seed), seeds);
  assert.equal(matrix.allPass, true);
  for (const k of [2, 3, 4]) for (const side of ["baseline", "candidate"]) {
    const spec = readJson(join(out, `four-seed/seed-${k}/${side}.observation.json`));
    assert.equal(spec.scenario.seed, seeds[k - 1]);
  }
  const pkg = readJson(join(out, "package/package.json"));
  assert.equal(pkg.outcome, OUTCOME.pass);
  assert.deepEqual(pkg.decision, { choice: null, by: null, at: null, note: null });
  assert.equal(pkg.fourSeed.seeds.length, 4);
  assert.match(fs.readFileSync(join(out, "package/README.md"), "utf8"), /PASS_FOR_HUMAN_REVIEW[\s\S]*Not decided/);
  assert.deepEqual(readJson(join(out, "loop.json")), summary);
});

test("a later round builds on the last accepted candidate", async (t) => {
  const { source, task, out } = setup(t);
  const round1 = after(SOURCE, "Fire", "Smoke", "Smoke Slow");
  const summary = await runLoop({ task, source, patchSets: [ROUND_1(), patchSet(round1, "Sparks")], out, run: fakeExe(), say: quiet });
  assert.equal(summary.outcome, OUTCOME.pass);
  assert.deepEqual(summary.rounds.map((v) => v.accepted), [true, true]);
  assert.equal(summary.candidate.round, 2);
  assert.equal(summary.candidate.sha256, sha256(Buffer.from(after(round1, "Sparks"))));
});

test("a rejected round is discarded and the next starts from the last accepted candidate", async (t) => {
  const { source, task, out } = setup(t);
  const stale = patchSet("not the source", "Fire");
  const summary = await runLoop({ task, source, patchSets: [stale, ROUND_1()], out, run: fakeExe(), say: quiet });
  assert.deepEqual(summary.rounds.map((v) => v.accepted), [false, true]);
  assert.match(summary.rounds[0].reason, /authored against .* not this round's input/);
  assert.equal(summary.rounds[1].input.sha256, sha256(Buffer.from(SOURCE)));
  assert.equal(summary.outcome, OUTCOME.pass);
  assert.ok(fs.existsSync(join(out, "rounds/01/patch-set.json")), "a rejected round keeps its artifacts");
});

test("no accepted round fails the technical gate without a four-seed review", async (t) => {
  const { source, task, out } = setup(t);
  const observeOptions = (alo) => (alo.includes("|Fire") ? { particles: [9, 20, 4, 0] } : {});
  const summary = await runLoop({ task, source, patchSets: [ROUND_1()], out, run: fakeExe({ observeOptions }), say: quiet });
  assert.equal(summary.outcome, OUTCOME.technical);
  assert.match(summary.rounds[0].reason, /budget failed on the primary seed: peak-particles-not-increased/);
  assert.equal(summary.candidate, undefined);
  assert.equal(fs.existsSync(join(out, "four-seed")), false);
  assert.equal(readJson(join(out, "package/package.json")).candidate, null);
});

test("a budget broken under one variation seed only fails the technical gate", async (t) => {
  const { source, task, out } = setup(t);
  const seeds = variationSeeds(task.observation.scenario.seed);
  const observeOptions = (alo, spec) => (alo.includes("|Fire") && spec.scenario.seed === seeds[2] ? { particles: [9, 20, 4, 0] } : {});
  const summary = await runLoop({ task, source, patchSets: [ROUND_1()], out, run: fakeExe({ observeOptions }), say: quiet });
  assert.equal(summary.rounds[0].accepted, true);
  assert.equal(summary.outcome, OUTCOME.technical);
  assert.deepEqual(summary.fourSeed.failedSeeds, [seeds[2]]);
});

test("two consecutive rejections stop the run and leave the decision to a human", async (t) => {
  const { source, task, out } = setup(t);
  const stale = () => patchSet("stale", "Fire");
  const summary = await runLoop({ task: { ...task, maxRounds: 5 }, source, patchSets: [ROUND_1(), stale(), stale(), ROUND_1()],
    out, run: fakeExe(), say: quiet });
  assert.deepEqual(summary.rounds.map((v) => v.accepted), [true, false, false]);
  assert.equal(summary.stop.condition, "two-rejections");
  assert.equal(summary.outcome, OUTCOME.judgment);
  assert.equal(summary.candidate.round, 1);
});

test("maxRounds stops the run with rounds unplayed", async (t) => {
  const { source, task, out } = setup(t);
  const round1 = after(SOURCE, "Fire", "Smoke", "Smoke Slow");
  const summary = await runLoop({ task: { ...task, maxRounds: 1 }, source, patchSets: [ROUND_1(), patchSet(round1, "Sparks")],
    out, run: fakeExe(), say: quiet });
  assert.equal(summary.rounds.length, 1);
  assert.equal(summary.stop.condition, "max-rounds");
  assert.equal(summary.outcome, OUTCOME.judgment);
});

test("the author can ask for human judgment", async (t) => {
  const { source, task, out } = setup(t);
  const flagged = { ...ROUND_1(), needsHumanJudgment: "the smoke density trade-off is a taste call" };
  const summary = await runLoop({ task, source, patchSets: [flagged], out, run: fakeExe(), say: quiet });
  assert.deepEqual(summary.stop, { condition: "author-flagged", detail: "the smoke density trade-off is a taste call" });
  assert.equal(summary.outcome, OUTCOME.judgment);
});

test("a tool refusal mid-chain rejects the round and keeps its report", async (t) => {
  const { source, task, out } = setup(t);
  const run = fakeExe({ refuse: (name) => (name === "Smoke" ? { code: 5, category: "rule" } : null) });
  const summary = await runLoop({ task, source, patchSets: [ROUND_1()], out, run, say: quiet });
  assert.match(summary.rounds[0].reason, /request 2 refused with --tool exit 5 \(rule/);
  assert.equal(readJson(join(out, "rounds/01/steps/02/report.json")).refusal.category, "rule");
  assert.equal(summary.outcome, OUTCOME.technical);
});

test("an invalid candidate rejects the round", async (t) => {
  const { source, task, out } = setup(t);
  const run = fakeExe({ validate: (bytes) => (bytes.includes("|Smoke Slow") ? 3 : 0) });
  const summary = await runLoop({ task, source, patchSets: [ROUND_1()], out, run, say: quiet });
  assert.match(summary.rounds[0].reason, /candidate is invalid/);
  assert.equal(summary.outcome, OUTCOME.technical);
});

test("a malformed finding or patch set rejects the round, not the run", async (t) => {
  const { source, task, out } = setup(t);
  const bad = { ...ROUND_1(), findings: [{ id: "F1", claim: "smoke is late" }] };
  const summary = await runLoop({ task, source, patchSets: [bad, { schema: 2 }], out, run: fakeExe(), say: quiet });
  assert.match(summary.rounds[0].reason, /findings\[0\] is missing severity/);
  assert.match(summary.rounds[1].reason, /schema must be 1/);
  assert.equal(summary.stop.condition, "two-rejections");
  assert.equal(summary.outcome, OUTCOME.technical);
});

async function blockedRun(t, change, { outcome, exitCode, step, message }) {
  const context = setup(t);
  const options = { ...context, patchSets: [ROUND_1()], run: fakeExe(), say: quiet };
  change(options);
  await assert.rejects(runLoop(options), (error) => error.exitCode === exitCode && error.outcome === outcome && message.test(error.message));
  const recorded = readJson(join(context.out, "loop.json"));
  assert.equal(recorded.outcome, outcome);
  assert.equal(recorded.failure.step, step);
  const pkg = readJson(join(context.out, "package/package.json"));
  assert.equal(pkg.outcome, outcome);
  assert.equal(pkg.failure.exitCode, exitCode);
}

test("a missing source is blocked by an asset", async (t) => {
  await blockedRun(t, (o) => { o.source += ".missing"; },
    { outcome: OUTCOME.asset, exitCode: EXIT.asset, step: "source", message: /source is unavailable/ });
});

test("an invalid source is blocked by an asset", async (t) => {
  await blockedRun(t, (o) => { o.run = fakeExe({ validate: () => 3 }); },
    { outcome: OUTCOME.asset, exitCode: EXIT.asset, step: "evidence", message: /source is invalid/ });
});

test("an effect --observe cannot load is blocked by an asset", async (t) => {
  await blockedRun(t, (o) => { o.run = fakeExe({ observeExit: (alo) => (alo.includes("|Fire") ? 3 : 0) }); },
    { outcome: OUTCOME.asset, exitCode: EXIT.asset, step: "round-01-observe", message: /could not load the effect/ });
});

test("a crashing tool is blocked by tooling", async (t) => {
  await blockedRun(t, (o) => { o.run = fakeExe({ crash: (verb) => (verb === "patch" ? 14 : 0) }); },
    { outcome: OUTCOME.tooling, exitCode: EXIT.tooling, step: "round-01-patch-01", message: /--tool exit 14/ });
});

test("misaligned observations are blocked by tooling", async (t) => {
  await blockedRun(t, (o) => { o.run = fakeExe({ observeOptions: (alo) => (alo.includes("|Fire") ? { crop: [0, 0, 16, 32] } : {}) }); },
    { outcome: OUTCOME.tooling, exitCode: EXIT.misaligned, step: "round-01-compare", message: /geometry\.crop/ });
});

test("the outcome table (plan §7.1 G)", () => {
  assert.equal(decideOutcome({ accepted: false, stop: "author-finished", fourSeedPass: false }), OUTCOME.technical);
  assert.equal(decideOutcome({ accepted: true, stop: "author-finished", fourSeedPass: false }), OUTCOME.technical);
  for (const stop of ["max-rounds", "two-rejections", "author-flagged"])
    assert.equal(decideOutcome({ accepted: true, stop, fourSeedPass: true }), OUTCOME.judgment);
  assert.equal(decideOutcome({ accepted: true, stop: "author-finished", fourSeedPass: true }), OUTCOME.pass);
});

test("a malformed task is refused before the task root exists", async (t) => {
  const context = setup(t);
  context.task.observation.scenario.aloPath = "C:/x.alo";
  await assert.rejects(runLoop({ ...context, patchSets: [ROUND_1()], run: fakeExe(), say: quiet }), (error) => error.exitCode === EXIT.input);
  assert.equal(fs.existsSync(context.out), false);
});

test("a non-empty task root is refused", async (t) => {
  const context = setup(t);
  fs.mkdirSync(context.out); fs.writeFileSync(join(context.out, "old.json"), "{}");
  await assert.rejects(runLoop({ ...context, patchSets: [ROUND_1()], run: fakeExe(), say: quiet }), (error) => error.exitCode === EXIT.io);
});

test("task validation names each problem", () => {
  const task = readJson(join(fixtures, "task-18-2.json"));
  assert.deepEqual(checkTask(task), []);
  const seed = task.observation.scenario.seed;
  assert.match(checkTask({ ...task, budgets: [{ id: "x", metric: "prettiness", maxDelta: 0 }] }).join(), /metric must be one of/);
  assert.match(checkTask({ ...task, budgets: [{ id: "x", metric: "peakParticles", maxDelta: 0 },
    { id: "x", metric: "finalFrame", maxDelta: 0 }] }).join(), /unique id/);
  assert.match(checkTask({ ...task, brief: " " }).join(), /brief/);
  assert.match(checkTask({ ...task, maxRounds: 0 }).join(), /maxRounds/);
  assert.match(checkTask({ ...task, variationSeeds: [seed, 1, 1, 2] }).join(), /four distinct/);
  assert.match(checkTask({ ...task, variationSeeds: [9, 1, 2, 3] }).join(), /variationSeeds\[0\] must equal/);
  assert.deepEqual(checkTask({ ...task, variationSeeds: [seed, 1, 2, 3] }), []);
  const noSeed = structuredClone(task); delete noSeed.observation.scenario.seed;
  assert.match(checkTask(noSeed).join(), /seed must be a 32-bit/);
  assert.deepEqual(checkTask({ schemaVersion: 2 }), ["task: schemaVersion must be 1"]);
});

test("variation seeds: the task's own first, then a fixed derivation, or the listed ones", () => {
  const seeds = variationSeeds(305419896);
  assert.equal(seeds[0], 305419896);
  assert.equal(new Set(seeds).size, 4);
  assert.ok(seeds.every((v) => Number.isInteger(v) && v >= 0 && v <= 0xffffffff));
  assert.deepEqual(variationSeeds(7, [7, 8, 9, 10]), [7, 8, 9, 10]);
});

test("the canned §18.2 patch set is well formed and edits three unlinked roots", () => {
  const set = readJson(join(fixtures, "canned-18-2.patch.json"));
  const hash = set.requests[0].selector.sourceDocumentHash;
  assert.deepEqual(checkPatchSet(set, hash), []);
  assert.deepEqual(set.requests.map((r) => r.selector.rolePath.at(-1).name), ["Fire", "Smoke", "Smoke Slow"]);
  const shipped = sha256(fs.readFileSync(join(repoRoot, "site/guide/downloads/P_EXPLOSION_EXAMPLE.ALO")));
  assert.equal(hash, shipped, "the canned patch names the shipped example effect");
});

test("task paths stay inside the task root", (t) => {
  const root = fs.mkdtempSync(join(scratch, "paths-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  assert.equal(taskPath(root, "a/b.json"), join(root, "a", "b.json"));
  for (const name of ["../x", "a/../../x", "C:/x", "a\\b", "/x", "a//b", "./a"])
    assert.throws(() => taskPath(root, name), /not task-relative/, name);
  const outside = fs.mkdtempSync(join(scratch, "outside-"));
  t.after(() => fs.rmSync(outside, { recursive: true, force: true }));
  fs.symlinkSync(outside, join(root, "link"), process.platform === "win32" ? "junction" : "dir");
  assert.throws(() => taskPath(root, "link/x.json"), /escapes the task root/);
});
