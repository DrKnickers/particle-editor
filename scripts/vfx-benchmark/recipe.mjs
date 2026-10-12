// Recipe verification (plan 3.4, recipe.mjs; execution notes: the native
// patcher is the projection). Pure: inspect dumps in, differences out.
//
//   field        the base patched by `--tool patch` with the recipe's request
//                must inspect exactly as the case does; the editor's own save
//                rules are the only projection, so nothing here re-implements them;
//   permutation  a base-position -> case-position map: every emitter's payload
//                equal by mapped identity, with only the derived index, parent
//                and spawn-slot changes the reorder makes;
//   seed, none   the inspect diff is empty (byte identity is checked by the caller).

export const GROUP_SPEED = 0, GROUP_LIFETIME = 1, GROUP_POSITION = 2;

const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);

// Every path at which two JSON values differ; `ignore` holds root keys to skip.
export function inspectDiff(a, b, { ignore = ["sourceDocumentHash"] } = {}) {
  const out = [];
  const walk = (x, y, path) => {
    if (Array.isArray(x) && Array.isArray(y)) {
      if (x.length !== y.length) { out.push(`${path}.length`); return; }
      x.forEach((v, i) => walk(v, y[i], `${path}[${i}]`));
    } else if (isObject(x) && isObject(y)) {
      for (const k of new Set([...Object.keys(x), ...Object.keys(y)])) {
        if (path === "" && ignore.includes(k)) continue;
        if (!Object.hasOwn(x, k) || !Object.hasOwn(y, k)) out.push(`${path}.${k}`);
        else walk(x[k], y[k], `${path}.${k}`);
      }
    } else if (!Object.is(x, y)) {
      out.push(path || ".");
    }
  };
  walk(a, b, "");
  return out;
}

// The emitter's place in the spawn tree, as --tool patch selects it.
export function rolePath(inspect, position) {
  const steps = [];
  let at = position;
  for (let guard = 0; guard <= inspect.emitters.length; guard++) {
    const e = inspect.emitters[at];
    if (e.parent === null) { steps.unshift({ role: "root", name: e.name }); return steps; }
    const parent = inspect.emitters[e.parent];
    const role = parent.spawnDuringLife === at ? "lifeChild" : parent.spawnOnDeath === at ? "deathChild" : null;
    if (!role) throw new Error(`emitter ${at} is not in its parent's spawn slots`);
    steps.unshift({ role, name: e.name });
    at = e.parent;
  }
  throw new Error("the spawn tree has a cycle");
}

export function positionOf(inspect, ref) {
  const at = inspect.emitters.findIndex((e) => e.index === ref.index && e.name === ref.name);
  if (at === -1) throw new Error(`no emitter ${ref.index} ${JSON.stringify(ref.name)}`);
  return at;
}

// The --tool patch request a field recipe authors.
export function patchRequest(recipe, baseInspect) {
  const at = positionOf(baseInspect, recipe.target);
  return {
    schema: 1,
    selector: { sourceDocumentHash: baseInspect.sourceDocumentHash, rolePath: rolePath(baseInspect, at), expected: { name: recipe.target.name } },
    mutations: recipe.mutations,
  };
}

const LINKS = ["index", "parent", "spawnDuringLife", "spawnOnDeath"];

function verifyPermutation(recipe, base, kase) {
  const errors = [];
  const map = recipe.map;
  const n = base.emitters.length;
  if (!Array.isArray(map) || map.length !== n || kase.emitters.length !== n || new Set(map).size !== n || !map.every((p) => Number.isInteger(p) && p >= 0 && p < n)) {
    return ["the permutation map is not a bijection of the emitter positions"];
  }
  if (map.every((p, i) => p === i)) return ["the permutation is the identity"];
  const mapped = (v) => (v === null ? null : map[v]);
  for (let b = 0; b < n; b++) {
    const be = base.emitters[b], ce = kase.emitters[map[b]];
    const strip = (e) => Object.fromEntries(Object.entries(e).filter(([k]) => !LINKS.includes(k)));
    for (const path of inspectDiff(strip(be), strip(ce), { ignore: [] })) errors.push(`emitter ${b} -> ${map[b]}: ${path} differs`);
    for (const k of ["parent", "spawnDuringLife", "spawnOnDeath"]) {
      if (ce[k] !== mapped(be[k])) errors.push(`emitter ${b} -> ${map[b]}: ${k} is not the mapped ${k}`);
    }
    // The authored index follows the list position the reorder moved it to.
    if (be.index === b && ce.index !== map[b]) errors.push(`emitter ${b} -> ${map[b]}: index ${ce.index} is not the derived ${map[b]}`);
    if (be.index !== b) errors.push(`base emitter ${b} has index ${be.index}, not its position`);
  }
  const { emitters: be, ...baseRest } = base, { emitters: ce, ...caseRest } = kase;
  for (const path of inspectDiff(baseRest, caseRest)) errors.push(`system ${path} differs`);
  const front = recipe.relation?.front, back = recipe.relation?.back;
  if (!recipe.decoy && (!front || !back)) errors.push("a permutation records its {front, back} relation");
  return errors;
}

// -> [] when the case is exactly what the recipe says.
export function verifyRecipe({ recipe, baseInspect, caseInspect, patchedInspect = null }) {
  switch (recipe.kind) {
    case "field": {
      if (!patchedInspect) return ["a field recipe needs the patched base's inspect"];
      if (inspectDiff(baseInspect, caseInspect).length === 0) return ["the case equals its base"];
      return inspectDiff(patchedInspect, caseInspect).map((p) => `${p} differs from the patched base`);
    }
    case "permutation":
      return verifyPermutation(recipe, baseInspect, caseInspect);
    case "seed":
    case "none":
      return inspectDiff(baseInspect, caseInspect).map((p) => `${p} differs from the base`);
    default:
      return [`unknown recipe kind ${JSON.stringify(recipe.kind)}`];
  }
}
