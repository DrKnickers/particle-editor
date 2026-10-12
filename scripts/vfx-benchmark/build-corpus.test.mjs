import { test } from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import { join } from "node:path";
import { draft, lock, readsDifferently } from "./build-corpus.mjs";
import { checkStore } from "./check-corpus.mjs";
import { writeTruth } from "./blind-page.mjs";
import { deriveTruth } from "./derive-truth.mjs";
import { prepare, score } from "./subagent-run.mjs";
import { contextFor } from "./context.mjs";
import { canonicalFinding } from "./answers.mjs";
import { scoreAttempt } from "./score.mjs";
import { invertMap, renumberFinding } from "./semantic.mjs";
import { markdown } from "./report.mjs";
import { RUBRIC } from "./schema-check.mjs";
import { store, sha256 } from "./store.mjs";
import { effectBytes, emitter, fakeBenchExe, fakeSsim } from "./testing.mjs";

const f = Math.fround;
const json = (v) => `${JSON.stringify(v, null, 2)}\n`;
// Every changed frame counts as perceptible: the fake frames are flat colours.
const fakeDiff = () => ({ ok: true, peak: 255 });
const SPEC = { schemaVersion: 1, scenario: { schemaVersion: 1, seed: 305419896, algorithmVersion: 1, clockAnchor: 0, spawnPosition: [0, 0, 0],
  spawnVelocity: [0, 0, 0], maxFrames: 300, overloadGuardEnabled: true, overloadGuardLimit: 10000, estimatedParticlesPerInstance: 0 },
  lifecycle: { type: "oneshot", quietTailFrames: 15 }, output: { strideInterval: 15, playbackStride: 0, maxBytes: 209715200 } };

function write(path, bytes) { fs.mkdirSync(join(path, ".."), { recursive: true }); fs.writeFileSync(path, bytes); }

// Base A: a fireball that hands over to smoke; base S: a randomised seed base.
const FIRE = () => [emitter(0, "Fireball", { lifetime: 2, blendMode: 1 }), emitter(1, "Smoke", { lifetime: 3, initialDelay: f(0.5) })];
const SEEDY = () => [emitter(0, "Puff", { lifetime: 2, randomLifetimePerc: f(0.4) })];

function setUp(t) {
  const root = fs.mkdtempSync(join(os.tmpdir(), "vfx-corpus-"));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const storeRoot = join(root, "stores", "corpus-v1");
  const exe = join(root, "fake.exe");
  fs.writeFileSync(exe, "fake exe");
  const content = join(root, "game");
  fs.mkdirSync(content);
  const bases = { A: FIRE(), S: SEEDY() };
  for (const [id, emitters] of Object.entries(bases)) {
    write(join(storeRoot, "bases", id, "effect.alo"), effectBytes(emitters));
    write(join(storeRoot, "bases", id, "observation.json"), json(SPEC));
  }
  write(join(storeRoot, "bases", "A", "intent.md"), "# Fireball\n\nA fireball hands over to smoke.\n\n## Targets\n\n- Fireball (0) lifetime: 2\n- Smoke (1) initialDelay: 0.5\n");
  write(join(storeRoot, "bases", "S", "intent.md"), "# Puff\n\nA puff.\n\n## Targets\n\n- Puff (0) lifetime: 2\n");
  // Authored cases: the fireball shortened, the smoke delayed, and a missing texture.
  const author = (name, recipe, edit) => {
    const emitters = FIRE();
    edit(emitters);
    for (const e of emitters) { e.groups[1].minY = f(e.lifetime * f(1 - e.randomLifetimePerc)); e.groups[1].maxY = e.lifetime; }
    write(join(storeRoot, "authoring", name, "effect.alo"), effectBytes(emitters));
    write(join(storeRoot, "authoring", name, "recipe.json"), json({ schemaVersion: 1, base: "A", kind: "field", ...recipe }));
  };
  author("short", { class: "short-lifetime", target: { index: 0, name: "Fireball" }, mutations: [{ op: "setProperty", field: "lifetime", value: 0.8 }] },
    (e) => { e[0].lifetime = f(0.8); });
  author("late", { class: "late-delay", target: { index: 1, name: "Smoke" }, mutations: [{ op: "setProperty", field: "initialDelay", value: 1.1 }] },
    (e) => { e[1].initialDelay = f(1.1); });
  author("texture", { class: "missing-texture", target: { index: 1, name: "Smoke" }, mutations: [{ op: "setColorTexture", name: "smoke_absent.tga" }] },
    (e) => { e[1].colorTexture = "smoke_absent.tga"; });
  const hash = (id) => sha256(fs.readFileSync(join(storeRoot, "bases", id, "effect.alo")));
  write(join(storeRoot, "corpus.json"), json({
    schemaVersion: 1, version: "corpus-v1", state: "draft", provenance: { exeSha256: "", contentRoots: [content] },
    targets: { objective: 80, subjective: 65, controlFlag: 15, fdp: 30, completion: 90 }, attempts: 3,
    bases: [{ id: "A", canonical: hash("A") }, { id: "S", canonical: hash("S"), seedBase: true }],
    cases: [], requiredClasses: ["short-lifetime", "late-delay", "missing-texture"],
    seedSearch: { base: "S", drawSeed: 11, labelSeed: 5 },
  }));
  let n = 0;
  return { storeRoot, exe, ids: () => `id${String(++n).padStart(2, "0")}` };
}

