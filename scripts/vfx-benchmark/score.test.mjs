import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { interveneContract, nextFloat32, restoreContract, seedContract } from "./persisted.mjs";
import { lockAnswerCheck, scoreAttempt } from "./score.mjs";
import { invertMap, renumberFinding } from "./semantic.mjs";
import { mergeAnswers } from "./answers.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const rubric = JSON.parse(readFileSync(join(here, "rubric.json"), "utf8"));
const f = Math.fround;

// The case: the fireball's lifetime planted from 4 down to 2.
const FIRE = { index: 0, name: "Fireball" }, SMOKE = { index: 1, name: "Smoke" }, SPARKS = { index: 2, name: "Sparks" };
const steps = [[0, 0], [25, 1], [50, 2], [75, 3], [100, 3]];
const tracks = (index) => Array.from({ length: 7 }, (_, i) => ({ interpolation: i === 5 ? 2 : 0, keys: i === 5 ? index : [[0, 1], [100, 1]] }));
const EMITTERS = [
  { ...FIRE, lifetime: 2, initialDelay: 0, randomLifetimePerc: 0, blendMode: 2, colorTexture: "fire.tga", textureSize: 16, tracks: tracks(steps.map(([t, v]) => [t, 3 - v])) },
  { ...SMOKE, lifetime: 3, initialDelay: f(0.2), randomLifetimePerc: f(0.1), blendMode: 2, colorTexture: "smoke.tga", textureSize: 16, tracks: tracks(steps) },
  { ...SPARKS, lifetime: 1, initialDelay: 0, randomLifetimePerc: 0, blendMode: 1, colorTexture: "spark.tga", textureSize: 4, tracks: tracks([[0, 0], [100, 0]]) },
];
const context = (over = {}) => ({
  condition: "primary", inspectInPack: true, emitters: EMITTERS, textures: ["fire.tga", "smoke.tga", "spark.tga"],
  bundles: { bundle: { imageFrames: [0, 15, 30, 44, 45, 46, 60], lastFrame: 90 } }, ...over,
});
const win = (image, telemetry = []) => ({ bundle: { image, telemetry } });
const BOTH = { primary: "frames", "bundle-only": "frames" };

const V0 = { id: 0, bundleId: "bundle", category: "duration", direction: "too-short", emitters: [FIRE], windows: win([44, 45, 46], [44, 45]),
  policy: BOTH, patchEligible: true, correction: { kind: "scalar", emitter: FIRE, field: "lifetime", accept: restoreContract("lifetime", 4, 2) } };
const V1 = { id: 1, bundleId: "bundle", category: "timing", direction: "too-late", emitters: [SMOKE], windows: win([45, 46]),
  policy: BOTH, patchEligible: true, correction: { kind: "scalar", emitter: SMOKE, field: "initialDelay", accept: interveneContract("initialDelay", f(0.2), f(0.2), "decrease") } };
const DEFECT = { kind: "defect" };

const finding = (over = {}) => ({
  severity: "medium", category: "duration", direction: "too-short", claim: "the fireball ends early",
  suspectedEmitters: [FIRE], evidence: { citations: [{ kind: "image", frame: 45 }, { kind: "image", frame: 46 }] },
  suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 3.5 }, confidence: 0.8, ...over,
});
const smokeFinding = (over = {}) => finding({ category: "timing", direction: "too-late", suspectedEmitters: [SMOKE],
  suggestedChange: { kind: "scalar", emitter: SMOKE, field: "initialDelay", value: 0.15 }, ...over });
const cite = (...c) => ({ citations: c });
const img = (frame, bundleId) => ({ kind: "image", frame, ...(bundleId ? { bundleId } : {}) });
const tel = (frame, bundleId) => ({ kind: "telemetry", frame, ...(bundleId ? { bundleId } : {}) });

const score = (findings, { answers = [V0, V1], ctx = context(), caseInfo = DEFECT } = {}) =>
  scoreAttempt({ caseInfo, answers, findings, context: ctx, rubric });
const labels = (s) => s.units.map((u) => u.label).sort();

function permutations(items) {
  if (items.length <= 1) return [items];
  return items.flatMap((x, i) => permutations([...items.slice(0, i), ...items.slice(i + 1)]).map((p) => [x, ...p]));
}
// Every output order of the findings must give a byte-identical score.
function stable(findings, options) {
  const first = JSON.stringify(score(findings, options));
  for (const p of permutations(findings)) assert.equal(JSON.stringify(score(p, options)), first);
  return JSON.parse(first);
}

