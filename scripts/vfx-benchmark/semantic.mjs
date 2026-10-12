// Per-finding semantic validation (plan 3.4, findings.schema.json row).
// Runs after schema-check.mjs on a structurally valid document, finding by
// finding: a semantic failure (an unknown emitter or category, a correction
// out of its domain, a track the editor would not persist as written) costs
// that finding, which scores as an unmatched false positive; it never fails
// the attempt. The scorer and the lock-time answer check both use it.
//
// It also normalises a finding into the parts the scorer compares:
// rubric-aliased category and direction, the sorted emitter set, the relation
// (draw order only), the resolved bundle, de-duplicated citations, and the
// correction after its persisted-type projection.
import { inRawDomain, inRepresentable, project, SCALAR_FIELDS } from "./persisted.mjs";
import { projectTrack, trackErrors, atlasCells } from "./track.mjs";

export const TEXTURE_FIELDS = ["colorTexture", "normalTexture"];
export const ENUM_FIELDS = { blendMode: { min: 0, max: 13 } };
export const TRACK_COUNT = 7;
export const INDEX_TRACK = 5;

// One stable text for any JSON value: object keys sorted.
export function canonical(value) {
  if (Array.isArray(value)) return `[${value.map(canonical).join(",")}]`;
  if (value !== null && typeof value === "object") {
    return `{${Object.keys(value).sort().map((k) => `${JSON.stringify(k)}:${canonical(value[k])}`).join(",")}}`;
  }
  return JSON.stringify(Object.is(value, -0) ? 0 : value);
}

const ref = (e) => ({ index: e.index, name: e.name });
const byRef = (a, b) => a.index - b.index || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0);
export const sameRef = (a, b) => !!a && !!b && a.index === b.index && a.name === b.name;

export function resolveAlias(rubric, category, direction) {
  const cat = Object.hasOwn(rubric.categoryAliases, category) ? rubric.categoryAliases[category] : category;
  const dir = Object.hasOwn(rubric.directionAliases, direction) ? rubric.directionAliases[direction] : direction;
  return { category: cat, direction: dir };
}

// `field` as an inspect path under one emitter: name, name[i], name[i].sub ...
export function inspectPath(emitter, field) {
  let node = emitter;
  for (const part of field.split(".")) {
    const m = /^([A-Za-z][A-Za-z0-9]*)((?:\[\d+\])*)$/.exec(part);
    if (!m || node === null || typeof node !== "object" || !Object.hasOwn(node, m[1])) return false;
    node = node[m[1]];
    for (const index of m[2].match(/\d+/g) ?? []) {
      if (!Array.isArray(node) || Number(index) >= node.length) return false;
      node = node[Number(index)];
    }
  }
  return true;
}

// The path a correction edits, as an inspect citation would name it.
export function correctionPath(correction) {
  return correction.kind === "track" ? `tracks[${correction.track}]` : correction.field;
}

// A citation names the path when it is the path or a part inside it.
export function citesPath(field, path) {
  return field === path || field.startsWith(`${path}.`) || field.startsWith(`${path}[`);
}

// The inverse of a permutation map (map[from] = to).
export function invertMap(map) {
  const out = new Array(map.length);
  map.forEach((to, from) => { out[to] = from; });
  if (out.length !== map.length || [...out.keys()].some((i) => !Number.isInteger(out[i]))) throw new Error("not a permutation");
  return out;
}

// A finding written against an order-blind pack names emitters by the pack's
// numbering; `map[packIndex]` gives the case's. An index the map does not
// cover is kept, so it stays unknown.
export function renumberFinding(finding, map) {
  const to = (e) => (e && Number.isInteger(e.index) && e.index >= 0 && e.index < map.length ? { ...e, index: map[e.index] } : e);
  const c = finding.suggestedChange;
  return {
    ...finding,
    suspectedEmitters: finding.suspectedEmitters?.map(to),
    ...(finding.relation ? { relation: { ...finding.relation, front: to(finding.relation.front), back: to(finding.relation.back) } } : {}),
    evidence: { ...finding.evidence, citations: finding.evidence?.citations?.map((x) => (x.kind === "inspect" ? { ...x, emitter: to(x.emitter) } : x)) },
    suggestedChange: c?.kind === "reorder" ? { ...c, front: to(c.front), back: to(c.back) } : c ? { ...c, emitter: to(c.emitter) } : c,
  };
}

function emitterOf(context, e) {
  const found = context.emitters.find((x) => x.index === e.index);
  return found && found.name === e.name ? found : null;
}

