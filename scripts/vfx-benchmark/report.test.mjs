import { test } from "node:test";
import assert from "node:assert/strict";
import { aggregate, buildReport, markdown, publicReport, scope, verdict } from "./report.mjs";

const TARGETS = { objective: 80, subjective: 65, controlFlag: 15, fdp: 30, completion: 90 };
const sev = () => ({ low: { tp: 0, redundant: 0, fp: 0, redundantEntries: 0 }, medium: { tp: 0, redundant: 0, fp: 0, redundantEntries: 0 }, high: { tp: 0, redundant: 0, fp: 0, redundantEntries: 0 } });
// A score as score.mjs writes it.
const hit = (eligible = true) => ({ completed: true, identified: true, diagnosisOnly: true, flagged: null, patchEligible: eligible,
  tp: 1, redundant: 0, fp: 0, redundantEntries: 0, bySeverity: { ...sev(), medium: { tp: 1, redundant: 0, fp: 0, redundantEntries: 0 } },
  calibration: [[0.9, 1]], unmapped: [] });
const miss = (fp = 1) => ({ completed: true, identified: false, diagnosisOnly: false, flagged: null, patchEligible: false,
  tp: 0, redundant: 0, fp, redundantEntries: 0, bySeverity: { ...sev(), low: { tp: 0, redundant: 0, fp, redundantEntries: 0 } },
  calibration: Array.from({ length: fp }, () => [0.5, 0]), unmapped: [] });
const clean = () => ({ ...miss(0), flagged: false });
const flagged = () => ({ ...miss(1), flagged: true });
const failed = () => null;

// Ten objective cases on base A/B, four subjective, one control per base.
function corpus(over = {}) {
  const cases = [];
  for (let i = 0; i < 10; i++) cases.push({ id: `o${i}`, kind: "defect", class: `obj${i % 5}`, base: i < 5 ? "A" : "B", stratum: "objective", shape: "single" });
  for (let i = 0; i < 4; i++) cases.push({ id: `s${i}`, kind: "defect", class: `sub${i}`, base: "A", stratum: "subjective", shape: "single", secondaryEligible: i < 2 });
  cases.push({ id: "cA", kind: "control", base: "A", shape: "single" }, { id: "cB", kind: "control", base: "B", shape: "single" });
  return { targets: TARGETS, attempts: 3, cases, reducedClassList: null, seedAbsence: "b", ...over };
}
const RUN = { agents: [{ id: "claude", model: "m", imageCondition: "frames" }], canary: "passed", probes: { registry: "passed" }, contentRoots: "denied", backend: "elevated" };

// scores from a per-case, per-attempt chooser.
function scoresFor(c, choose, condition = "primary") {
  const out = [];
  for (const k of c.cases) for (let a = 1; a <= c.attempts; a++) out.push({ agent: "claude", condition, caseId: k.id, attempt: a, score: choose(k, a) });
  return out;
}
const baseline = (k) => (k.kind === "control" ? clean() : hit());

test("a run clearing every threshold graduates and grants scope", () => {
  const c = corpus();
  const r = buildReport(c, RUN, scoresFor(c, baseline));
  assert.equal(r.agents[0].primary.verdict.status, "GRADUATED");
  assert.ok(r.agents[0].primary.scope.granted.includes("obj0@A"));
});

test("objective at exactly 80 % passes, one attempt fewer fails", () => {
  const c = corpus();
  // 30 objective attempts; 24 identified = 80 %.
  const at = (n) => scoresFor(c, (k, a) => (k.kind === "control" ? clean() : k.stratum === "objective" && Number(k.id.slice(1)) * 3 + a > n ? miss(0) : hit()));
  assert.equal(verdict(c, RUN, aggregate(c, at(24), "claude", "primary", c.cases)).passes.objective, true);
  assert.equal(verdict(c, RUN, aggregate(c, at(23), "claude", "primary", c.cases)).passes.objective, false);
});

test("subjective at exactly 65 % passes, below fails", () => {
  const c = corpus({ cases: [...corpus().cases.filter((k) => k.stratum !== "subjective"),
    ...Array.from({ length: 20 }, (_, i) => ({ id: `s${i}`, kind: "defect", class: "sub", base: "A", stratum: "subjective", shape: "single" }))], attempts: 1 });
  const at = (n) => scoresFor(c, (k) => (k.kind === "control" ? clean() : k.stratum === "subjective" && Number(k.id.slice(1)) >= n ? miss(0) : hit()));
  assert.equal(verdict(c, RUN, aggregate(c, at(13), "claude", "primary", c.cases)).passes.subjective, true);
  assert.equal(verdict(c, RUN, aggregate(c, at(12), "claude", "primary", c.cases)).passes.subjective, false);
});