// The owner's blind pass, recorded through blind-page.mjs's own write path.
function blindPass(s, corpus, map) {
  const byName = Object.fromEntries(Object.entries(map).map(([name, id]) => [id, name]));
  for (const c of corpus.cases) {
    const name = byName[c.id];
    const drift = c.kind === "defect" && c.shape === "single" ? s.json("cases", c.id, "drift.json") : null;
    const both = (variants, visible = true) => ({ transports: { frames: { visible, variants }, "contact-sheets": { visible, variants } } });
    const policy = { primary: "frames", "bundle-only": "frames" };
    let variants = [];
    if (name === "short") variants = [{ bundleId: "bundle", category: "duration", direction: "too-short", emitters: [{ index: 0, name: "Fireball" }],
      windows: { bundle: { image: drift.candidateImage.slice(0, 2), telemetry: [] } }, policy,
      correction: { kind: "scalar", emitter: { index: 0, name: "Fireball" }, field: "lifetime" } }];
    if (name === "late") variants = [{ bundleId: "bundle", category: "timing", direction: "too-late", emitters: [{ index: 1, name: "Smoke" }],
      windows: { bundle: { image: [], telemetry: drift.candidateTelemetry.slice(0, 3) } }, policy,
      correction: { kind: "scalar", emitter: { index: 1, name: "Smoke" }, field: "initialDelay" } }];
    if (name === "texture") variants = [{ bundleId: "bundle", category: "missing-texture", direction: "unresolved", emitters: [{ index: 1, name: "Smoke" }],
      windows: { bundle: { image: [], telemetry: [] } }, policy: { primary: "inspect", "bundle-only": null },
      correction: { kind: "texture", emitter: { index: 1, name: "Smoke" }, field: "colorTexture" } }];
    if (name === "seed") {
      const recipe = s.json("cases", c.id, "recipe.json");
      variants = [{ bundleId: recipe.outlier, category: "duration", direction: "too-long", emitters: [{ index: 0, name: "Puff" }],
        windows: { [recipe.outlier]: { image: [], telemetry: [60, 61, 62] } }, policy,
        correction: { kind: "scalar", emitter: { index: 0, name: "Puff" }, field: "lifetime" } }];
    }
    writeTruth(s.root, c.id, both(variants));
  }
}

