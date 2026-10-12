// The benchmark report (plan 3.8, 3.7 step 8). Pure: scores in, report out.
//
// Every graduation metric is per agent over that agent's primary-condition
// scheduled attempts (cases x attempts, controls x attempts); a missing score
// is a scheduled attempt that never completed. The bundle-only condition is
// reported beside it and never gates. Thresholds compare in integers, so a
// rate of exactly 80 % is 80 %.
//
// corpus: {
//   targets: { objective, subjective, controlFlag, fdp, completion }   percentages
//   attempts: scheduled attempts per case (3)
//   cases: [{ id, kind: "defect" | "control", class, base, stratum, shape: "single" | "three", secondaryEligible, decoy }]
//   requiredClasses: [class ids], reducedClassList: [uncovered class ids] | null, seedAbsence: reason | null
// }
// run: { agents: [{ id, model, imageCondition }], canary, probes: { name: status }, contentRoots, backend }
// scores: [{ agent, condition, caseId, attempt, score }]

const pct = (num, den) => (den === 0 ? null : (100 * num) / den);
const atLeast = (num, den, target) => den > 0 && 100 * num >= target * den;
const below = (num, den, target) => den > 0 && 100 * num < target * den;

function attemptsFor(scores, agent, condition, caseId, count) {
  const found = scores.filter((s) => s.agent === agent && s.condition === condition && s.caseId === caseId);
  return Array.from({ length: count }, (_, i) => found.find((s) => s.attempt === i + 1)?.score ?? null);
}

// The metrics of one agent under one condition over the given cases.
export function aggregate(corpus, scores, agent, condition, cases) {
  const n = condition === "primary" ? corpus.attempts : 1;
  const out = {
    objective: { identified: 0, scheduled: 0 }, subjective: { identified: 0, scheduled: 0 },
    controls: { flagged: 0, scheduled: 0 }, decoys: { flagged: 0, scheduled: 0 }, completion: { completed: 0, scheduled: 0 },
    tp: 0, redundant: 0, fp: 0, redundantEntries: 0, brier: { sum: 0, count: 0 },
    bySeverity: {}, classes: {}, bases: {}, exhausted: 0, unmapped: [],
  };
  for (const c of cases) {
    const attempts = attemptsFor(scores, agent, condition, c.id, n);
    for (const s of attempts) {
      out.completion.scheduled++;
      if (s?.completed) out.completion.completed++;
      if (!s) continue;
      out.tp += s.tp; out.redundant += s.redundant; out.fp += s.fp; out.redundantEntries += s.redundantEntries;
      for (const [conf, label] of s.calibration) { out.brier.sum += (conf - label) ** 2; out.brier.count++; }
      for (const [sev, counts] of Object.entries(s.bySeverity)) {
        const row = (out.bySeverity[sev] ??= { tp: 0, redundant: 0, fp: 0, redundantEntries: 0 });
        for (const k of Object.keys(row)) row[k] += counts[k];
      }
      out.unmapped.push(...s.unmapped);
      if (s.exhausted) out.exhausted++;
    }
    if (c.kind === "control") {
      const flagged = attempts.filter((s) => !s || s.flagged).length;
      out.controls.flagged += flagged;
      out.controls.scheduled += n;
      if (c.decoy) { out.decoys.flagged += flagged; out.decoys.scheduled += n; }
      const base = (out.bases[c.base] ??= { controlFlags: 0, controlAttempts: 0, seedControlFlags: 0, seedControlAttempts: 0 });
      if (c.shape === "three") { base.seedControlFlags += flagged; base.seedControlAttempts += n; }
      else { base.controlFlags += flagged; base.controlAttempts += n; }
      continue;
    }
    const identified = attempts.filter((s) => s?.identified).length;
    const eligible = attempts.filter((s) => s?.identified && s.patchEligible).length;
    const diagnosis = attempts.filter((s) => s?.diagnosisOnly).length;
    out[c.stratum].identified += identified;
    out[c.stratum].scheduled += n;
    const row = (out.classes[c.class] ??= { stratum: c.stratum, identified: 0, diagnosisOnly: 0, scheduled: 0, combos: [] });
    row.identified += identified; row.diagnosisOnly += diagnosis; row.scheduled += n;
    row.combos.push({ base: c.base, identified, eligible, attempts: n, seed: c.class === "seed" });
  }
  out.unmapped = [...new Set(out.unmapped)].sort();
  return out;
}