test("control flags below 15 % pass, exactly 15 % fails", () => {
  const cases = [{ id: "o", kind: "defect", class: "x", base: "A", stratum: "objective", shape: "single" },
    ...Array.from({ length: 20 }, (_, i) => ({ id: `c${i}`, kind: "control", base: i ? `B${i}` : "A", shape: "single" }))];
  const c = corpus({ cases, attempts: 1 });
  const flags = (n) => scoresFor(c, (k) => (k.kind === "control" ? (Number(k.id.slice(1)) < n ? flagged() : clean()) : hit()));
  assert.equal(verdict(c, RUN, aggregate(c, flags(2), "claude", "primary", c.cases)).passes.controlFlag, true);
  assert.equal(verdict(c, RUN, aggregate(c, flags(3), "claude", "primary", c.cases)).passes.controlFlag, false);
});

test("FDP at exactly 30 % passes, above fails, and 0/0 is undefined, never 0", () => {
  const c = corpus({ cases: [{ id: "o", kind: "defect", class: "x", base: "A", stratum: "objective", shape: "single" }, { id: "cA", kind: "control", base: "A", shape: "single" }], attempts: 1 });
  const with7 = { ...hit(), tp: 7, fp: 3 };
  const with6 = { ...hit(), tp: 6, fp: 3 };
  assert.equal(verdict(c, RUN, aggregate(c, scoresFor(c, (k) => (k.kind === "control" ? clean() : with7)), "claude", "primary", c.cases)).passes.fdp, true);
  assert.equal(verdict(c, RUN, aggregate(c, scoresFor(c, (k) => (k.kind === "control" ? clean() : with6)), "claude", "primary", c.cases)).passes.fdp, false);
  const none = buildReport(c, RUN, scoresFor(c, (k) => (k.kind === "control" ? clean() : miss(0))));
  assert.equal(none.agents[0].primary.fdp, null);
  assert.equal(none.agents[0].primary.verdict.passes.fdp, false);
});

test("completion at exactly 90 % is valid, below is INVALID with no verdict", () => {
  const cases = Array.from({ length: 10 }, (_, i) => ({ id: `o${i}`, kind: "defect", class: "x", base: "A", stratum: "objective", shape: "single" }));
  const c = corpus({ cases: [...cases, { id: "cA", kind: "control", base: "A", shape: "single" }], attempts: 1 });
  const fail = (n) => scoresFor(c, (k) => (k.kind === "control" ? clean() : Number(k.id.slice(1)) < n ? failed() : hit()));
  assert.notEqual(verdict(c, RUN, aggregate(c, fail(1), "claude", "primary", c.cases)).status, "INVALID");
  assert.equal(verdict(c, RUN, aggregate(c, fail(2), "claude", "primary", c.cases)).status, "INVALID");
});

test("a missing control stratum yields no verdict", () => {
  const c = corpus();
  c.cases = c.cases.filter((k) => k.id !== "cB");
  assert.equal(buildReport(c, RUN, scoresFor(c, baseline)).agents[0].primary.verdict.status, "NO VERDICT");
});

test("a reduced class list, a failed canary or a failed probe ends INCOMPLETE with no scope", () => {
  const c = corpus({ reducedClassList: ["draw-order"] });
  const r = buildReport(c, RUN, scoresFor(c, baseline));
  assert.equal(r.agents[0].primary.verdict.status, "INCOMPLETE");
  assert.match(r.agents[0].primary.verdict.reasons.join(), /draw-order/);
  assert.deepEqual(r.agents[0].primary.scope.granted, []);
  for (const run of [{ ...RUN, canary: "inconclusive" }, { ...RUN, probes: { registry: "failed" } }]) {
    const x = buildReport(corpus(), run, scoresFor(corpus(), baseline)).agents[0].primary;
    assert.equal(x.verdict.status, "INCOMPLETE");
    assert.deepEqual(x.scope.granted, []);
  }
});