test("draft, blind pass and lock: a corpus whose every answer scores and whose controls score clean", async (t) => {
  const { storeRoot, exe, ids } = setUp(t);
  const lines = [];
  const drafted = await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: (l) => lines.push(l) });
  // Three authored cases and the seed case; a single-bundle control for each of the two bases, and the three-seed control.
  assert.equal(drafted.cases.length, 7);
  assert.deepEqual(drafted.cases.map((c) => `${c.kind}/${c.shape}`).sort(),
    ["control/single", "control/single", "control/three", "defect/single", "defect/single", "defect/single", "defect/three"]);
  assert.equal(drafted.provenance.render.vertexProcessing, "HWVP");
  const s = store(storeRoot);
  const map = s.json("authoring-map.json");
  const seed = drafted.cases.find((c) => c.class === "seed");
  assert.equal(seed.shape, "three");
  assert.equal(s.json("seed-search.json").outlier, 37);
  // A seed pack carries three bundles; a primary pack has inspect.json, a bundle-only pack does not.
  assert.ok(s.exists("packs", seed.id, "bundle-b", "frames.jsonl"));
  assert.ok(s.exists("packs", map.short, "inspect.json"));
  assert.ok(!s.exists("packs-bundle-only", map.late, "inspect.json"));
  assert.ok(!fs.readFileSync(s.at("packs", map.short, "frames.jsonl"), "utf8").includes("rngState"));
  assert.deepEqual(JSON.parse(s.text("packs", map.short, "textures.json")), ["fireball.tga", "puff.tga", "smoke.tga"]);

  blindPass(s, drafted, map);
  const locked = lock({ root: storeRoot, say: () => {} });
  assert.equal(locked.state, "locked");
  assert.match(locked.hashes.truth, /^[0-9a-f]{64}$/);
  assert.equal(locked.cases.find((c) => c.id === map.late).secondaryEligible, true);

  // The frozen answers score: every canonical finding is identified in the primary pack.
  for (const c of locked.cases.filter((x) => x.kind === "defect")) {
    const answers = s.json("cases", c.id, "answers.json").frames.variants;
    const context = contextFor(s, c, "primary", JSON.parse(s.text("packs", c.id, "textures.json")));
    const finding = canonicalFinding(answers[0], "primary", context.textures);
    const score = scoreAttempt({ caseInfo: { kind: "defect" }, answers, findings: [finding], context, rubric: RUBRIC });
    assert.equal(score.identified, true, `${c.class}: ${JSON.stringify(score.units)}`);
  }
  assert.throws(() => lock({ root: storeRoot, say: () => {} }), /already locked/);

  // check-corpus: clean, then each kind of tampering refused.
  assert.deepEqual(checkStore(storeRoot), []);
  const pack = s.at("packs", map.short, "intent.md");
  const original = fs.readFileSync(pack);
  fs.writeFileSync(pack, "# edited\n");
  assert.match(checkStore(storeRoot).join("\n"), /intent.md does not match the manifest/);
  fs.writeFileSync(pack, original);
  fs.utimesSync(pack, new Date(Date.UTC(2000, 0, 1)), new Date(Date.UTC(2000, 0, 1)));
  assert.deepEqual(checkStore(storeRoot), []);
  const truthPath = s.at("cases", map.late, "truth.json");
  const truthText = fs.readFileSync(truthPath, "utf8");
  fs.writeFileSync(truthPath, truthText.replace("too-late", "too-early"));
  const tampered = checkStore(storeRoot).join("\n");
  assert.match(tampered, /truth hash differs/);
  assert.match(tampered, /truth.json changed since it was pinned/);
  fs.writeFileSync(truthPath, truthText);
  const lockedCorpus = s.json("corpus.json");
  s.writeJson(["corpus.json"], { ...lockedCorpus, hashes: { ...lockedCorpus.hashes, prompt: "0".repeat(64) } });
  assert.match(checkStore(storeRoot).join("\n"), /prompt hash differs/);
  s.writeJson(["corpus.json"], lockedCorpus);
  fs.writeFileSync(s.at("cases", map.late, "extra.txt"), "x");
  assert.match(checkStore(storeRoot).join("\n"), /extra.txt is not pinned/);
});

