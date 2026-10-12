// The benchmark scorer (plan 3.8). Pure: an attempt's findings, the case's
// frozen answers and the pack context in, one score out.
//
// Order of evaluation:
//   1. every finding is normalised and validated on its own (semantic.mjs);
//   2. exact duplicates collapse into one unit: the same diagnosis, the same
//      projected correction and the same de-duplicated citations, severity
//      and confidence excluded; the representative is the highest confidence,
//      then the highest severity, then canonical order;
//   3. units and answers form a bipartite graph with an edge wherever a unit
//      fully matches an answer (clauses 1-4), and a maximum-cardinality
//      matching is taken with units in canonical order and answers in id
//      order, so the output order of the findings never matters;
//   4. the matched unit on the recipe's answer (id 0), else the first matched
//      unit, is the true positive; every other matched unit is redundant;
//      every unmatched unit is a false positive.
import { acceptScalar } from "./persisted.mjs";
import { stepTrackEquivalent } from "./track.mjs";
import { canonical, citesPath, correctionPath, inspectPath, normaliseFinding, sameRef } from "./semantic.mjs";
import { canonicalFinding, mergeAnswers, variantErrors } from "./answers.mjs";

const SEVERITY_RANK = { low: 0, medium: 1, high: 2 };

const sameSet = (a, b) => a.length === b.length && a.every((x, i) => sameRef(x, b[i]));

// Clauses 1 and 2: category, direction, emitters, relation, bundle.
function diagnosisMatches(normal, answer) {
  if (normal.category !== answer.category || normal.direction !== answer.direction) return false;
  if (normal.bundleId !== answer.bundleId) return false;
  if (!sameSet(normal.emitters, answer.emitters)) return false;
  if (answer.relation) {
    if (!normal.relation || !sameRef(normal.relation.front, answer.relation.front) || !sameRef(normal.relation.back, answer.relation.back)) return false;
  }
  return true;
}

// Clause 3 under the frames policy: every frame tuple exists in the finding's
// own bundle, and a strict majority lies inside its kind's validated window.
function framesPass(normal, answer, context) {
  const tuples = normal.citations.frames;
  if (tuples.length === 0) return false;
  let inside = 0;
  for (const t of tuples) {
    if (t.bundleId !== normal.bundleId) return false;
    const bundle = context.bundles[t.bundleId];
    if (!bundle) return false;
    if (t.kind === "image" ? !bundle.imageFrames.includes(t.frame) : !(t.frame >= (bundle.firstFrame ?? 0) && t.frame <= bundle.lastFrame)) return false;
    const window = answer.windows[t.bundleId]?.[t.kind] ?? [];
    if (window.includes(t.frame)) inside++;
  }
  return 2 * inside > tuples.length;
}

// Clause 3 under the inspect policy: a valid inspect citation names the
// answer's correction emitter and field.
function inspectPass(normal, answer, context) {
  if (!context.inspectInPack || answer.correction.kind === "reorder") return false;
  const path = correctionPath(answer.correction);
  return normal.citations.inspect.some((c) => {
    if (!sameRef(c.emitter, answer.correction.emitter)) return false;
    const emitter = context.emitters.find((e) => e.index === c.emitter.index && e.name === c.emitter.name);
    return emitter && inspectPath(emitter, c.field) && citesPath(c.field, path);
  });
}

function evidenceMatches(normal, answer, context) {
  const policy = answer.policy[context.condition];
  if (policy === "frames") return framesPass(normal, answer, context);
  if (policy === "inspect") return inspectPass(normal, answer, context);
  if (policy === "either") return framesPass(normal, answer, context) || inspectPass(normal, answer, context);
  return false;
}

// Clause 4: well-typed, in domain, bounded, effective and on target.
function correctionMatches(normal, answer, context) {
  const p = normal.correction, a = answer.correction;
  if (p.kind !== a.kind) return false;
  if (a.kind === "reorder") return sameRef(p.front, answer.relation.front) && sameRef(p.back, answer.relation.back);
  if (!sameRef(p.emitter, a.emitter)) return false;
  switch (a.kind) {
    case "scalar": return p.field === a.field && acceptScalar(a.accept, p.value).ok;
    case "enum": return p.field === a.field && a.acceptable.includes(p.value);
    case "texture": return p.field === a.field && context.textures.includes(p.name);
    case "track": return p.track === a.track && stepTrackEquivalent(a.baseKeys, p.keys);
  }
  return false;
}

// Kuhn's augmenting paths, units in canonical order, answers in id order.
function maximumMatching(edges, unitCount, answerCount) {
  const answerOwner = new Array(answerCount).fill(-1);
  const tryUnit = (u, seen) => {
    for (const a of edges[u]) {
      if (seen[a]) continue;
      seen[a] = true;
      if (answerOwner[a] === -1 || tryUnit(answerOwner[a], seen)) { answerOwner[a] = u; return true; }
    }
    return false;
  };
  for (let u = 0; u < unitCount; u++) tryUnit(u, new Array(answerCount).fill(false));
  const unitAnswer = new Array(unitCount).fill(-1);
  answerOwner.forEach((u, a) => { if (u !== -1) unitAnswer[u] = a; });
  return unitAnswer;
}

const emptyCounts = () => ({ tp: 0, redundant: 0, fp: 0, redundantEntries: 0 });