test("a full single-finding match is identified, via the recipe's answer or an alternative", () => {
  const s = score([finding()]);
  assert.equal(s.identified, true);
  assert.deepEqual([s.tp, s.redundant, s.fp], [1, 0, 0]);
  assert.equal(s.units[0].answer, 0);
  const alt = score([smokeFinding()]);
  assert.equal(alt.identified, true);
  assert.equal(alt.units[0].answer, 1);
});

test("two fully correct alternative answers: one TP, one redundant, never an FP", () => {
  const s = stable([smokeFinding(), finding()]);
  assert.deepEqual([s.tp, s.redundant, s.fp], [1, 1, 0]);
  assert.equal(s.units.find((u) => u.label === "tp").answer, 0);
});

test("clauses are never assembled across findings", () => {
  const diagnosisOnly = finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 2.1 } });
  const correctionOnly = finding({ category: "variation", direction: "too-much-variation" });
  const s = stable([diagnosisOnly, correctionOnly]);
  assert.equal(s.identified, false);
  assert.deepEqual([s.tp, s.fp], [0, 2]);
  assert.equal(s.diagnosisOnly, true);
});

for (const [name, over] of [
  ["a category-only match", { direction: "too-long" }],
  ["a direction mismatch", { direction: "too-long" }],
  ["a wrong emitter", { suspectedEmitters: [SMOKE] }],
  ["an extra emitter", { suspectedEmitters: [FIRE, SPARKS] }],
  ["a cited frame absent from the bundle", { evidence: cite(img(45), img(47)) }],
  ["a telemetry citation outside the review window", { evidence: cite(img(45), tel(91)) }],
  ["a telemetry tuple counted against the image window", { evidence: cite(tel(46)) }],
  ["an inspect citation counted toward the majority", { evidence: cite(img(45), img(60), { kind: "inspect", emitter: FIRE, field: "lifetime" }) }],
  ["an empty citation list", { evidence: cite() }],
  ["exactly half the tuples inside the window", { evidence: cite(img(45), img(60)) }],
  ["a mis-typed correction", { suggestedChange: { kind: "enum", emitter: FIRE, field: "blendMode", value: 1 } }],
  ["a wrong-field correction", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "initialDelay", value: 0.1 } }],
  ["a correction on another emitter", { suggestedChange: { kind: "scalar", emitter: SMOKE, field: "lifetime", value: 3.5 } }],
  ["a wrong-sign correction", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 1.5 } }],
  ["an overshoot past the base", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 5 } }],
  ["a float32-adjacent nudge", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: nextFloat32(2, "increase") } }],
  ["just short of the quarter restoration", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: nextFloat32(2.5, "decrease") } }],
  ["just past the base", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: nextFloat32(4, "increase") } }],
  ["a non-finite value", { suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: Infinity } }],
  ["an unknown emitter", { suspectedEmitters: [{ index: 0, name: "Fire" }] }],
]) test(`not identified on ${name}, which counts as an FP`, () => {
  const s = score([finding(over)]);
  assert.equal(s.identified, false, name);
  assert.equal(s.fp, 1);
});

test("the quarter restoration and the base itself both pass", () => {
  for (const value of [2.5, 4]) assert.equal(score([finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value } })]).identified, true);
});

test("image and telemetry tuples at the same frame count separately", () => {
  // 44 is in both windows; 15 in neither: 2 of 3 inside.
  assert.equal(score([finding({ evidence: cite(img(44), tel(44), img(15)) })]).identified, true);
  // 46 image inside; 46 telemetry outside the telemetry window [44, 45]: 1 of 2.
  assert.equal(score([finding({ evidence: cite(img(46), tel(46)) })]).identified, false);
});

test("mixed-kind majority: one inside image tuple passes, plus one existent telemetry tuple on an image-only answer fails", () => {
  const imageOnly = { ...V0, windows: win([45]) };
  assert.equal(score([finding({ evidence: cite(img(45)) })], { answers: [imageOnly] }).identified, true);
  assert.equal(score([finding({ evidence: cite(img(45), tel(45)) })], { answers: [imageOnly] }).identified, false);
  const telemetryOnly = { ...V0, windows: win([], [45]) };
  assert.equal(score([finding({ evidence: cite(tel(45)) })], { answers: [telemetryOnly] }).identified, true);
  assert.equal(score([finding({ evidence: cite(tel(45), img(45)) })], { answers: [telemetryOnly] }).identified, false);
});