test("derive-truth: a corpus without a person locks on the planted answers, and every one scores", async (t) => {
  const { storeRoot, exe, ids } = setUp(t);
  const s = store(storeRoot);
  const skeleton = s.json("corpus.json");
  delete skeleton.seedSearch;
  s.writeJson(["corpus.json"], { ...skeleton, seedAbsence: "c" });
  const drafted = await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} });
  assert.equal(drafted.cases.length, 4);
  deriveTruth({ root: storeRoot, say: () => {}, diff: fakeDiff });
  const map = s.json("authoring-map.json");
  const texture = s.json("cases", map.texture, "truth.json").transports.frames.variants[0];
  assert.deepEqual([texture.category, texture.policy.primary, texture.correction.field], ["missing-texture", "inspect", "colorTexture"]);
  assert.deepEqual(s.json("cases", map["control-A"], "truth.json").transports["contact-sheets"], { visible: false, variants: [] });
  const locked = lock({ root: storeRoot, say: () => {} });
  const textures = JSON.parse(s.text("packs", map.short, "textures.json"));
  for (const c of locked.cases.filter((x) => x.kind === "defect")) {
    const answers = s.json("cases", c.id, "answers.json").frames.variants;
    const finding = canonicalFinding(answers[0], "primary", textures);
    const score = scoreAttempt({ caseInfo: c, answers, findings: [finding], context: contextFor(s, c, "primary", textures), rubric: RUBRIC });
    assert.equal(score.identified, true, c.class);
  }
  assert.deepEqual(checkStore(storeRoot), []);

  // A sub-agent run over it: staged, answered perfectly, audited clean, scored.
  const runRoot = join(storeRoot, "..", "..", "runs", "run-1");
  const { attempts } = prepare({ storeRoot, runRoot, agent: "claude", model: "opus" });
  assert.equal(attempts.length, locked.cases.length * 3 + locked.cases.filter((c) => c.kind === "control" || c.secondaryEligible).length);
  assert.equal(JSON.parse(fs.readFileSync(join(runRoot, "private", "prompts.json"), "utf8"))[attempts[0].id].includes(join(runRoot, "attempts", attempts[0].id).replace(/\//g, "\\")), true);
  for (const a of attempts) {
    const c = locked.cases.find((x) => x.id === a.caseId);
    const answers = c.kind === "defect" ? s.json("cases", c.id, "answers.json").frames.variants.filter((v) => v.policy[a.condition]) : [];
    const findings = answers.length ? [canonicalFinding(answers[0], a.condition, textures)] : [];
    fs.writeFileSync(join(runRoot, "attempts", a.id, "out", "findings.json"), JSON.stringify({ findings }));
  }
  const clean = Object.fromEntries(attempts.map((a) => [a.id, { transcript: `${a.id}.jsonl`, calls: 3, violations: [], escapes: [] }]));
  fs.writeFileSync(join(runRoot, "private", "audit.json"), JSON.stringify(clean));
  const run = score({ storeRoot, runRoot });
  assert.equal(run.report.agents[0].primary.verdict.status, "GRADUATED", run.line);
  assert.equal(run.rows.filter((r) => r.class !== "control").every((r) => r.identified), true);
  // A voided attempt's answer is discarded, however good it is.
  const perfect = attempts.find((a) => a.condition === "primary" && locked.cases.find((c) => c.id === a.caseId).kind === "defect");
  clean[perfect.id].violations = ["Bash is not an allowed tool"];
  fs.writeFileSync(join(runRoot, "private", "audit.json"), JSON.stringify(clean));
  const voided = score({ storeRoot, runRoot });
  assert.deepEqual(["verdict", "identified"].map((k) => voided.rows.find((r) => r.id === perfect.id)[k]), ["voided", false]);
  assert.equal(voided.report.canary, "passed");
  clean[perfect.id].escapes = ["Read C:/elsewhere is outside the attempt"];
  fs.writeFileSync(join(runRoot, "private", "audit.json"), JSON.stringify(clean));
  assert.notEqual(score({ storeRoot, runRoot }).report.agents[0].primary.verdict.status, "GRADUATED");
});

test("derive-truth refuses the seed case, whose answer is a judgement of intent", async (t) => {
  const { storeRoot, exe, ids } = setUp(t);
  await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} });
  assert.throws(() => deriveTruth({ root: storeRoot, say: () => {}, diff: fakeDiff }), /seed case/);
});