// caseInfo: { kind: "defect" | "control" }; answers: the case's frozen answer
// variants (merged here too, so a duplicate planted past lock changes nothing);
// findings: the schema-valid findings array, or null for a terminal failure;
// context: { condition, bundles, emitters, inspectInPack, textures }.
export function scoreAttempt({ caseInfo, answers, findings, context, rubric }) {
  const merged = mergeAnswers(answers ?? [], { strict: false });
  const usable = merged.filter((a) => a.policy[context.condition]);
  const completed = Array.isArray(findings);
  const units = new Map();
  for (const finding of findings ?? []) {
    const { errors, normal } = normaliseFinding(finding, context, rubric);
    const key = canonical(normal);
    const entry = { severity: finding.severity, confidence: finding.confidence, errors };
    if (!units.has(key)) units.set(key, { key, normal, entries: [] });
    units.get(key).entries.push(entry);
  }
  const ordered = [...units.values()].sort((a, b) => (a.key < b.key ? -1 : a.key > b.key ? 1 : 0));
  for (const unit of ordered) {
    unit.entries.sort((x, y) => y.confidence - x.confidence || SEVERITY_RANK[y.severity] - SEVERITY_RANK[x.severity]);
    unit.representative = unit.entries[0];
    unit.full = []; unit.diagnosis = false;
    if (!unit.normal.valid) continue;
    usable.forEach((answer, a) => {
      if (!diagnosisMatches(unit.normal, answer) || !evidenceMatches(unit.normal, answer, context)) return;
      unit.diagnosis = true;
      if (correctionMatches(unit.normal, answer, context)) unit.full.push(a);
    });
  }
  const unitAnswer = maximumMatching(ordered.map((u) => u.full), ordered.length, usable.length);
  const recipe = usable.findIndex((a) => a.id === 0);
  let tp = unitAnswer.findIndex((a) => a !== -1 && a === recipe);
  if (tp === -1) tp = unitAnswer.findIndex((a) => a !== -1);

  const counts = emptyCounts();
  const bySeverity = Object.fromEntries(rubric.severities.map((s) => [s, emptyCounts()]));
  const calibration = [];
  const rows = ordered.map((unit, u) => {
    const label = u === tp ? "tp" : unitAnswer[u] !== -1 ? "redundant" : "fp";
    counts[label]++;
    bySeverity[unit.representative.severity][label]++;
    for (const extra of unit.entries.slice(1)) { counts.redundantEntries++; bySeverity[extra.severity].redundantEntries++; }
    calibration.push([unit.representative.confidence, label === "fp" ? 0 : 1]);
    return {
      label, answer: unitAnswer[u] === -1 ? null : usable[unitAnswer[u]].id,
      severity: unit.representative.severity, confidence: unit.representative.confidence,
      entries: unit.entries.length, category: unit.normal.category, direction: unit.normal.direction,
      errors: unit.representative.errors,
    };
  });
  const matched = unitAnswer.filter((a) => a !== -1).map((a) => usable[a]);
  const brier = calibration.length ? calibration.reduce((s, [c, l]) => s + (c - l) ** 2, 0) / calibration.length : null;
  const control = caseInfo.kind === "control";
  return {
    condition: context.condition,
    completed,
    identified: !control && completed && matched.length > 0,
    diagnosisOnly: !control && completed && ordered.some((u) => u.diagnosis),
    flagged: control ? !completed || ordered.length > 0 : null,
    patchEligible: matched.some((a) => a.patchEligible),
    ...counts,
    bySeverity,
    calibration,
    brier,
    units: rows,
    unmapped: rows.filter((r) => r.errors.some((e) => e.startsWith("unmapped category"))).map((r) => `${r.category}/${r.direction}`),
  };
}

// The lock-time answer check (plan 3.8): every answer's canonical finding
// scores as identified in every condition it carries a policy for, and a
// control's empty findings score as complete, unflagged and FP-free.
// contexts: { primary: context, "bundle-only": context | absent }.
export function lockAnswerCheck({ caseInfo, variants, contexts, rubric }) {
  const errors = [];
  if (caseInfo.kind === "control") {
    for (const [condition, context] of Object.entries(contexts)) {
      const s = scoreAttempt({ caseInfo, answers: [], findings: [], context, rubric });
      if (!s.completed || s.flagged || s.fp !== 0) errors.push(`${condition}: an empty answer does not score clean`);
    }
    return errors;
  }
  let answers;
  try {
    for (const v of variants) for (const e of variantErrors(v)) errors.push(`variant ${v.id}: ${e}`);
    answers = mergeAnswers(variants, { strict: true });
  } catch (e) {
    return [...errors, e.message];
  }
  if (!answers.some((a) => a.id === 0)) errors.push("no recipe answer (id 0)");
  for (const [condition, context] of Object.entries(contexts)) {
    let passing = 0;
    for (const answer of answers) {
      if (!answer.policy[condition]) continue;
      const finding = canonicalFinding(answer, condition, context.textures);
      const s = scoreAttempt({ caseInfo, answers, findings: [finding], context, rubric });
      if (s.identified && s.tp === 1) passing++;
      else errors.push(`${condition}: answer ${answer.id}'s canonical finding is not identified (${s.units[0]?.errors.join("; ") || "no full match"})`);
    }
    if (condition === "primary" && passing === 0) errors.push("primary: no passing answer");
  }
  return errors;
}