test("the per-base scope gate: 3 / 21 graduates, yet one flag of three removes that base's combinations", () => {
  const c = corpus();
  for (const b of ["C", "D", "E", "F", "G"]) c.cases.push({ id: `c${b}`, kind: "control", base: b, shape: "single" });
  const all = scoresFor(c, (k) => (k.id === "cB" ? flagged() : baseline(k)));
  const m3 = aggregate(c, all, "claude", "primary", c.cases);
  assert.equal(verdict(c, RUN, m3).status, "GRADUATED");
  assert.ok(!scope(m3).granted.some((x) => x.endsWith("@B")));
  const one = scoresFor(c, (k, a) => (k.id === "cB" && a === 1 ? flagged() : baseline(k)));
  const m = aggregate(c, one, "claude", "primary", c.cases);
  assert.equal(verdict(c, RUN, m).status, "GRADUATED");
  const s = scope(m);
  assert.ok(s.granted.includes("obj0@A"));
  assert.ok(!s.granted.some((x) => x.endsWith("@B")));
  assert.ok(s.diagnosisOnly.includes("obj0@B"));
});

test("scope needs two of three identified through a patch-eligible answer", () => {
  const c = corpus();
  const m = aggregate(c, scoresFor(c, (k, a) => (k.id === "o0" ? (a === 1 ? hit(true) : hit(false)) : baseline(k))), "claude", "primary", c.cases);
  const s = scope(m);
  assert.ok(!s.granted.includes("obj0@A"));
  assert.ok(s.diagnosisOnly.includes("obj0@A"));
});

test("the seed case also needs its clean three-seed control", () => {
  const c = corpus();
  c.cases.push({ id: "seed", kind: "defect", class: "seed", base: "A", stratum: "subjective", shape: "three" },
    { id: "seedc", kind: "control", base: "A", shape: "three" });
  const ok = scope(aggregate(c, scoresFor(c, baseline), "claude", "primary", c.cases));
  assert.ok(ok.granted.includes("seed@A"));
  const bad = scope(aggregate(c, scoresFor(c, (k, a) => (k.id === "seedc" && a === 2 ? flagged() : baseline(k))), "claude", "primary", c.cases));
  assert.ok(!bad.granted.includes("seed@A"));
  assert.ok(bad.granted.includes("obj0@A"));
});

test("the secondary condition never gates and lists its ineligible cases", () => {
  const c = corpus();
  const scores = [...scoresFor(c, baseline), ...scoresFor({ ...c, attempts: 1 }, () => failed(), "bundle-only")];
  const r = buildReport(c, RUN, scores);
  assert.equal(r.agents[0].primary.verdict.status, "GRADUATED");
  assert.equal(r.agents[0].bundleOnly.completion, 0);
  assert.deepEqual(r.secondaryIneligible, ["sub2", "sub3"]);
});

test("the human baseline is scored over authored and retained cases", () => {
  const c = corpus();
  const authored = [...c.cases, { id: "dropped", kind: "defect", class: "obj0", base: "A", stratum: "objective", shape: "single" }];
  const human = { authoredCases: authored, scores: authored.map((k) => ({ caseId: k.id, score: k.id === "dropped" ? miss(1) : baseline(k) })) };
  const r = buildReport(c, RUN, scoresFor(c, baseline), human);
  assert.ok(r.humanBaseline.authored.objective < 100);
  assert.equal(r.humanBaseline.retained.objective, 100);
});

test("the public aggregate carries numbers and the verdict line, never case ids", () => {
  const c = corpus();
  const r = buildReport(c, RUN, scoresFor(c, baseline));
  const p = publicReport(r, c.cases.map((k) => k.id).filter((id) => id.length > 2));
  assert.match(p.agents[0].verdictLine, /GRADUATED on this frozen corpus/);
  assert.match(p.agents[0].verdictLine, /canary passed/);
  assert.match(p.agents[0].verdictLine, /frames/);
  assert.throws(() => publicReport(r, ["obj0"]), /leaks case id/);
  assert.match(markdown(r), /no claim about visual competence/);
});

test("emitter names that overlap the report vocabulary never reach the aggregate", () => {
  const c = corpus();
  const r = buildReport(c, RUN, scoresFor(c, baseline));
  const text = JSON.stringify(publicReport(r, []));
  assert.ok(!text.includes("units"));
  assert.ok(!text.includes("errors"));
});