test("evidence policies: inspect needs an inspect citation; either passes on frames or inspect", () => {
  const inspectOnly = { ...V0, windows: win([]), policy: { primary: "inspect", "bundle-only": null } };
  const byInspect = finding({ evidence: cite({ kind: "inspect", emitter: FIRE, field: "lifetime" }) });
  assert.equal(score([finding()], { answers: [inspectOnly] }).identified, false);
  assert.equal(score([byInspect], { answers: [inspectOnly] }).identified, true);
  const either = { ...V0, policy: { primary: "either", "bundle-only": "frames" } };
  assert.equal(score([byInspect], { answers: [either] }).identified, true);
  assert.equal(score([finding()], { answers: [either] }).identified, true);
  // The bundle-only pack has no inspect.json: the same inspect citation proves nothing there.
  const bundleOnly = context({ condition: "bundle-only", inspectInPack: false });
  assert.equal(score([byInspect], { answers: [either], ctx: bundleOnly }).identified, false);
  assert.equal(score([finding()], { answers: [either], ctx: bundleOnly }).identified, true);
  assert.equal(score([finding()], { answers: [inspectOnly], ctx: bundleOnly }).identified, false);
  // An inspect citation proves nothing in a pack that carries no inspect.json.
  assert.equal(score([byInspect], { answers: [inspectOnly], ctx: context({ inspectInPack: false }) }).identified, false);
});

test("an incomplete and a complete finding with one key: one TP and one FP in either order", () => {
  const s = stable([finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 2.1 } }), finding()]);
  assert.deepEqual([s.tp, s.fp], [1, 1]);
});

test("exact duplicates collapse: byte-identical, float32-equal, confidence or severity apart", () => {
  const same = stable([finding(), finding()]);
  assert.deepEqual([same.tp, same.redundant, same.fp, same.redundantEntries], [1, 0, 0, 1]);
  const v = (value) => finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value } });
  const near = stable([v(0.25 * 12), v(3.0000000000000004)]);
  assert.deepEqual([near.tp, near.fp, near.redundantEntries], [1, 0, 1]);
  const conf = stable([finding({ confidence: 0.9 }), finding({ confidence: 0.1 })]);
  assert.deepEqual([conf.tp, conf.redundant, conf.fp, conf.redundantEntries], [1, 0, 0, 1]);
  assert.deepEqual(conf.calibration, [[0.9, 1]]);
  assert.ok(Math.abs(conf.brier - 0.01) < 1e-12);
  const sev = stable([finding({ severity: "low" }), finding({ severity: "high" })]);
  assert.equal(sev.units[0].severity, "high");
  assert.equal(sev.bySeverity.low.redundantEntries, 1);
  assert.equal(sev.bySeverity.high.tp, 1);
});

test("a valid 0 and an invalid -1e-50 on a fraction are one TP and one FP, whichever is more confident", () => {
  const V = { ...V0, emitters: [SMOKE], correction: { kind: "scalar", emitter: SMOKE, field: "randomLifetimePerc",
    accept: restoreContract("randomLifetimePerc", 0, f(0.9)) }, category: "variation", direction: "too-much-variation" };
  const base = { category: "variation", direction: "too-much-variation", suspectedEmitters: [SMOKE] };
  for (const [a, b] of [[0.9, 0.1], [0.1, 0.9]]) {
    const s = stable([
      finding({ ...base, confidence: a, suggestedChange: { kind: "scalar", emitter: SMOKE, field: "randomLifetimePerc", value: 0 } }),
      finding({ ...base, confidence: b, suggestedChange: { kind: "scalar", emitter: SMOKE, field: "randomLifetimePerc", value: -1e-50 } }),
    ], { answers: [V] });
    assert.deepEqual([s.tp, s.fp], [1, 1]);
  }
});

test("failed findings de-duplicate among themselves by raw correction identity", () => {
  const unknown = (value) => finding({ suspectedEmitters: [{ index: 9, name: "Ghost" }], suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value } });
  const s = stable([unknown(3), unknown(3.0000000000000004)]);
  assert.deepEqual([s.fp, s.redundantEntries], [2, 0]);
  const same = stable([unknown(3), unknown(3)]);
  assert.deepEqual([same.fp, same.redundantEntries], [1, 1]);
});