// A store without the seed search and with one decoy: Fireball and Smoke swapped.
function setUpDecoy(t) {
  const set = setUp(t);
  const s = store(set.storeRoot);
  const skeleton = s.json("corpus.json");
  delete skeleton.seedSearch;
  s.writeJson(["corpus.json"], { ...skeleton, seedAbsence: "c" });
  const [fire, smoke] = FIRE();
  s.write(["authoring", "decoy", "effect.alo"], effectBytes([{ ...smoke, index: 0 }, { ...fire, index: 1 }]));
  s.writeJson(["authoring", "decoy", "recipe.json"], { schemaVersion: 1, base: "A", kind: "permutation", map: [1, 0], decoy: true });
  return { ...set, s };
}

test("a decoy reorder is a control the draft proves invisible, and its flags are reported apart", async (t) => {
  const { storeRoot, exe, ids, s } = setUpDecoy(t);
  const drafted = await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, diff: () => ({ ok: true, peak: 15 }), ids, say: () => {} });
  const map = s.json("authoring-map.json");
  const entry = drafted.cases.find((c) => c.id === map.decoy);
  assert.deepEqual([entry.kind, entry.shape, entry.decoy, entry.class], ["control", "single", true, undefined]);
  assert.ok(Object.values(s.json("cases", map.decoy, "decoy.json").peaks).length > 0, "the swapped fake frames differ, so every peak is recorded");
  assert.match(s.text("packs", map.decoy, "intent.md"), /- Smoke \(0\) initialDelay: 0\.5\n- Fireball \(1\) lifetime: 2\n/);
  assert.ok(s.exists("packs-bundle-only", map.decoy, "intent.md"), "a decoy has a bundle-only pack, like every control");

  deriveTruth({ root: storeRoot, say: () => {}, diff: fakeDiff });
  assert.deepEqual(s.json("cases", map.decoy, "truth.json").transports.frames, { visible: false, variants: [] });
  lock({ root: storeRoot, say: () => {} });
  assert.deepEqual(checkStore(storeRoot), []);

  // Every decoy attempt flagged (an unreadable answer is a flag); every other control clean.
  const runRoot = join(storeRoot, "..", "..", "runs", "run-1");
  const { attempts } = prepare({ storeRoot, runRoot, agent: "claude", model: "opus" });
  for (const a of attempts) fs.writeFileSync(join(runRoot, "attempts", a.id, "out", "findings.json"), a.caseId === map.decoy ? "not json" : JSON.stringify({ findings: [] }));
  fs.writeFileSync(join(runRoot, "private", "audit.json"), JSON.stringify(Object.fromEntries(attempts.map((a) => [a.id, { transcript: `${a.id}.jsonl`, calls: 3, violations: [], escapes: [] }]))));
  const run = score({ storeRoot, runRoot });
  const primary = run.report.agents[0].primary;
  assert.deepEqual([primary.decoyFlag, primary.controlFlag], [100, 50]);
  assert.match(markdown(run.report), /Decoy controls \(a reorder that cannot show; counted in the control flags\): flags 100\.0 %/);
});

test("draft refuses a decoy whose reorder shows", async (t) => {
  const { storeRoot, exe, ids } = setUpDecoy(t);
  await assert.rejects(draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, diff: () => ({ ok: true, peak: 16 }), ids, say: () => {} }),
    /decoy's reorder shows .* it is a defect, not a control/);
});

// A store without the seed search and with one draw-order case: the smoke, meant to draw over the fireball, moved behind it.
function setUpOrder(t) {
  const set = setUp(t);
  const s = store(set.storeRoot);
  const skeleton = s.json("corpus.json");
  delete skeleton.seedSearch;
  s.writeJson(["corpus.json"], { ...skeleton, seedAbsence: "c" });
  // Base A rewritten with unequal bursts: the fake tints a frame by position-weighted counts, so only then does the swap show where both live.
  const [fire, smoke] = [emitter(0, "Fireball", { lifetime: 2, blendMode: 1 }), emitter(1, "Smoke", { lifetime: 3, initialDelay: f(0.5), nParticlesPerBurst: 3 })];
  s.write(["bases", "A", "effect.alo"], effectBytes([fire, smoke]));
  s.writeJson(["corpus.json"], { ...s.json("corpus.json"), requiredClasses: ["draw-order"],
    bases: [{ id: "A", canonical: sha256(fs.readFileSync(s.at("bases", "A", "effect.alo"))) }, s.json("corpus.json").bases[1]] });
  for (const name of ["short", "late", "texture"]) fs.rmSync(s.at("authoring", name), { recursive: true });
  s.write(["authoring", "order", "effect.alo"], effectBytes([{ ...smoke, index: 0 }, { ...fire, index: 1 }]));
  s.writeJson(["authoring", "order", "recipe.json"], { schemaVersion: 1, base: "A", kind: "permutation", class: "draw-order", map: [1, 0],
    relation: { front: { index: 0, name: "Smoke" }, back: { index: 1, name: "Fireball" } } });
  return { ...set, s };
}