// A class-and-base combination enters scope when its case was identified
// through a patch-eligible answer in at least two of three attempts and that
// base's clean control (and, for the seed case, the three-seed control) was
// never flagged.
export function scope(metrics) {
  const granted = [], diagnosisOnly = [];
  for (const [cls, row] of Object.entries(metrics.classes)) {
    for (const combo of row.combos) {
      const base = metrics.bases[combo.base] ?? { controlFlags: 1, controlAttempts: 0, seedControlFlags: 0, seedControlAttempts: 0 };
      const clean = base.controlAttempts > 0 && base.controlFlags === 0 &&
        (!combo.seed || (base.seedControlAttempts > 0 && base.seedControlFlags === 0));
      const label = `${cls}@${combo.base}`;
      if (3 * combo.eligible >= 2 * combo.attempts && clean) granted.push(label);
      else if (3 * combo.identified >= 2 * combo.attempts) diagnosisOnly.push(label);
    }
  }
  return { granted: granted.sort(), diagnosisOnly: diagnosisOnly.sort() };
}

const FRESH_PROBES_PASSED = (run) => Object.entries(run.probes ?? {}).filter(([, status]) => status !== "passed").map(([name]) => name);

export function verdict(corpus, run, metrics) {
  const t = corpus.targets;
  const c = metrics.completion, o = metrics.objective, s = metrics.subjective, k = metrics.controls;
  const missingControls = corpus.cases.filter((x) => x.kind === "defect").map((x) => x.base)
    .filter((b, i, all) => all.indexOf(b) === i && !corpus.cases.some((x) => x.kind === "control" && x.base === b && x.shape === "single"));
  if (missingControls.length) return { status: "NO VERDICT", reasons: [`missing control stratum for ${missingControls.join(", ")}`] };
  if (!atLeast(c.completed, c.scheduled, t.completion)) return { status: "INVALID", reasons: [`completion ${pct(c.completed, c.scheduled)?.toFixed(1)} % below ${t.completion} %`] };
  const reasons = [];
  if (corpus.reducedClassList?.length) reasons.push(`reduced class list: ${corpus.reducedClassList.join(", ")} not covered`);
  if (run.canary !== "passed") reasons.push(`confinement canary ${run.canary ?? "missing"}`);
  for (const probe of FRESH_PROBES_PASSED(run)) reasons.push(`probe ${probe} ${run.probes[probe]}`);
  const fdpDen = metrics.tp + metrics.fp;
  const passes = {
    objective: atLeast(o.identified, o.scheduled, t.objective),
    subjective: s.scheduled === 0 || atLeast(s.identified, s.scheduled, t.subjective),
    controlFlag: below(k.flagged, k.scheduled, t.controlFlag),
    fdp: fdpDen > 0 && 100 * metrics.fp <= t.fdp * fdpDen,
  };
  if (reasons.length) return { status: "INCOMPLETE", reasons, passes };
  const failed = Object.entries(passes).filter(([, ok]) => !ok).map(([name]) => name);
  return failed.length ? { status: "NOT GRADUATED", reasons: failed.map((f) => `${f} limit not met`), passes } : { status: "GRADUATED", reasons: [], passes };
}

