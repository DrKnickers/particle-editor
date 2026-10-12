// The structural validator for findings.json (plan 3.4, findings.schema.json).
// Dependency-free: it implements exactly the shape findings.schema.json
// documents. Its verdict is one of
//   ok                 the envelope is well formed;
//   schema-error       a shape failure, the only repairable kind;
//   resource-overflow  a frozen size, count, aggregate or string limit was
//                      crossed; decided before the shape check, never repaired.
// Per-finding semantics (domains, track rules, emitter identities) live in
// semantic.mjs: a semantic failure costs that finding, never the attempt.
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
export const RUBRIC = JSON.parse(readFileSync(join(here, "rubric.json"), "utf8"));
export const LIMITS = RUBRIC.limits;

const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const isInt = (v) => Number.isInteger(v) && v >= 0;

// Walks every string and every container to apply the aggregate limits.
function overflow(value) {
  let longest = 0;
  const visit = (v) => {
    if (typeof v === "string") longest = Math.max(longest, v.length);
    else if (Array.isArray(v)) v.forEach(visit);
    else if (isObject(v)) for (const [k, x] of Object.entries(v)) { longest = Math.max(longest, k.length); visit(x); }
  };
  visit(value);
  if (longest > LIMITS.stringLength) return `a string of ${longest} characters (limit ${LIMITS.stringLength})`;
  const findings = isObject(value) && Array.isArray(value.findings) ? value.findings : [];
  if (findings.length > LIMITS.findings) return `${findings.length} findings (limit ${LIMITS.findings})`;
  let keys = 0, citations = 0;
  for (const f of findings) {
    if (!isObject(f)) continue;
    if (isObject(f.suggestedChange) && Array.isArray(f.suggestedChange.keys)) keys += f.suggestedChange.keys.length;
    if (isObject(f.evidence) && Array.isArray(f.evidence.citations)) citations += f.evidence.citations.length;
  }
  if (keys > LIMITS.trackKeysTotal) return `${keys} track keys in total (limit ${LIMITS.trackKeysTotal})`;
  if (citations > LIMITS.citationsTotal) return `${citations} citations in total (limit ${LIMITS.citationsTotal})`;
  return null;
}

function exactKeys(obj, required, optional, where, errors) {
  for (const k of required) if (!Object.hasOwn(obj, k)) errors.push(`${where}: missing ${k}`);
  for (const k of Object.keys(obj)) if (!required.includes(k) && !optional.includes(k)) errors.push(`${where}: unknown key ${k}`);
}

function emitterRef(v, where, errors) {
  if (!isObject(v)) { errors.push(`${where}: must be {index, name}`); return; }
  exactKeys(v, ["index", "name"], [], where, errors);
  if (!isInt(v.index)) errors.push(`${where}.index: must be a non-negative integer`);
  if (typeof v.name !== "string") errors.push(`${where}.name: must be a string`);
}

function citation(c, where, errors) {
  if (!isObject(c)) { errors.push(`${where}: must be an object`); return; }
  if (c.kind === "image" || c.kind === "telemetry") {
    exactKeys(c, ["kind", "frame"], ["bundleId"], where, errors);
    if (!isInt(c.frame)) errors.push(`${where}.frame: must be a non-negative integer`);
    if (Object.hasOwn(c, "bundleId") && typeof c.bundleId !== "string") errors.push(`${where}.bundleId: must be a string`);
  } else if (c.kind === "inspect") {
    exactKeys(c, ["kind", "emitter", "field"], [], where, errors);
    emitterRef(c.emitter, `${where}.emitter`, errors);
    if (typeof c.field !== "string") errors.push(`${where}.field: must be a string`);
  } else {
    errors.push(`${where}.kind: must be image, telemetry or inspect`);
  }
}