test("a draw-order case's bundle-only pack is order-blind: it reads like its base's control, and scores through the base's numbering", async (t) => {
  const { storeRoot, exe, ids, s } = setUpOrder(t);
  await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} });
  const map = s.json("authoring-map.json");
  // The primary pack states the case's numbering beside inspect.json; the bundle-only pack keeps the base's.
  assert.match(s.text("packs", map.order, "intent.md"), /- Smoke \(0\) initialDelay: 0\.5\n- Fireball \(1\) lifetime: 2\n/);
  assert.equal(s.text("packs-bundle-only", map.order, "intent.md"), s.text("bases", "A", "intent.md"));
  assert.deepEqual(JSON.parse(s.text("packs-bundle-only", map.order, "summary.json")).emitters.map((e) => `${e.index} ${e.name}`), ["0 Fireball", "1 Smoke"]);
  assert.deepEqual(readsDifferently(s.json("packs-bundle-only", `${map.order}.manifest.json`), s.json("packs-bundle-only", `${map["control-A"]}.manifest.json`)), []);
  assert.notDeepEqual(readsDifferently(s.json("packs", `${map.order}.manifest.json`), s.json("packs", `${map["control-A"]}.manifest.json`)), []);

  deriveTruth({ root: storeRoot, say: () => {}, diff: fakeDiff });
  const locked = lock({ root: storeRoot, say: () => {} });
  const c = locked.cases.find((x) => x.id === map.order);
  assert.deepEqual([c.class, c.secondaryEligible], ["draw-order", true]);
  assert.deepEqual(checkStore(storeRoot), []);

  // An answer in the pack's numbering is the right one; the case's numbering names the wrong emitters there.
  const textures = JSON.parse(s.text("packs", map.order, "textures.json"));
  const answers = s.json("cases", map.order, "answers.json").frames.variants;
  const context = contextFor(s, c, "bundle-only", textures);
  assert.deepEqual(context.toCase, [1, 0]);
  const inCase = canonicalFinding(answers[0], "bundle-only", textures);
  const inPack = renumberFinding(inCase, invertMap(context.toCase));
  assert.deepEqual([inPack.relation.front, inPack.relation.back], [{ index: 1, name: "Smoke" }, { index: 0, name: "Fireball" }]);
  const scored = (finding) => scoreAttempt({ caseInfo: c, answers, findings: [finding], context, rubric: RUBRIC });
  assert.equal(scored(inPack).identified, true);
  assert.equal(scored(inCase).identified, false);

  // Three bundle-only attempts for the draw-order case; one answers in the case's numbering and misses.
  const runRoot = join(storeRoot, "..", "..", "runs", "run-1");
  const { attempts } = prepare({ storeRoot, runRoot, agent: "claude", model: "opus" });
  const blind = attempts.filter((a) => a.caseId === map.order && a.condition === "bundle-only");
  assert.deepEqual(blind.map((a) => a.attempt).sort(), [1, 2, 3]);
  assert.equal(attempts.filter((a) => a.caseId === map["control-A"] && a.condition === "bundle-only").length, 3);
  for (const a of attempts) {
    const k = locked.cases.find((x) => x.id === a.caseId);
    const usable = k.kind === "defect" ? s.json("cases", k.id, "answers.json").frames.variants.filter((v) => v.policy[a.condition]) : [];
    let findings = usable.length ? [canonicalFinding(usable[0], a.condition, textures)] : [];
    if (a.caseId === map.order && a.condition === "bundle-only" && a.attempt !== 1) findings = findings.map((x) => renumberFinding(x, [1, 0]));
    // The clean control's pack reads the same as the order case's: the same claim there is a false alarm.
    if (a.caseId === map["control-A"] && a.condition === "bundle-only" && a.attempt !== 1) findings = [inPack];
    fs.writeFileSync(join(runRoot, "attempts", a.id, "out", "findings.json"), JSON.stringify({ findings }));
  }
  fs.writeFileSync(join(runRoot, "private", "audit.json"), JSON.stringify(Object.fromEntries(attempts.map((a) => [a.id, { transcript: `${a.id}.jsonl`, calls: 3, violations: [], escapes: [] }]))));
  const run = score({ storeRoot, runRoot });
  assert.equal(run.report.agents[0].bundleOnly.classes["draw-order"].scheduled, 3);
  assert.match(markdown(run.report), /draw order from the frames alone 66\.7 % \(diagnosis 66\.7 %; a draw-order claim on the same bases' clean controls 66\.7 %\)/);
});