function rates(metrics) {
  const fdpDen = metrics.tp + metrics.fp;
  return {
    objective: pct(metrics.objective.identified, metrics.objective.scheduled),
    subjective: pct(metrics.subjective.identified, metrics.subjective.scheduled),
    controlFlag: pct(metrics.controls.flagged, metrics.controls.scheduled),
    decoyFlag: pct(metrics.decoys.flagged, metrics.decoys.scheduled),
    fdp: fdpDen === 0 ? null : pct(metrics.fp, fdpDen),
    completion: pct(metrics.completion.completed, metrics.completion.scheduled),
    brier: metrics.brier.count ? metrics.brier.sum / metrics.brier.count : null,
    classes: Object.fromEntries(Object.entries(metrics.classes).sort(([a], [b]) => (a < b ? -1 : 1)).map(([id, r]) => [id, {
      stratum: r.stratum, identified: pct(r.identified, r.scheduled), diagnosisOnly: pct(r.diagnosisOnly, r.scheduled), scheduled: r.scheduled,
    }])),
    bases: Object.fromEntries(Object.entries(metrics.bases).sort(([a], [b]) => (a < b ? -1 : 1))),
    counts: { tp: metrics.tp, redundant: metrics.redundant, fp: metrics.fp, redundantEntries: metrics.redundantEntries },
    bySeverity: metrics.bySeverity,
    exhausted: metrics.exhausted,
    unmapped: metrics.unmapped,
  };
}

// The full (private) report: per agent, primary verdict and scope, bundle-only beside it.
export function buildReport(corpus, run, scores, human = null) {
  const primaryCases = corpus.cases;
  const secondaryCases = corpus.cases.filter((c) => c.kind === "control" || (c.stratum === "subjective" && c.secondaryEligible));
  const agents = run.agents.map((agent) => {
    const primary = aggregate(corpus, scores, agent.id, "primary", primaryCases);
    const v = verdict(corpus, run, primary);
    const secondary = aggregate(corpus, scores, agent.id, "bundle-only", secondaryCases);
    return {
      id: agent.id, model: agent.model, imageCondition: agent.imageCondition,
      primary: { ...rates(primary), verdict: v, scope: v.status === "GRADUATED" ? scope(primary) : { granted: [], diagnosisOnly: scope(primary).diagnosisOnly } },
      bundleOnly: rates(secondary),
    };
  });
  const secondaryIneligible = corpus.cases.filter((c) => c.kind === "defect" && c.stratum === "subjective" && !c.secondaryEligible).map((c) => c.class).sort();
  let baseline = null;
  if (human) {
    const humanCorpus = { ...corpus, attempts: 1 };
    const all = aggregate(humanCorpus, human.scores.map((s) => ({ ...s, agent: "owner", condition: "primary", attempt: 1 })), "owner", "primary", human.authoredCases);
    const kept = aggregate(humanCorpus, human.scores.map((s) => ({ ...s, agent: "owner", condition: "primary", attempt: 1 })), "owner", "primary", corpus.cases);
    baseline = { authored: rates(all), retained: rates(kept) };
  }
  return {
    schemaVersion: 1,
    profile: "this frozen corpus under the fixed observation profile",
    caseCounts: {
      defect: corpus.cases.filter((c) => c.kind === "defect").length,
      control: corpus.cases.filter((c) => c.kind === "control").length,
    },
    seedCase: corpus.cases.some((c) => c.class === "seed") ? "present" : `absent (${corpus.seedAbsence ?? "no reason recorded"})`,
    contentRoots: run.contentRoots, backend: run.backend, canary: run.canary ?? "missing",
    targets: corpus.targets, secondaryIneligible, agents, humanBaseline: baseline,
  };
}

const fmt = (v) => (v === null || v === undefined ? "undefined" : `${v.toFixed(1)} %`);