test("a same-key shotgun scores at most one TP", () => {
  const shots = [finding()];
  for (let i = 0; i < 50; i++) shots.push(finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 2.6 + i * 0.01 } }));
  for (let i = 0; i < 49; i++) shots.push(finding({ evidence: cite(img(i % 2 ? 0 : 15)), suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 3 + i * 0.001 } }));
  const s = score(shots);
  assert.equal(s.tp, 1);
  assert.equal(s.redundant, 0);
  assert.equal(s.fp, 99 - 1 + 1);
  const sample = [shots.slice().reverse(), shots.slice(1).concat(shots[0])];
  for (const order of sample) assert.equal(JSON.stringify(score(order)), JSON.stringify(s));
});

test("the overlap counterexample: X matches V1 and V2, Y matches V1 only, in both orders", () => {
  const V2 = { ...V0, id: 2, correction: { ...V0.correction, accept: { ...V0.correction.accept, interval: [3, 5] } } };
  // X sorts first (its citations do), takes V0, and must be moved to V2 by an augmenting path.
  const X = finding({ evidence: cite(img(44), img(45)), suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 3.5 } });
  const Y = finding({ evidence: cite(img(45), img(46)), suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 2.6 } });
  const s = stable([X, Y], { answers: [V0, V2] });
  assert.deepEqual([s.tp, s.redundant, s.fp], [1, 1, 0]);
  const alone = stable([X], { answers: [V0, V2] });
  assert.deepEqual([alone.tp, alone.redundant], [1, 0]);
});

test("a third finding on an already-assigned answer with other citations is an FP", () => {
  const s = stable([finding(), finding({ evidence: cite(img(44), img(45)) }), smokeFinding()]);
  assert.deepEqual([s.tp, s.redundant, s.fp], [1, 1, 1]);
});

test("a duplicate contract planted past lock collapses: proposals 3 and 4 stay TP + FP", () => {
  const twin = { ...V0, id: 7 };
  for (const answers of [[V0], [V0, twin]]) {
    const s = stable([finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 3 } }),
      finding({ suggestedChange: { kind: "scalar", emitter: FIRE, field: "lifetime", value: 4 } })], { answers });
    assert.deepEqual([s.tp, s.redundant, s.fp], [1, 0, 1]);
  }
  assert.throws(() => mergeAnswers([V0, twin], { strict: true }), /duplicates/);
});

test("window-only variants merge into one answer; splitting a window changes nothing", () => {
  const wide = { ...V0, windows: win([45, 46]) };
  const narrow = { ...V0, id: 3, windows: win([45]) };
  const merged = mergeAnswers([wide, narrow], { strict: true });
  assert.equal(merged.length, 1);
  const pair = [finding({ evidence: cite(img(45)) }), finding({ evidence: cite(img(46)) })];
  const one = stable(pair, { answers: [wide] });
  const split = stable(pair, { answers: [wide, narrow] });
  assert.deepEqual([one.tp, one.fp], [1, 1]);
  assert.equal(JSON.stringify(split), JSON.stringify(one));
});

test("one contract under two evidence policies is refused in either order", () => {
  const inspect = { ...V0, id: 4, windows: win([]), policy: { primary: "inspect", "bundle-only": null } };
  assert.throws(() => mergeAnswers([V0, inspect], { strict: true }), /different evidence policies/);
  assert.throws(() => mergeAnswers([inspect, V0], { strict: true }), /different evidence policies/);
});

test("patch eligibility comes from every matched answer", () => {
  const ineligible = { ...V0, patchEligible: false };
  const s = score([finding()], { answers: [ineligible, V1] });
  assert.equal(s.identified, true);
  assert.equal(s.patchEligible, false);
  const both = score([finding(), smokeFinding()], { answers: [ineligible, V1] });
  assert.equal(both.patchEligible, true);
});

test("a texture proposal must be in the inventory", () => {
  const V = { ...V0, category: "missing-texture", direction: "unresolved", correction: { kind: "texture", emitter: FIRE, field: "colorTexture" } };
  const tex = (name) => finding({ category: "missing-texture", direction: "unresolved", suggestedChange: { kind: "texture", emitter: FIRE, field: "colorTexture", name } });
  assert.equal(score([tex("smoke.tga")], { answers: [V] }).identified, true);
  assert.equal(score([tex("fire2.tga")], { answers: [V] }).identified, false);
});