test("the order-blind comparison sets the frames aside and catches every other difference", () => {
  const m = (...files) => ({ files: files.map(([path, sha256]) => ({ path, sha256 })) });
  assert.deepEqual(readsDifferently(m(["intent.md", "a"], ["frames/frame-00000.png", "x"]), m(["intent.md", "a"], ["frames/frame-00000.png", "y"])), []);
  assert.deepEqual(readsDifferently(m(["summary.json", "a"], ["intent.md", "a"]), m(["summary.json", "b"], ["intent.md", "a"])), ["summary.json"]);
  assert.deepEqual(readsDifferently(m(["frames/frame-00000.png", "x"]), m(["frames/frame-00015.png", "x"])), ["frames/frame-00000.png", "frames/frame-00015.png"]);
});

test("draft refuses a base without its canonicalisation proof, and a case that is not what its recipe says", async (t) => {
  const { storeRoot, exe, ids } = setUp(t);
  const s = store(storeRoot);
  const corpus = s.json("corpus.json");
  corpus.bases[0].canonical = "0".repeat(64);
  s.writeJson(["corpus.json"], corpus);
  await assert.rejects(draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} }), /canonicalisation proof/);

  corpus.bases[0].canonical = sha256(s.read("bases", "A", "effect.alo"));
  s.writeJson(["corpus.json"], corpus);
  const recipe = s.json("authoring", "short", "recipe.json");
  recipe.mutations[0].value = 0.9;   // the authored file says 0.8
  s.writeJson(["authoring", "short", "recipe.json"], recipe);
  await assert.rejects(draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} }), /differs from the patched base/);
});

test("lock refuses an incomplete blind pass, an unscorable subjective window and an objective window outside its candidate", async (t) => {
  const { storeRoot, exe, ids } = setUp(t);
  const drafted = await draft({ root: storeRoot, exe, run: fakeBenchExe(), ssim: fakeSsim, ids, say: () => {} });
  assert.throws(() => lock({ root: storeRoot, say: () => {} }), /no truth.json/);
  const s = store(storeRoot);
  const map = s.json("authoring-map.json");
  blindPass(s, drafted, map);
  // A subjective window is the owner's own frames, but they must exist in the pack.
  const truth = s.json("cases", map.short, "truth.json");
  const good = structuredClone(truth);
  truth.transports.frames.variants[0].windows.bundle.image = [1];
  s.writeJson(["cases", map.short, "truth.json"], truth);
  assert.throws(() => lock({ root: storeRoot, say: () => {} }), new RegExp(`${map.short} frames: primary: answer 0`));
  s.writeJson(["cases", map.short, "truth.json"], good);
  // An objective window frame must come from the drift floor's candidate.
  const tex = s.json("cases", map.texture, "truth.json");
  tex.transports.frames.variants[0].policy = { primary: "either", "bundle-only": null };
  tex.transports.frames.variants[0].windows.bundle.image = [1];
  s.writeJson(["cases", map.texture, "truth.json"], tex);
  assert.throws(() => lock({ root: storeRoot, say: () => {} }), /outside its candidate window/);
});