export function verdictLine(report, agent) {
  const p = agent.primary;
  return `${agent.id} (${agent.model}, ${agent.imageCondition}): ${p.verdict.status} on ${report.profile}; ` +
    `${report.caseCounts.defect} defect cases and ${report.caseCounts.control} controls; completion ${fmt(p.completion)}; ` +
    `seed case ${report.seedCase}; content roots ${report.contentRoots}; sandbox backend ${report.backend}; canary ${report.canary}; ` +
    `classes passed: ${Object.entries(p.classes).filter(([, r]) => r.identified !== null && r.identified >= (r.stratum === "objective" ? report.targets.objective : report.targets.subjective)).map(([id]) => id).join(", ") || "none"}; ` +
    `supervised-trial scope: ${p.scope.granted.join(", ") || "none"}` + (p.verdict.reasons.length ? ` (${p.verdict.reasons.join("; ")})` : "");
}

// The aggregate copied into the repository: a strict allowlist of the public
// taxonomy, numbers and the verdict line; never emitter names or case-derived
// prose. Refuses if any case id or frame-citation shape slipped through.
export function publicReport(report, caseIds) {
  const pick = (r) => ({
    objective: r.objective, subjective: r.subjective, controlFlag: r.controlFlag, decoyFlag: r.decoyFlag, fdp: r.fdp, completion: r.completion,
    brier: r.brier, counts: r.counts, bySeverity: r.bySeverity, exhausted: r.exhausted,
    classes: Object.fromEntries(Object.entries(r.classes).map(([id, c]) => [id, { stratum: c.stratum, identified: c.identified, diagnosisOnly: c.diagnosisOnly, scheduled: c.scheduled }])),
  });
  const out = {
    schemaVersion: 1, caseCounts: report.caseCounts, seedCase: report.seedCase, contentRoots: report.contentRoots,
    backend: report.backend, canary: report.canary, targets: report.targets, secondaryIneligible: report.secondaryIneligible,
    agents: report.agents.map((a) => ({
      id: a.id, model: a.model, imageCondition: a.imageCondition, verdict: a.primary.verdict.status,
      verdictLine: verdictLine(report, a), scope: a.primary.scope.granted,
      primary: pick(a.primary), bundleOnly: pick(a.bundleOnly),
    })),
  };
  const text = JSON.stringify(out);
  for (const id of caseIds) if (text.includes(`"${id}"`) || text.includes(` ${id} `)) throw new Error(`public report leaks case id ${id}`);
  if (/"frame"\s*:/.test(text)) throw new Error("public report carries a frame citation");
  return out;
}

export function markdown(report) {
  const lines = ["# Defect benchmark report", ""];
  for (const a of report.agents) {
    lines.push(`## ${a.id}`, "", verdictLine(report, a), "", "| Class | Stratum | Identified | Diagnosis only |", "|---|---|---|---|");
    for (const [id, c] of Object.entries(a.primary.classes)) lines.push(`| ${id} | ${c.stratum} | ${fmt(c.identified)} | ${fmt(c.diagnosisOnly)} |`);
    lines.push("", `Objective ${fmt(a.primary.objective)}, subjective ${fmt(a.primary.subjective)}, control flags ${fmt(a.primary.controlFlag)}, FDP ${fmt(a.primary.fdp)}, completion ${fmt(a.primary.completion)}, Brier ${a.primary.brier === null ? "undefined" : a.primary.brier.toFixed(3)}.`, "");
    if (a.primary.decoyFlag !== null) lines.push(`Decoy controls (a reorder that cannot show; counted in the control flags): flags ${fmt(a.primary.decoyFlag)}.`, "");
    lines.push(`Diagnosis without source inspection (bundle-only, does not gate): subjective ${fmt(a.bundleOnly.subjective)}, control flags ${fmt(a.bundleOnly.controlFlag)}, completion ${fmt(a.bundleOnly.completion)}.`, "");
  }
  if (report.secondaryIneligible.length) lines.push(`Secondary-ineligible: ${report.secondaryIneligible.join(", ")}.`, "");
  lines.push("Version 1 makes no claim about visual competence: the bundle-only condition still carries per-emitter telemetry.", "");
  return lines.join("\n");
}