function change(c, where, errors) {
  if (!isObject(c)) { errors.push(`${where}: must be an object`); return; }
  switch (c.kind) {
    case "scalar":
    case "enum":
      exactKeys(c, ["kind", "emitter", "field", "value"], [], where, errors);
      emitterRef(c.emitter, `${where}.emitter`, errors);
      if (typeof c.field !== "string") errors.push(`${where}.field: must be a string`);
      if (typeof c.value !== "number") errors.push(`${where}.value: must be a number`);
      break;
    case "texture":
      exactKeys(c, ["kind", "emitter", "field", "name"], [], where, errors);
      emitterRef(c.emitter, `${where}.emitter`, errors);
      if (typeof c.field !== "string") errors.push(`${where}.field: must be a string`);
      if (typeof c.name !== "string") errors.push(`${where}.name: must be a string`);
      break;
    case "track":
      exactKeys(c, ["kind", "emitter", "track", "keys"], [], where, errors);
      emitterRef(c.emitter, `${where}.emitter`, errors);
      if (!isInt(c.track)) errors.push(`${where}.track: must be a non-negative integer`);
      if (!Array.isArray(c.keys) || c.keys.length < 2 || c.keys.length > LIMITS.trackKeysPerFinding) {
        errors.push(`${where}.keys: must hold 2 to ${LIMITS.trackKeysPerFinding} keys`);
      } else if (!c.keys.every((k) => Array.isArray(k) && k.length === 2 && k.every((x) => typeof x === "number" && Number.isFinite(x)))) {
        errors.push(`${where}.keys: every key must be [time, value] with finite numbers`);
      }
      break;
    case "reorder":
      exactKeys(c, ["kind", "front", "back"], [], where, errors);
      emitterRef(c.front, `${where}.front`, errors);
      emitterRef(c.back, `${where}.back`, errors);
      break;
    default:
      errors.push(`${where}.kind: must be scalar, enum, texture, track or reorder`);
  }
}

function finding(f, where, errors) {
  if (!isObject(f)) { errors.push(`${where}: must be an object`); return; }
  exactKeys(f, ["severity", "category", "direction", "claim", "suspectedEmitters", "evidence", "suggestedChange", "confidence"],
    ["id", "bundleId", "relation"], where, errors);
  if (Object.hasOwn(f, "id") && typeof f.id !== "string") errors.push(`${where}.id: must be a string`);
  if (!RUBRIC.severities.includes(f.severity)) errors.push(`${where}.severity: must be one of ${RUBRIC.severities.join(", ")}`);
  for (const k of ["category", "direction", "claim"]) if (typeof f[k] !== "string") errors.push(`${where}.${k}: must be a string`);
  if (Object.hasOwn(f, "bundleId") && typeof f.bundleId !== "string") errors.push(`${where}.bundleId: must be a string`);
  if (!Array.isArray(f.suspectedEmitters)) errors.push(`${where}.suspectedEmitters: must be an array`);
  else f.suspectedEmitters.forEach((e, i) => emitterRef(e, `${where}.suspectedEmitters[${i}]`, errors));
  if (Object.hasOwn(f, "relation")) {
    if (!isObject(f.relation)) errors.push(`${where}.relation: must be {front, back}`);
    else {
      exactKeys(f.relation, ["front", "back"], [], `${where}.relation`, errors);
      emitterRef(f.relation.front, `${where}.relation.front`, errors);
      emitterRef(f.relation.back, `${where}.relation.back`, errors);
    }
  }
  if (!isObject(f.evidence)) errors.push(`${where}.evidence: must be {citations}`);
  else {
    exactKeys(f.evidence, ["citations"], [], `${where}.evidence`, errors);
    if (!Array.isArray(f.evidence.citations)) errors.push(`${where}.evidence.citations: must be an array`);
    else f.evidence.citations.forEach((c, i) => citation(c, `${where}.evidence.citations[${i}]`, errors));
  }
  change(f.suggestedChange, `${where}.suggestedChange`, errors);
  if (typeof f.confidence !== "number" || !(f.confidence >= 0 && f.confidence <= 1)) errors.push(`${where}.confidence: must be a number in [0, 1]`);
}

// `input` is the file's bytes (Buffer or Uint8Array) or its text.
export function checkFindings(input) {
  const bytes = typeof input === "string" ? Buffer.byteLength(input, "utf8") : input.length;
  if (bytes > LIMITS.fileBytes) return { verdict: "resource-overflow", errors: [`${bytes} bytes (limit ${LIMITS.fileBytes})`] };
  let text = typeof input === "string" ? input : Buffer.from(input).toString("utf8");
  if (text.charCodeAt(0) === 0xfeff) return { verdict: "schema-error", errors: ["the file starts with a byte-order mark"] };
  let value;
  try { value = JSON.parse(text); } catch (e) { return { verdict: "schema-error", errors: [`not JSON: ${e.message}`] }; }
  const over = overflow(value);
  if (over) return { verdict: "resource-overflow", errors: [over] };
  const errors = [];
  if (!isObject(value)) errors.push("the document must be an object");
  else {
    exactKeys(value, ["findings"], [], "document", errors);
    if (!Array.isArray(value.findings)) errors.push("findings: must be an array");
    else value.findings.forEach((f, i) => finding(f, `findings[${i}]`, errors));
  }
  return errors.length ? { verdict: "schema-error", errors } : { verdict: "ok", errors: [], value };
}
