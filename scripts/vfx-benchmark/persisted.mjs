// The one projection and endpoint procedure for scalar corrections (benchmark
// plan decision 11, section 3.8 clause 4). Lock derives every accepted
// interval with it, the scorer tests every proposal with it, and rules.md
// prints its worked examples from it, so the three can never disagree.
//
// A field persists as float32 (the writer saves the raw float bits) or as an
// unsigned 32-bit count. Endpoints are derived in double from persisted
// values, sorted, projected (float32: round to nearest; counts: the lower by
// ceiling, the upper by floor), then intersected with the field's
// representable domain. A proposal must be in its raw domain, project into
// the representable domain, lie between the projected endpoints inclusive,
// and lie strictly on the diagnosed side of the current persisted value.

export const F32_MAX = 3.4028234663852886e38;
export const F32_MIN_POSITIVE = 2 ** -149;
export const U32_MAX = 4294967295;

// The persisted type and semantic domain of every field a scalar correction
// may name. `positive` excludes zero; the others include their minimum.
export const SCALAR_FIELDS = Object.freeze({
  lifetime: { type: "float32", domain: "positive" },
  initialDelay: { type: "float32", domain: "non-negative" },
  randomLifetimePerc: { type: "float32", domain: "fraction" },
  randomScalePerc: { type: "float32", domain: "fraction" },
  nParticlesPerBurst: { type: "uint32", domain: "count" },
  nParticlesPerSecond: { type: "uint32", domain: "count" },
});

export function scalarField(field) {
  const spec = Object.hasOwn(SCALAR_FIELDS, field) ? SCALAR_FIELDS[field] : null;
  if (!spec) throw new Error(`no scalar field ${JSON.stringify(field)}`);
  return spec;
}

// The representable domain [min, max] of a field after projection.
export function representable(field) {
  const { type, domain } = scalarField(field);
  if (type === "uint32") return [1, U32_MAX];
  if (domain === "positive") return [F32_MIN_POSITIVE, F32_MAX];
  if (domain === "fraction") return [0, 1];
  return [0, F32_MAX];
}

const clean = (v) => (Object.is(v, -0) ? 0 : v);

// The value a save would persist. Only call on a value already in its raw domain.
export function project(field, value) {
  return clean(scalarField(field).type === "float32" ? Math.fround(value) : value);
}

// The raw proposal, before projection: finite, of the right kind, and inside
// the semantic domain (so -1e-50 on a fraction fails here although it would
// project to 0).
export function inRawDomain(field, value) {
  if (typeof value !== "number" || !Number.isFinite(value)) return false;
  const { type, domain } = scalarField(field);
  if (type === "uint32") return Number.isInteger(value) && value >= 1 && value <= U32_MAX;
  if (domain === "positive") return value > 0;
  if (domain === "fraction") return value >= 0 && value <= 1;
  return value >= 0;
}

export function inRepresentable(field, projected) {
  const [lo, hi] = representable(field);
  return Number.isFinite(projected) && projected >= lo && projected <= hi;
}

// Two double endpoints -> the projected, domain-clipped interval, or null when empty.
export function projectInterval(field, a, b) {
  let [lo, hi] = a <= b ? [a, b] : [b, a];
  if (scalarField(field).type === "uint32") {
    lo = Math.ceil(lo);
    hi = Math.floor(hi);
  } else {
    lo = Math.fround(lo);
    hi = Math.fround(hi);
  }
  const [dlo, dhi] = representable(field);
  lo = clean(Math.max(lo, dlo));
  hi = clean(Math.min(hi, dhi));
  return lo <= hi ? [lo, hi] : null;
}

// The next representable value toward the diagnosed side's far edge, used to
// decide feasibility: the interval must hold a value strictly past `current`.
export function nextFloat32(value, direction) {
  const f = new Float32Array(1);
  const u = new Uint32Array(f.buffer);
  f[0] = value;
  if (value === 0) {
    if (direction === "increase") return F32_MIN_POSITIVE;
    return -F32_MIN_POSITIVE;
  }
  const up = direction === "increase";
  if ((value > 0) === up) u[0] += 1;
  else u[0] -= 1;
  return clean(f[0]);
}