test("enum, track and reorder answers", () => {
  const blend = { ...V0, category: "blend-mode", direction: "should-be-additive", correction: { kind: "enum", emitter: FIRE, field: "blendMode", acceptable: [1, 4] } };
  const enumFinding = (value) => finding({ category: "blend-mode", direction: "should-be-additive", suggestedChange: { kind: "enum", emitter: FIRE, field: "blendMode", value } });
  assert.equal(score([enumFinding(4)], { answers: [blend] }).identified, true);
  assert.equal(score([enumFinding(2)], { answers: [blend] }).identified, false);
  // Two bases differing only in original mode give identical packs and the same verdict.
  assert.equal(JSON.stringify(score([enumFinding(1)], { answers: [blend] })), JSON.stringify(score([enumFinding(1)], { answers: [{ ...blend }] })));

  const flip = { ...V0, category: "flipbook", direction: "reversed", correction: { kind: "track", emitter: FIRE, track: 5, baseKeys: steps, cells: 16 } };
  const trackFinding = (keys) => finding({ category: "flipbook", direction: "reversed", suggestedChange: { kind: "track", emitter: FIRE, track: 5, keys } });
  assert.equal(score([trackFinding(steps.map(([t, v]) => [t, v + 0.25]))], { answers: [flip] }).identified, true);
  assert.equal(score([trackFinding(EMITTERS[0].tracks[5].keys)], { answers: [flip] }).identified, false);
  const invalid = score([trackFinding([[0, 0], [25, 1], [25, 2], [100, 3]])], { answers: [flip] });
  assert.match(invalid.units[0].errors[0], /strictly after/);

  const relation = { front: SPARKS, back: FIRE };
  const order = { ...V0, category: "draw-order", direction: "wrong-order", emitters: [FIRE, SPARKS], relation, correction: { kind: "reorder" } };
  const orderFinding = (rel) => finding({ category: "draw-order", direction: "wrong-order", suspectedEmitters: [SPARKS, FIRE], relation: rel,
    suggestedChange: { kind: "reorder", ...rel } });
  assert.equal(score([orderFinding(relation)], { answers: [order] }).identified, true);
  assert.equal(score([orderFinding({ front: FIRE, back: SPARKS })], { answers: [order] }).identified, false);
});

test("controls: any finding flags, whatever its severity; a terminal failure flags", () => {
  const control = { kind: "control" };
  assert.equal(score([], { answers: [], caseInfo: control }).flagged, false);
  const low = score([finding({ severity: "low" })], { answers: [], caseInfo: control });
  assert.equal(low.flagged, true);
  assert.equal(low.fp, 1);
  assert.equal(score(null, { answers: [], caseInfo: control }).flagged, true);
});

test("a defect case with no findings, or a terminal failure, is not identified", () => {
  assert.equal(score([]).identified, false);
  const failed = score(null);
  assert.equal(failed.identified, false);
  assert.equal(failed.completed, false);
});

test("the seed case: a three-bundle pack scores the outlier's bundle only", () => {
  const ctx = context({ bundles: { "bundle-a": { imageFrames: [0, 30, 60], lastFrame: 90 }, "bundle-b": { imageFrames: [0, 30, 60], lastFrame: 90 }, "bundle-c": { imageFrames: [0, 30, 60], lastFrame: 90 } } });
  const seed = { id: 0, bundleId: "bundle-b", category: "variation", direction: "too-much-variation", emitters: [SMOKE],
    windows: { "bundle-b": { image: [30, 60], telemetry: [] } }, policy: BOTH, patchEligible: true,
    correction: { kind: "scalar", emitter: SMOKE, field: "randomLifetimePerc", accept: seedContract("randomLifetimePerc", f(0.1), "decrease") } };
  const s = (bundleId, citations, value = 0.05) => score([finding({ bundleId, category: "variation", direction: "too-much-variation", suspectedEmitters: [SMOKE],
    evidence: { citations }, suggestedChange: { kind: "scalar", emitter: SMOKE, field: "randomLifetimePerc", value } })], { answers: [seed], ctx });
  assert.equal(s("bundle-b", [img(30, "bundle-b"), img(60, "bundle-b")]).identified, true);
  assert.equal(s("bundle-a", [img(30, "bundle-a"), img(60, "bundle-a")]).identified, false);
  assert.equal(s("bundle-b", [img(30, "bundle-a"), img(60, "bundle-b")]).identified, false);
  // A foreign tuple fails the finding even when the majority would hold without it.
  assert.equal(s("bundle-b", [img(30, "bundle-b"), img(60, "bundle-b"), img(0, "bundle-a")]).identified, false);
  assert.equal(s("bundle-b", [img(30, "bundle-b")], 0.08).identified, false);
  assert.match(s(undefined, [img(30, "bundle-b")]).units[0].errors.join(), /bundleId/);
});