function correctionOf(finding, context, errors) {
  const c = finding.suggestedChange;
  const target = c.kind === "reorder" ? null : emitterOf(context, c.emitter);
  if (c.kind !== "reorder" && !target) errors.push(`the correction names an unknown emitter ${c.emitter.index} ${JSON.stringify(c.emitter.name)}`);
  switch (c.kind) {
    case "scalar": {
      if (!Object.hasOwn(SCALAR_FIELDS, c.field)) { errors.push(`${c.field} is not a scalar field`); break; }
      if (!inRawDomain(c.field, c.value)) { errors.push(`${c.value} is outside the domain of ${c.field}`); break; }
      const value = project(c.field, c.value);
      if (!inRepresentable(c.field, value)) { errors.push(`${c.value} persists outside the domain of ${c.field}`); break; }
      return { kind: "scalar", emitter: ref(c.emitter), field: c.field, value };
    }
    case "enum": {
      const spec = Object.hasOwn(ENUM_FIELDS, c.field) ? ENUM_FIELDS[c.field] : null;
      if (!spec) { errors.push(`${c.field} is not an enum field`); break; }
      if (!Number.isInteger(c.value) || c.value < spec.min || c.value > spec.max) { errors.push(`${c.value} is not a ${c.field} value`); break; }
      return { kind: "enum", emitter: ref(c.emitter), field: c.field, value: c.value };
    }
    case "texture":
      if (!TEXTURE_FIELDS.includes(c.field)) { errors.push(`${c.field} is not a texture field`); break; }
      if (c.name === "") { errors.push("an empty texture name"); break; }
      return { kind: "texture", emitter: ref(c.emitter), field: c.field, name: c.name };
    case "track": {
      if (c.track >= TRACK_COUNT) { errors.push(`track ${c.track} does not exist`); break; }
      if (!target) break;
      const base = target.tracks?.[c.track];
      if (c.track !== INDEX_TRACK || !base) { errors.push(`track ${c.track} proposals are not scored`); break; }
      const cells = atlasCells(target.textureSize);
      const raw = trackErrors(c.keys, cells);
      if (raw.length) { errors.push(`invalid track: ${raw[0]}`); break; }
      const keys = projectTrack(c.keys);
      const saved = trackErrors(keys, cells);
      if (saved.length) { errors.push(`invalid track after save: ${saved[0]}`); break; }
      return { kind: "track", emitter: ref(c.emitter), track: c.track, keys };
    }
    case "reorder":
      if (!emitterOf(context, c.front) || !emitterOf(context, c.back)) { errors.push("the reorder names an unknown emitter"); break; }
      if (sameRef(c.front, c.back)) { errors.push("the reorder names one emitter twice"); break; }
      return { kind: "reorder", front: ref(c.front), back: ref(c.back) };
  }
  return null;
}

// -> { errors, normal }. `normal` is set even with errors (raw identity), so
// failed findings still de-duplicate among themselves.
export function normaliseFinding(finding, context, rubric) {
  if (context.toCase) finding = renumberFinding(finding, context.toCase);
  const errors = [];
  const { category, direction } = resolveAlias(rubric, finding.category, finding.direction);
  const known = Object.hasOwn(rubric.categories, category) && Object.hasOwn(rubric.categories[category].directions, direction);
  if (!known) errors.push(`unmapped category ${JSON.stringify(finding.category)} / ${JSON.stringify(finding.direction)}`);

  const bundleIds = Object.keys(context.bundles);
  let bundleId = finding.bundleId;
  if (bundleId === undefined && bundleIds.length === 1) bundleId = bundleIds[0];
  if (bundleId === undefined) errors.push("a three-bundle pack needs a bundleId");
  else if (!bundleIds.includes(bundleId)) errors.push(`unknown bundle ${JSON.stringify(bundleId)}`);

  const emitters = [];
  for (const e of finding.suspectedEmitters) {
    if (!emitterOf(context, e)) errors.push(`unknown emitter ${e.index} ${JSON.stringify(e.name)}`);
    if (!emitters.some((x) => sameRef(x, e))) emitters.push(ref(e));
  }
  emitters.sort(byRef);

  let relation = null;
  if (category === "draw-order") {
    if (!finding.relation) errors.push("draw-order needs a relation");
    else if (!emitterOf(context, finding.relation.front) || !emitterOf(context, finding.relation.back)) errors.push("the relation names an unknown emitter");
    else relation = { front: ref(finding.relation.front), back: ref(finding.relation.back) };
  }

  const frames = new Map(), inspects = new Map();
  for (const c of finding.evidence.citations) {
    if (c.kind === "inspect") {
      const item = { emitter: ref(c.emitter), field: c.field };
      inspects.set(canonical(item), item);
    } else {
      const item = { bundleId: c.bundleId ?? (bundleIds.length === 1 ? bundleIds[0] : null), kind: c.kind, frame: c.frame };
      frames.set(canonical(item), item);
    }
  }
  const sorted = (m) => [...m.entries()].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0)).map(([, v]) => v);

  const correction = correctionOf(finding, context, errors);
  // A failed finding keeps its raw correction, so it never joins a valid unit.
  const valid = errors.length === 0;
  const normal = {
    valid, bundleId: bundleId ?? null, category, direction, emitters, relation,
    citations: { frames: sorted(frames), inspect: sorted(inspects) },
    correction: valid ? correction : { invalid: canonical(finding.suggestedChange) },
  };
  return { errors, normal };
}
