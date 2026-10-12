// Corpus admission (plan 3.1, 3.4 corpus.mjs, 3.5). Pure: parsed JSON and
// inspect dumps in, refusals out. The I/O checks (reparse points, hashes on
// disk) live in check-corpus.mjs.
import { stepTrackEquivalent, isPerfectSquare, atlasCells } from "./track.mjs";
import { GROUP_LIFETIME, GROUP_POSITION, GROUP_SPEED, positionOf } from "./recipe.mjs";
import { STEP } from "./freeze.mjs";
import { RUBRIC } from "./schema-check.mjs";

const GT_EXACT = 0;
const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);

// Strict keys at one level.
function keys(obj, required, optional, where, errors) {
  if (!isObject(obj)) { errors.push(`${where}: must be an object`); return false; }
  for (const k of required) if (!Object.hasOwn(obj, k)) errors.push(`${where}: missing ${k}`);
  for (const k of Object.keys(obj)) if (!required.includes(k) && !optional.includes(k)) errors.push(`${where}: unknown key ${k}`);
  return true;
}

// Windows paths compared case-insensitively, with one separator form.
export function normalisePath(p) {
  return p.replace(/\//g, "\\").replace(/\\+$/, "").toLowerCase();
}
export const encloses = (outer, inner) => {
  const o = normalisePath(outer), i = normalisePath(inner);
  return i === o || i.startsWith(`${o}\\`);
};
const absolute = (p) => /^[A-Za-z]:[\\/]/.test(p);

// Root rules (3.1): absolute; no store, run or CLI root under the checkout or
// enclosing it; no run or CLI root under the stores; content roots may nest
// among themselves and collapse to their outermost covering roots; every
// other overlap refuses.
export function checkRoots({ repo, stores, runs, cli, bin, content = [] }) {
  const errors = [];
  const named = [["stores", stores], ["runs", runs], ["cli", cli], ["bin", bin]].filter(([, p]) => p);
  for (const [name, p] of [...named, ...content.map((c, i) => [`content[${i}]`, c])]) {
    if (!absolute(p)) errors.push(`${name} root ${p} is not absolute`);
    if (encloses(repo, p)) errors.push(`${name} root ${p} is under the repository`);
    if (encloses(p, repo)) errors.push(`${name} root ${p} encloses the repository`);
  }
  for (let i = 0; i < named.length; i++) {
    for (let j = i + 1; j < named.length; j++) {
      const [a, pa] = named[i], [b, pb] = named[j];
      if (encloses(pa, pb) || encloses(pb, pa)) errors.push(`${a} and ${b} roots overlap`);
    }
  }
  for (const c of content) {
    for (const [name, p] of named) if (encloses(c, p) || encloses(p, c)) errors.push(`content root ${c} overlaps the ${name} root`);
  }
  return errors;
}

// The outermost content roots: the covering set the ACL steps would use.
export function coveringRoots(content) {
  return content.filter((c, i) => !content.some((d, j) => j !== i && encloses(d, c) && !(normalisePath(d) === normalisePath(c) && j > i)));
}

const CASE_KINDS = ["defect", "control"];
const SHAPES = ["single", "three"];
const CLASS_IDS = RUBRIC.classes.map((c) => c.id);

export function checkCorpus(corpus) {
  const errors = [];
  if (!keys(corpus, ["schemaVersion", "version", "state", "provenance", "targets", "attempts", "bases", "cases", "requiredClasses"],
    ["reducedClassList", "seedAbsence", "seedSearch", "order", "hashes", "pins", "lock"], "corpus", errors)) return errors;
  if (corpus.schemaVersion !== 1) errors.push("corpus: schemaVersion must be 1");
  if (!["draft", "locked"].includes(corpus.state)) errors.push("corpus: state must be draft or locked");
  if (keys(corpus.provenance, ["exeSha256", "contentRoots"], ["engineCommit", "gameVersions", "render"], "provenance", errors)) {
    if (!Array.isArray(corpus.provenance.contentRoots) || corpus.provenance.contentRoots.length === 0) errors.push("provenance: contentRoots must be a non-empty ordered list");
  }
  keys(corpus.targets, ["objective", "subjective", "controlFlag", "fdp", "completion"], [], "targets", errors);
  if (corpus.attempts !== 3) errors.push("corpus: three scheduled attempts per case");
  const bases = new Set();
  for (const [i, b] of (corpus.bases ?? []).entries()) {
    if (keys(b, ["id"], ["seedBase", "drawOrderBase", "canonical"], `bases[${i}]`, errors)) {
      if (bases.has(b.id)) errors.push(`bases[${i}]: duplicate id ${b.id}`);
      bases.add(b.id);
    }
  }
  const ids = new Set();
  for (const [i, c] of (corpus.cases ?? []).entries()) {
    if (!keys(c, ["id", "base", "kind", "shape"], ["class", "stratum", "secondaryEligible", "eligibility", "decoy"], `cases[${i}]`, errors)) continue;
    if (ids.has(c.id)) errors.push(`cases[${i}]: duplicate id ${c.id}`);
    ids.add(c.id);
    if (!bases.has(c.base)) errors.push(`cases[${i}]: unknown base ${c.base}`);
    if (!CASE_KINDS.includes(c.kind)) errors.push(`cases[${i}]: kind must be defect or control`);
    if (!SHAPES.includes(c.shape)) errors.push(`cases[${i}]: shape must be single or three`);
    if (c.kind === "defect" && !CLASS_IDS.includes(c.class)) errors.push(`cases[${i}]: unknown class ${c.class}`);
    if (c.decoy !== undefined && (c.decoy !== true || c.kind !== "control" || c.shape !== "single")) errors.push(`cases[${i}]: a decoy is a single-bundle control`);
  }
  // Required control strata: a single-bundle control per base a defect uses;
  // the three-seed control exactly when the seed case is present.
  const cases = corpus.cases ?? [];
  for (const base of new Set(cases.filter((c) => c.kind === "defect").map((c) => c.base))) {
    if (!cases.some((c) => c.kind === "control" && c.shape === "single" && c.base === base)) errors.push(`base ${base} has no single-bundle control`);
  }
  const seed = cases.some((c) => c.class === "seed"), seedControl = cases.some((c) => c.kind === "control" && c.shape === "three");
  if (seed !== seedControl) errors.push("the seed case and its three-seed control are retained or dropped together");
  // Required defect strata: every class but the seed case, unless a reduced list declares the gap.
  const covered = new Set(cases.filter((c) => c.kind === "defect").map((c) => c.class));
  const declared = new Set(corpus.reducedClassList ?? []);
  for (const cls of corpus.requiredClasses ?? []) {
    if (!covered.has(cls) && !declared.has(cls)) errors.push(`required class ${cls} has no retained case`);
  }
  if (!seed && !corpus.seedAbsence) errors.push("an absent seed case needs a pre-registered absence reason");
  if (corpus.seedAbsence && !["a", "b", "c", "d"].includes(corpus.seedAbsence)) errors.push("seedAbsence must be a, b, c or d");
  if (corpus.seedSearch) keys(corpus.seedSearch, ["base", "drawSeed", "labelSeed"], [], "seedSearch", errors);
  if (corpus.order && (corpus.order.length !== ids.size || !corpus.order.every((id) => ids.has(id)))) errors.push("order must be a permutation of the case ids");
  return errors;
}

export function checkRecipe(recipe) {
  const errors = [];
  if (!keys(recipe, ["schemaVersion", "base", "kind"], ["class", "category", "direction", "emitters", "target", "mutations", "map", "relation", "seeds", "outlier", "decoy"], "recipe", errors)) return errors;
  if (!["field", "permutation", "seed", "none"].includes(recipe.kind)) errors.push("recipe: kind must be field, permutation, seed or none");
  if (recipe.kind === "field" && (!recipe.target || !Array.isArray(recipe.mutations) || recipe.mutations.length === 0)) errors.push("a field recipe names its target and mutations");
  if (recipe.kind === "field" && recipe.mutations?.some((m) => m.op === "rename")) errors.push("a case never renames an emitter");
  // A decoy reorders emitters whose order cannot show: a control, so it names no class or relation.
  if (recipe.decoy !== undefined && (recipe.decoy !== true || recipe.kind !== "permutation" || recipe.class !== undefined || recipe.relation !== undefined)) errors.push("a decoy is a permutation with no class or relation");
  else if (recipe.kind === "permutation" && (!Array.isArray(recipe.map) || (!recipe.decoy && !recipe.relation))) errors.push("a permutation recipe records its map and relation");
  if (recipe.kind === "seed" && (!isObject(recipe.seeds) || !recipe.outlier)) errors.push("a seed recipe records its seed map and outlier");
  return errors;
}

// Every emitter's colour texture is named (an empty name would be substituted
// unseen); the seed and draw-order bases meet their structural rules.
export function admitBase(inspect, { seedBase = false, drawOrderBase = false } = {}) {
  const errors = [];
  inspect.emitters.forEach((e, i) => {
    if (e.colorTexture === "") errors.push(`emitter ${i} ${e.name} has an empty colorTexture`);
  });
  if (seedBase && !inspect.emitters.some((e) => e.randomLifetimePerc > 0)) errors.push("the seed base needs randomLifetimePerc > 0");
  if (drawOrderBase) {
    inspect.emitters.forEach((e, i) => {
      const random = e.randomLifetimePerc !== 0 || e.randomScalePerc !== 0 || e.randomColors.some((x) => x !== 0) || e.randomRotation || e.randomRotationDirection;
      if (random) errors.push(`emitter ${i} ${e.name} has random fields`);
      if (e.groups[GROUP_SPEED].type !== GT_EXACT || e.groups[GROUP_POSITION].type !== GT_EXACT) errors.push(`emitter ${i} ${e.name} has a non-exact position or speed group`);
      if (e.isWeatherParticle) errors.push(`emitter ${i} ${e.name} is a weather emitter`);
      if (e.linkGroup !== 0) errors.push(`emitter ${i} ${e.name} is linked`);
    });
  }
  return errors;
}

// The magnitude ranges of plan 3.5, by class: the ratio or the stored value.
export const MAGNITUDES = {
  "short-lifetime": { field: "lifetime", ratio: [0.3, 0.5] },
  "lingering-trail": { field: "lifetime", ratio: [2.5, 4] },
  "late-delay": { field: "initialDelay", add: [0.4, 0.8] },
  "child-timing": { field: "initialDelay", add: [0.3, 0.6] },
  "random-lifetime": { field: "randomLifetimePerc", stored: [0.8, 0.95] },
  "random-scale": { field: "randomScalePerc", stored: [0.8, 0.95] },
  "particle-count": { ratio: [6, 10] },
};
const within = (v, [lo, hi]) => v >= lo * (1 - 1e-6) && v <= hi * (1 + 1e-6);

const isDeathChild = (inspect, at) => {
  const p = inspect.emitters[at].parent;
  return p !== null && inspect.emitters[p].spawnOnDeath === at;
};
const isLifeChild = (inspect, at) => {
  const p = inspect.emitters[at].parent;
  return p !== null && inspect.emitters[p].spawnDuringLife === at;
};

// The structural refusals for one defect recipe against its base and case dumps.
export function admitRecipe(recipe, baseInspect, caseInspect) {
  const errors = [];
  if (recipe.kind === "field") {
    let at;
    try { at = positionOf(baseInspect, recipe.target); } catch (e) { return [e.message]; }
    const b = baseInspect.emitters[at], c = caseInspect.emitters[at];
    if (b.linkGroup !== 0) errors.push("the recipe target is linked");
    const names = (x) => x.emitters.map((e) => e.name).join("\u0000");
    if (names(baseInspect) !== names(caseInspect)) errors.push("a case renames an emitter");
    const changed = caseInspect.emitters.filter((e, i) => JSON.stringify(e) !== JSON.stringify(baseInspect.emitters[i])).length;
    if (changed > 1) errors.push("a field recipe touches more than one emitter");
    const m = MAGNITUDES[recipe.class];
    if (m) {
      const field = m.field ?? (b.useBursts ? "nParticlesPerBurst" : "nParticlesPerSecond");
      if (recipe.class === "particle-count") {
        if (b.isWeatherParticle) errors.push("a count recipe never targets a weather emitter");
        const edited = ["nParticlesPerBurst", "nParticlesPerSecond"].filter((f) => b[f] !== c[f]);
        if (edited.length !== 1 || edited[0] !== field) errors.push(`the count recipe must edit ${field}, the field that controls emission`);
      }
      const ratio = c[field] / b[field], add = c[field] - b[field];
      if (m.ratio && !within(ratio, m.ratio)) errors.push(`${field} x${ratio} is outside ${m.ratio.join("-")}`);
      if (m.add && !within(add, m.add)) errors.push(`${field} +${add} is outside ${m.add.join("-")}`);
      if (m.stored && !within(c[field], m.stored)) errors.push(`${field} ${c[field]} is outside ${m.stored.join("-")}`);
    }
    if (b.initialDelay !== c.initialDelay) {
      if (b.isWeatherParticle) errors.push("an initialDelay target is never a weather emitter");
      if (isDeathChild(baseInspect, at)) errors.push("an initialDelay target is never a death child");
      if (isLifeChild(baseInspect, at)) {
        const parent = baseInspect.emitters[b.parent];
        if (!(parent.groups[GROUP_LIFETIME].minY > c.initialDelay)) errors.push("the parent's minimum lifetime does not exceed the edited delay");
      }
      if (recipe.class === "child-timing" && (!isLifeChild(baseInspect, at) || b.skipTime !== 0)) errors.push("child timing needs a during-life child with skipTime 0");
    }
    if (recipe.class === "reversed-flipbook") {
      const bt = b.tracks[5], ct = c.tracks[5];
      if (bt.interpolation !== STEP) errors.push("the base index track is not step-interpolated");
      if (!bt.keys.every((k, i) => i === 0 || k[0] > bt.keys[i - 1][0])) errors.push("the base index key times are not strictly increasing");
      if (!isPerfectSquare(b.textureSize)) errors.push(`textureSize ${b.textureSize} is not a positive perfect square`);
      else if (!bt.keys.every(([, v]) => v >= 0 && v < atlasCells(b.textureSize))) errors.push("a base index key is outside the atlas cells");
      if (stepTrackEquivalent(bt.keys, ct.keys)) errors.push("the planted track plays like the base");
    }
  } else if (recipe.kind === "permutation") {
    // The relation names case identities: what the agent sees. A decoy has none.
    for (const ref of recipe.decoy ? [] : [recipe.relation?.front, recipe.relation?.back]) {
      let at;
      try { at = positionOf(caseInspect, ref); } catch (e) { errors.push(e.message); continue; }
      const e = caseInspect.emitters[at];
      if (e.parent !== null || e.spawnDuringLife !== null || e.spawnOnDeath !== null) errors.push(`${e.name} is not a childless root`);
    }
  }
  return errors;
}