test("severity never filters: severity-parking fails the control-flag rate and the FDP", () => {
  const control = { kind: "control" };
  const parked = Array.from({ length: 200 }, (_, i) => finding({ severity: "low", claim: `p${i}`, evidence: cite(img(15 + (i % 2) * 15)) }));
  const c = score(parked, { answers: [], caseInfo: control });
  assert.equal(c.flagged, true);
  assert.equal(c.bySeverity.low.fp + c.bySeverity.low.redundantEntries, 200);
});

test("the lock-time check passes every canonical answer and refuses an unpassable one", () => {
  const contexts = { primary: context(), "bundle-only": context({ condition: "bundle-only", inspectInPack: false }) };
  assert.deepEqual(lockAnswerCheck({ caseInfo: DEFECT, variants: [V0, V1], contexts, rubric }), []);
  assert.deepEqual(lockAnswerCheck({ caseInfo: { kind: "control" }, variants: [], contexts, rubric }), []);
  const unpassable = { ...V1, windows: win([47]) };
  assert.match(lockAnswerCheck({ caseInfo: DEFECT, variants: [V0, unpassable], contexts, rubric }).join("\n"), /answer 1/);
  const bad = { ...V1, policy: { primary: "inspect", "bundle-only": "frames" }, windows: win([]) };
  assert.match(lockAnswerCheck({ caseInfo: DEFECT, variants: [V0, bad], contexts, rubric }).join("\n"), /bundle-only/);
});

test("an order-blind pack: every reference is renumbered to the case's before it is checked, and the lock check writes as an agent there would", () => {
  // The pack numbers the emitters smoke 0, sparks 1, fireball 2; toCase[packIndex] = caseIndex.
  const toCase = [1, 2, 0];
  assert.deepEqual(invertMap(toCase), [2, 0, 1]);
  assert.throws(() => invertMap([0, 0]), /not a permutation/);
  assert.throws(() => invertMap([0, 2]), /not a permutation/);
  const PACK_FIRE = { index: 2, name: "Fireball" };
  const moved = renumberFinding({ ...finding({ suspectedEmitters: [PACK_FIRE, { index: 7, name: "Ghost" }], suggestedChange: { kind: "scalar", emitter: PACK_FIRE, field: "lifetime", value: 3.5 } }),
    relation: { front: { index: 0, name: "Smoke" }, back: PACK_FIRE }, evidence: cite(img(45), { kind: "inspect", emitter: PACK_FIRE, field: "lifetime" }) }, toCase);
  assert.deepEqual(moved.suspectedEmitters, [FIRE, { index: 7, name: "Ghost" }], "an index outside the map stays, and stays unknown");
  assert.deepEqual([moved.relation.front, moved.relation.back, moved.suggestedChange.emitter, moved.evidence.citations[1].emitter], [SMOKE, FIRE, FIRE, FIRE]);
  assert.deepEqual(moved.evidence.citations[0], img(45));

  const blind = context({ condition: "bundle-only", inspectInPack: false, toCase });
  const inPack = finding({ suspectedEmitters: [PACK_FIRE], suggestedChange: { kind: "scalar", emitter: PACK_FIRE, field: "lifetime", value: 3.5 } });
  assert.equal(score([inPack], { ctx: blind }).identified, true);
  assert.equal(score([finding()], { ctx: blind }).identified, false, "the case's numbering names the smoke's slot there");
  assert.deepEqual(lockAnswerCheck({ caseInfo: DEFECT, variants: [V0, V1], contexts: { primary: context(), "bundle-only": blind }, rubric }), []);
});
