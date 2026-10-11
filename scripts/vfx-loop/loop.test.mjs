import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { sha256 } from "../observe.mjs";
import { runLoop, checkTask, checkPatchSet, taskPath, EXIT } from "./loop.mjs";
import { fakeExe } from "./testing.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const scratch = join(repoRoot, "tests/.tmp/vfx-loop-unit");
fs.mkdirSync(scratch, { recursive: true });
const fixtures = join(repoRoot, "scripts/vfx-loop/fixtures");
const readJson = (path) => JSON.parse(fs.readFileSync(path, "utf8"));
const quiet = () => {};

function setup(t) {
  const root = fs.mkdtempSync(join(scratch, "loop-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const source = join(root, "p_effect.alo");
  fs.writeFileSync(source, "fake effect bytes");
  const hash = sha256(fs.readFileSync(source));
  const task = { ...readJson(join(fixtures, "task-18-2.json")) };
  task.observation = { ...task.observation, output: { ...task.observation.output, strideInterval: 2 } };
  const request = (name) => ({ schema: 1, selector: { sourceDocumentHash: hash, rolePath: [{ role: "root", name }], expected: { name } },
    mutations: [{ op: "setProperty", field: "lifetime", value: 1 }] });
  const patchSet = { schema: 1, requests: ["Fire", "Smoke", "Smoke Slow"].map(request) };
  return { root, source, hash, task, patchSet, request, out: join(root, "task") };
}

test("a full run leaves every artifact, chains the requests, and never writes the source", async (t) => {
  const { source, hash, task, patchSet, out } = setup(t);
  const before = fs.readFileSync(source);
  const summary = await runLoop({ task, source, patchSet, out, run: fakeExe(), say: quiet });
  assert.equal(summary.failure, null);
  assert.equal(summary.technicalGate.pass, true);
  for (const name of ["loop.json", "source/p_effect.alo", "evidence/inspect.json", "evidence/validate.json",
    "patch/patch-set.json", "observations/baseline.observation.json", "observations/baseline/manifest.json",
    "observations/candidate.observation.json", "observations/candidate/manifest.json",
    "candidate/p_effect.alo", "candidate/validate.json", "comparison/paired-filmstrip.png", "comparison/metrics.json",
    "comparison/telemetry-delta.json", "comparison/random-schedule-note.json", "comparison/review.json",
    ...["01", "02", "03"].flatMap((s) => [`patch/${s}/request.json`, `patch/${s}/report.json`, `patch/${s}/p_effect.alo`])])
    assert.ok(fs.existsSync(join(out, name)), name);
  assert.ok(fs.readFileSync(source).equals(before), "source bytes unchanged");
  // Request 1 names the source; each later one is rebound to the step before.
  assert.equal(readJson(join(out, "patch/01/request.json")).selector.sourceDocumentHash, hash);
  for (const [step, previous] of [["02", "01"], ["03", "02"]])
    assert.equal(readJson(join(out, `patch/${step}/request.json`)).selector.sourceDocumentHash,
      readJson(join(out, `patch/${previous}/report.json`)).candidateHash);
  const candidateHash = sha256(fs.readFileSync(join(out, "candidate/p_effect.alo")));
  assert.equal(summary.candidate.sha256, candidateHash);
  assert.equal(readJson(join(out, "observations/candidate/capture.json")).source.sha256, candidateHash);
  // Both observations ran one spec, apart from the effect they load.
  const specs = ["baseline", "candidate"].map((v) => readJson(join(out, `observations/${v}.observation.json`)));
  assert.notEqual(specs[0].scenario.aloPath, specs[1].scenario.aloPath);
  delete specs[0].scenario.aloPath; delete specs[1].scenario.aloPath;
  assert.deepEqual(specs[0], specs[1]);
  assert.deepEqual(readJson(join(out, "loop.json")), summary);
  assert.deepEqual(summary.steps.map((v) => v.step), ["source", "evidence", "patch-set", "baseline", "patch-01", "patch-02",
    "patch-03", "candidate", "candidate-observation", "compare", "source-check"]);
});

test("an over-budget candidate completes with the technical gate failed", async (t) => {
  const { source, task, patchSet, out } = setup(t);
  const observeOptions = (alo) => (alo.includes("|Fire") ? { particles: [9, 20, 4, 0] } : {});
  const summary = await runLoop({ task, source, patchSet, out, run: fakeExe({ observeOptions }), say: quiet });
  assert.deepEqual(summary.technicalGate, { pass: false, failed: ["peak-particles-not-increased"] });
});

async function refused(t, change, { exitCode, step, message, run = fakeExe() }) {
  const context = setup(t);
  change(context);
  await assert.rejects(runLoop({ ...context, run, say: quiet }),
    (error) => error.exitCode === exitCode && message.test(error.message));
  const recorded = readJson(join(context.out, "loop.json"));
  assert.equal(recorded.failure.step, step);
  assert.equal(recorded.failure.exitCode, exitCode);
  return context;
}

test("a patch set authored against another source is refused before anything is observed", async (t) => {
  const { out } = await refused(t, (c) => { c.patchSet.requests[1].selector.sourceDocumentHash = "0".repeat(64); },
    { exitCode: EXIT.refused, step: "patch-set", message: /requests\[1\] was authored against 0{64}/ });
  assert.equal(fs.existsSync(join(out, "observations")), false);
});

test("two requests for one emitter are refused as malformed", async (t) => {
  await refused(t, (c) => { c.patchSet.requests.push(c.request("Fire")); },
    { exitCode: EXIT.input, step: "patch-set", message: /targets .*Fire.* again/ });
});

test("a tool refusal mid-chain stops the loop and keeps its report", async (t) => {
  const run = fakeExe({ refuse: (name) => (name === "Smoke" ? { code: 5, category: "rule" } : null) });
  const { out } = await refused(t, () => {}, { exitCode: EXIT.refused, step: "patch-02", message: /request 2 refused with --tool exit 5 \(rule/, run });
  assert.equal(readJson(join(out, "patch/02/report.json")).refusal.category, "rule");
  assert.equal(fs.existsSync(join(out, "candidate")), false);
});

test("an invalid candidate stops the loop with exit 3", async (t) => {
  const run = fakeExe({ validate: (bytes) => (bytes.includes("|Smoke Slow") ? 3 : 0) });
  await refused(t, () => {}, { exitCode: EXIT.invalid, step: "candidate", message: /candidate is invalid/, run });
});

test("an invalid source stops the loop before any patch", async (t) => {
  const run = fakeExe({ validate: () => 3 });
  const { out } = await refused(t, () => {}, { exitCode: EXIT.invalid, step: "evidence", message: /source is invalid/, run });
  assert.equal(fs.existsSync(join(out, "patch")), false);
});

test("misaligned observations stop the loop at the comparison", async (t) => {
  const observeOptions = (alo) => (alo.includes("|Fire") ? { crop: [0, 0, 16, 32] } : {});
  await refused(t, () => {}, { exitCode: EXIT.misaligned, step: "compare", message: /geometry\.crop/, run: fakeExe({ observeOptions }) });
});

test("a malformed finding is refused (§18.3)", async (t) => {
  await refused(t, (c) => { c.patchSet.findings = [{ id: "F1", claim: "smoke is late" }]; },
    { exitCode: EXIT.input, step: "patch-set", message: /findings\[0\] is missing severity/ });
});

test("a malformed task is refused before the task root exists", async (t) => {
  const context = setup(t);
  context.task = { ...context.task, observation: { ...context.task.observation,
    scenario: { ...context.task.observation.scenario, aloPath: "C:/x.alo" } } };
  await assert.rejects(runLoop({ ...context, run: fakeExe(), say: quiet }), (error) => error.exitCode === EXIT.input);
  assert.equal(fs.existsSync(context.out), false);
});

test("a non-empty task root is refused", async (t) => {
  const context = setup(t);
  fs.mkdirSync(context.out); fs.writeFileSync(join(context.out, "old.json"), "{}");
  await assert.rejects(runLoop({ ...context, run: fakeExe(), say: quiet }), (error) => error.exitCode === EXIT.io);
});

test("task validation names each problem", () => {
  const task = readJson(join(fixtures, "task-18-2.json"));
  assert.deepEqual(checkTask(task), []);
  assert.match(checkTask({ ...task, budgets: [{ id: "x", metric: "prettiness", maxDelta: 0 }] }).join(), /metric must be one of/);
  assert.match(checkTask({ ...task, budgets: [{ id: "x", metric: "peakParticles", maxDelta: 0 },
    { id: "x", metric: "finalFrame", maxDelta: 0 }] }).join(), /unique id/);
  assert.match(checkTask({ ...task, brief: " " }).join(), /brief/);
  assert.deepEqual(checkTask({ schemaVersion: 2 }), ["task: schemaVersion must be 1"]);
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