function strictlyPast(field, current, direction) {
  if (scalarField(field).type === "uint32") return direction === "increase" ? current + 1 : current - 1;
  return nextFloat32(Math.fround(current), direction);
}

// A frozen acceptance contract: { field, rule, direction, current, interval }.
// Returns the contract, or { error } when no representable movement exists.
function contract(field, rule, direction, current, a, b) {
  const interval = projectInterval(field, a, b);
  if (!interval) return { error: `${rule} interval for ${field} is empty after clipping to the domain` };
  const past = strictlyPast(field, current, direction);
  const feasible = direction === "decrease"
    ? interval[0] <= Math.min(interval[1], past)
    : interval[1] >= Math.max(interval[0], past);
  if (!feasible) return { error: `${rule} interval for ${field} holds no value ${direction === "decrease" ? "below" : "above"} the current ${current}` };
  return { field, rule, direction, current, interval };
}

function persistedValue(field, value, label) {
  if (!inRawDomain(field, value) || project(field, value) !== value) {
    throw new Error(`${label} ${value} is not a persisted ${field} value`);
  }
  return value;
}

// restore: the planted field, moved between a quarter of the way back and the base.
export function restoreContract(field, base, caseValue) {
  persistedValue(field, base, "base");
  persistedValue(field, caseValue, "case");
  if (base === caseValue) return { error: `restore needs a planted delta on ${field}` };
  const direction = base > caseValue ? "increase" : "decrease";
  return contract(field, "restore", direction, caseValue, caseValue + 0.25 * (base - caseValue), base);
}

// intervene: a field the recipe left unchanged, moved by 10-50 % of its stated
// target in the diagnosed direction (0.1-0.5 absolute for a zero target).
export function interveneContract(field, base, target, direction) {
  persistedValue(field, base, "base");
  if (target !== base) return { error: `stated target ${target} is not the persisted base ${base}` };
  const sign = direction === "increase" ? 1 : -1;
  const [near, far] = target === 0 ? [0.1, 0.5] : [0.1 * Math.abs(target), 0.5 * Math.abs(target)];
  return contract(field, "intervene", direction, base, base + sign * near, base + sign * far);
}

// The seed rule's per-field budget B (decision 11).
export function seedBudget(field, value) {
  const { domain } = scalarField(field);
  if (domain === "fraction") return 0.5;
  if (field === "lifetime") return 0.5 * value;
  return null;
}

// seed: the file is unchanged; move the frozen field by c in [0.25 B_eff, B_eff].
export function seedContract(field, value, direction) {
  persistedValue(field, value, "base");
  const budget = seedBudget(field, value);
  if (budget === null) return { error: `no seed budget for ${field}` };
  const [dlo, dhi] = representable(field);
  const room = direction === "decrease" ? value - (scalarField(field).domain === "positive" ? 0 : dlo) : dhi - value;
  const effective = Math.min(budget, room);
  if (!(effective > 0)) return { error: `seed budget for ${field} at ${value} is zero` };
  const sign = direction === "increase" ? 1 : -1;
  return contract(field, "seed", direction, value, value + sign * 0.25 * effective, value + sign * effective);
}

// The scorer's test: { ok } or { ok: false, reason }.
export function acceptScalar(accept, value) {
  const { field, direction, current, interval } = accept;
  if (!inRawDomain(field, value)) return { ok: false, reason: "out of domain" };
  const p = project(field, value);
  if (!inRepresentable(field, p)) return { ok: false, reason: "projects outside the domain" };
  if (direction === "decrease" ? !(p < current) : !(p > current)) return { ok: false, reason: "not on the diagnosed side" };
  if (p < interval[0] || p > interval[1]) return { ok: false, reason: "outside the accepted interval" };
  return { ok: true };
}

// Shortest round-trip decimal of a float32, for rules.md and intent targets.
export function float32Text(value) {
  const f = Math.fround(value);
  for (let digits = 1; digits <= 17; digits++) {
    const text = Number(f.toPrecision(digits)).toString();
    if (Math.fround(Number(text)) === f) return text;
  }
  return f.toString();
}

export function persistedText(field, value) {
  return scalarField(field).type === "uint32" ? String(value) : float32Text(value);
}
