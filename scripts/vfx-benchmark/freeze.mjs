// Freezing answer variants at lock (plan 3.8, decision 11). The owner's blind
// pass names each variant's diagnosis, windows, policy and correction target;
// lock derives what it accepts from the public rules and the base's stated
// targets, never from a per-case choice:
//   variant 0 of a field case   restore  (the planted field, nonzero delta)
//   variant 0 of the seed case  seed     (field and direction frozen in truth)
//   any other variant           intervene (a field the recipe left unchanged)
import { interveneContract, project, restoreContract, scalarField, seedContract } from "./persisted.mjs";
import { atlasCells, isPerfectSquare, stepTrackEquivalent } from "./track.mjs";
import { sameRef } from "./semantic.mjs";
import { positionOf } from "./recipe.mjs";

export const STEP = 2;   // ParticleSystem::Emitter::Track::IT_STEP
export const PATCH_VOCABULARY = ["scalar", "enum", "texture", "track"];   // design 15.2: no structural reorder

const ref = (e) => ({ index: e.index, name: e.name });

// The intent's *Targets* section: one line per stated quantity,
//   - <name> (<index>) <field>: <value>
// with the value printed exactly as the field persists.
export function parseTargets(markdown) {
  const section = /^##\s+Targets\s*$([\s\S]*?)(?=^##\s|(?![\s\S]))/m.exec(markdown);
  if (!section) return { targets: [], errors: ["intent.md has no ## Targets section"] };
  const targets = [], errors = [];
  for (const line of section[1].split(/\r?\n/)) {
    if (!line.trim()) continue;
    const m = /^- (.+) \((\d+)\) ([A-Za-z]+): (\S+)\s*$/.exec(line);
    if (!m) { errors.push(`unreadable target line: ${line}`); continue; }
    targets.push({ emitter: { index: Number(m[2]), name: m[1] }, field: m[3], text: m[4], value: Number(m[4]) });
  }
  return { targets, errors };
}

// A stated target must be the base's persisted value exactly, printed as it persists.
export function checkTargets(targets, baseInspect, printed) {
  const errors = [];
  for (const t of targets) {
    let at;
    try { at = positionOf(baseInspect, t.emitter); } catch (e) { errors.push(e.message); continue; }
    let spec;
    try { spec = scalarField(t.field); } catch (e) { errors.push(e.message); continue; }
    const base = baseInspect.emitters[at][t.field];
    if (printed(t.field, base) !== t.text) errors.push(`target ${t.emitter.name} ${t.field}: ${t.text} is not the persisted ${printed(t.field, base)}`);
    else if (spec.type === "float32" ? project(t.field, t.value) !== base : t.value !== base) errors.push(`target ${t.emitter.name} ${t.field} differs from the base`);
  }
  return errors;
}

function option(rubric, v) {
  const options = rubric.categories[v.category]?.directions?.[v.direction];
  if (!options) return { error: `${v.category}/${v.direction} is not in the rubric` };
  const c = v.correction;
  const found = options.find((o) => o.kind === c.kind && (o.field === undefined || o.field === c.field) && (o.track === undefined || o.track === c.track));
  return found ? { option: found } : { error: `${c.kind} ${c.field ?? c.track ?? ""} is not a correction option for ${v.category}/${v.direction}` };
}

// owner variant -> frozen variant, or { error }.
// ctx: { recipe, baseInspect, caseInspect, targets, rubric, isRecipeVariant }
export function freezeVariant(v, ctx) {
  const { recipe, baseInspect, caseInspect, targets, rubric } = ctx;
  if (v.category === "draw-order") {
    if (v.correction.kind !== "reorder" || !v.relation) return { error: "draw-order freezes a reorder with its relation" };
    return { ...v, correction: { kind: "reorder" }, patchEligible: false };
  }
  const found = option(rubric, v);
  if (found.error) return found;
  const target = v.correction.emitter;
  let at;
  try { at = positionOf(caseInspect, target); } catch (e) { return { error: e.message }; }
  const baseEmitter = baseInspect.emitters[at], caseEmitter = caseInspect.emitters[at];
  if (!sameRef(ref(baseEmitter), target)) return { error: "the correction target differs between base and case" };
  const patchEligible = caseEmitter.linkGroup === 0 && PATCH_VOCABULARY.includes(v.correction.kind);
  const c = v.correction;
  switch (c.kind) {
    case "scalar": {
      const field = c.field, base = baseEmitter[field], current = caseEmitter[field];
      let accept;
      if (recipe.kind === "seed") {
        if (!ctx.isRecipeVariant) return { error: "the seed case has one answer, frozen in truth" };
        if (base !== current) return { error: "a seed contract needs a zero planted delta" };
        accept = seedContract(field, current, found.option.direction);
      } else if (ctx.isRecipeVariant) {
        if (base === current) return { error: `restore needs a planted delta on ${field}` };
        const stated = targets.find((t) => sameRef(t.emitter, target) && t.field === field);
        if (!stated) return { error: `${field} on ${target.name} has no stated target` };
        accept = restoreContract(field, base, current);
      } else {
        if (base !== current) return { error: `an alternative may not correct the planted field ${field}` };
        const stated = targets.find((t) => sameRef(t.emitter, target) && t.field === field);
        if (!stated) return { error: `${field} on ${target.name} has no stated target` };
        accept = interveneContract(field, base, base, found.option.direction);
      }
      if (accept.error) return { error: accept.error };
      if (accept.direction !== found.option.direction) return { error: `the ${accept.rule} direction ${accept.direction} is not the category's ${found.option.direction}` };
      return { ...v, correction: { kind: "scalar", emitter: ref(target), field, accept }, patchEligible };
    }
    case "enum": {
      if (found.option.acceptable.includes(caseEmitter[c.field])) return { error: "the current blend mode is already acceptable" };
      return { ...v, correction: { kind: "enum", emitter: ref(target), field: c.field, acceptable: found.option.acceptable }, patchEligible };
    }
    case "texture":
      return { ...v, correction: { kind: "texture", emitter: ref(target), field: c.field }, patchEligible };
    case "track": {
      const track = baseEmitter.tracks[c.track];
      if (track.interpolation !== STEP) return { error: "the base index track is not step-interpolated" };
      if (!isPerfectSquare(baseEmitter.textureSize)) return { error: `textureSize ${baseEmitter.textureSize} is not a positive perfect square` };
      const cells = atlasCells(baseEmitter.textureSize);
      if (!track.keys.every(([, value]) => value >= 0 && value < cells)) return { error: "a base index key lies outside the atlas cells" };
      if (stepTrackEquivalent(track.keys, caseEmitter.tracks[c.track].keys)) return { error: "the case track plays like the base" };
      return { ...v, correction: { kind: "track", emitter: ref(target), track: c.track, baseKeys: track.keys, cells }, patchEligible };
    }
  }
  return { error: `unknown correction kind ${c.kind}` };
}
