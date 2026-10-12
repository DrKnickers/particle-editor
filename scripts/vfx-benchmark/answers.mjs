// Answer variants (plan 3.8): complete contracts, merged into answers.
//
// A variant is { id, bundleId, category, direction, emitters, relation?,
// windows, policy, correction, patchEligible }:
//   id          a non-negative integer frozen at lock; 0 is the recipe's;
//   windows     { <bundleId>: { image: [frames], telemetry: [frames] } };
//   policy      { primary, "bundle-only" }: frames | inspect | either | null;
//   correction  { kind: "scalar", emitter, field, accept }   accept from persisted.mjs
//             | { kind: "enum", emitter, field, acceptable }
//             | { kind: "texture", emitter, field }
//             | { kind: "track", emitter, track, baseKeys, cells }
//             | { kind: "reorder" }                           uses relation.
//
// The matching slot is the diagnosis-and-correction contract, never the claim
// text, the id or the windows: variants that differ only by window merge into
// one answer whose window is the union, an exact duplicate is refused at lock
// (and collapsed by the scorer if one was planted past lock), and one contract
// under two evidence policies is refused in either order.
import { canonical } from "./semantic.mjs";

export const CONDITIONS = ["primary", "bundle-only"];
const POLICIES = ["frames", "inspect", "either"];

export function contractKey(v) {
  const { id, windows, policy, patchEligible, claim, ...contract } = v;
  return canonical(contract);
}

const union = (a, b) => [...new Set([...(a ?? []), ...(b ?? [])])].sort((x, y) => x - y);

function mergeWindows(a, b) {
  const out = {};
  for (const bundle of new Set([...Object.keys(a), ...Object.keys(b)])) {
    out[bundle] = {
      image: union(a[bundle]?.image, b[bundle]?.image),
      telemetry: union(a[bundle]?.telemetry, b[bundle]?.telemetry),
    };
  }
  return out;
}

// strict (lock): an exact duplicate is refused. Either way one contract under
// two policies is refused, and window-only variants merge.
export function mergeAnswers(variants, { strict }) {
  const groups = new Map();
  for (const v of [...variants].sort((a, b) => a.id - b.id)) {
    const key = contractKey(v);
    const existing = groups.get(key);
    if (!existing) { groups.set(key, { ...v, windows: mergeWindows(v.windows, {}) }); continue; }
    if (canonical(existing.policy) !== canonical(v.policy)) {
      throw new Error(`variants ${existing.id} and ${v.id} share a contract under different evidence policies`);
    }
    if (strict && canonical(mergeWindows(existing.windows, {})) === canonical(mergeWindows(v.windows, {}))) {
      throw new Error(`variant ${v.id} duplicates variant ${existing.id}`);
    }
    existing.windows = mergeWindows(existing.windows, v.windows);
  }
  return [...groups.values()].sort((a, b) => a.id - b.id);
}

const nonEmpty = (w) => Object.values(w).some((x) => x.image.length + x.telemetry.length > 0);

// The structural rules lock applies to every variant before scoring it.
export function variantErrors(v, { inspectDiffEmpty = false } = {}) {
  const errors = [];
  if (!Number.isInteger(v.id) || v.id < 0) errors.push("id must be a non-negative integer");
  for (const condition of CONDITIONS) {
    const p = v.policy?.[condition] ?? null;
    if (p !== null && !POLICIES.includes(p)) errors.push(`${condition} policy ${p} is not frames, inspect or either`);
  }
  if (!v.policy?.primary) errors.push("every variant has a primary policy");
  const windows = mergeWindows(v.windows ?? {}, {});
  const any = nonEmpty(windows);
  const primary = v.policy?.primary;
  if ((primary === "frames" || primary === "either") && !any) errors.push(`a ${primary} policy needs a non-empty window`);
  if (primary === "inspect" && any) errors.push("an inspect policy needs empty windows");
  if (primary === "either" && inspectDiffEmpty) errors.push("an either policy needs a non-empty inspect diff");
  const secondary = v.policy?.["bundle-only"] ?? null;
  if (secondary !== null && secondary !== "frames") errors.push("the bundle-only policy can only be frames");
  if (secondary === "frames" && !any) errors.push("a bundle-only frames policy needs a non-empty window");
  if ((primary === "inspect" || (primary === "either" && !any)) && secondary !== null) errors.push("an inspect-only variant has no bundle-only policy");
  if (v.category === "draw-order" && !v.relation) errors.push("draw-order needs a relation");
  if (v.correction?.kind !== "reorder" && v.correction?.emitter === undefined) errors.push("the correction names its target emitter");
  return errors;
}

// The canonical finding of an answer: what a fully correct agent would write.
export function canonicalFinding(answer, condition, textures) {
  const policy = answer.policy[condition];
  const citations = [];
  const window = answer.windows[answer.bundleId] ?? { image: [], telemetry: [] };
  if (policy === "frames" || (policy === "either" && (window.image.length || window.telemetry.length))) {
    if (window.image.length) citations.push({ kind: "image", bundleId: answer.bundleId, frame: window.image[0] });
    else citations.push({ kind: "telemetry", bundleId: answer.bundleId, frame: window.telemetry[0] });
  } else {
    const c = answer.correction;
    citations.push({ kind: "inspect", emitter: c.emitter, field: c.kind === "track" ? `tracks[${c.track}]` : c.field });
  }
  const c = answer.correction;
  let suggestedChange;
  switch (c.kind) {
    case "scalar":
      suggestedChange = { kind: "scalar", emitter: c.emitter, field: c.field,
        value: c.accept.direction === "decrease" ? c.accept.interval[0] : c.accept.interval[1] };
      break;
    case "enum": suggestedChange = { kind: "enum", emitter: c.emitter, field: c.field, value: c.acceptable[0] }; break;
    case "texture": suggestedChange = { kind: "texture", emitter: c.emitter, field: c.field, name: textures[0] }; break;
    case "track": suggestedChange = { kind: "track", emitter: c.emitter, track: c.track, keys: c.baseKeys }; break;
    case "reorder": suggestedChange = { kind: "reorder", front: answer.relation.front, back: answer.relation.back }; break;
  }
  return {
    severity: "medium", category: answer.category, direction: answer.direction, claim: "canonical answer",
    bundleId: answer.bundleId, suspectedEmitters: answer.emitters,
    ...(answer.relation ? { relation: answer.relation } : {}),
    evidence: { citations }, suggestedChange, confidence: 1,
  };
}
