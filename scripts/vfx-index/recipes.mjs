// [vfx-index] The recipe registry: curated, reviewed recipes,
// registered by hand in v1 (no automatic mining). validateRecipe implements
// exactly recipe.schema.json, plus what the schema cannot say: each type's
// emitter count, and, given the catalog, that the source effect, its emitters
// and every expected texture exist under the same content stack.
//
//   node scripts/vfx-index/recipes.mjs register-recipe --registry <recipes.json>
//        --recipe <recipe.json> [--catalog <catalog.json>]
//
// The registry is { schema: 1, recipes: [...] }, sorted by id. An id is
// registered once; a rejected recipe leaves the registry untouched.
import * as fs from "node:fs";
import { pathToFileURL } from "node:url";

export const TYPES = ["single-emitter", "subtree", "layer-group", "template"];
// The fields each object may carry, as recipe.schema.json lists them.
export const FIELDS = {
  recipe: ["schema", "id", "type", "title", "description", "tags", "source", "emitters", "dependencies", "expectedTextures", "safeTransforms"],
  source: ["effect", "sha256", "contentStackHash"],
  dependency: ["kind", "name"],
  expectedTexture: ["canonicalName", "sha256"],
  safeTransform: ["field", "min", "max", "note"],
};
const HEX64 = /^[0-9a-f]{64}$/;
const ID = /^[a-z0-9]+(-[a-z0-9]+)*$/;
const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const isText = (v, max = Infinity) => typeof v === "string" && v.length >= 1 && v.length <= max;

function only(value, keys, path, errors) {
  for (const k of Object.keys(value)) if (!keys.includes(k)) errors.push(`${path}.${k}: not a recipe field`);
}

export function validateRecipe(r, { catalog = null } = {}) {
  const errors = [];
  if (!isObject(r)) return ["the recipe is not an object"];
  only(r, FIELDS.recipe, "recipe", errors);
  for (const k of ["schema", "id", "type", "title", "source", "dependencies", "expectedTextures", "safeTransforms"])
    if (!(k in r)) errors.push(`recipe.${k}: required`);
  if ("schema" in r && r.schema !== 1) errors.push("recipe.schema: must be 1");
  if ("id" in r && !(isText(r.id, 80) && ID.test(r.id))) errors.push("recipe.id: lowercase words joined by hyphens, at most 80 characters");
  if ("type" in r && !TYPES.includes(r.type)) errors.push(`recipe.type: one of ${TYPES.join(", ")}`);
  if ("title" in r && !isText(r.title, 120)) errors.push("recipe.title: 1-120 characters");
  if ("description" in r && !(typeof r.description === "string" && r.description.length <= 2000)) errors.push("recipe.description: at most 2000 characters");
  if ("tags" in r && !(Array.isArray(r.tags) && r.tags.length <= 20 && r.tags.every((t) => isText(t, 40))))
    errors.push("recipe.tags: at most 20 tags of 1-40 characters");
  if ("source" in r) {
    if (!isObject(r.source)) errors.push("recipe.source: an object");
    else {
      only(r.source, FIELDS.source, "recipe.source", errors);
      if (!isText(r.source.effect)) errors.push("recipe.source.effect: required");
      if (!HEX64.test(r.source.sha256 ?? "")) errors.push("recipe.source.sha256: 64 lowercase hex digits");
      if (!HEX64.test(r.source.contentStackHash ?? "")) errors.push("recipe.source.contentStackHash: 64 lowercase hex digits");
    }
  }
  if ("emitters" in r && !(Array.isArray(r.emitters) && r.emitters.every((e) => isText(e))))
    errors.push("recipe.emitters: an array of emitter names");
  const emitters = Array.isArray(r.emitters) ? r.emitters.length : 0;
  const need = { "single-emitter": [1, 1], subtree: [1, Infinity], "layer-group": [2, Infinity], template: [0, 0] }[r.type];
  if (need && (emitters < need[0] || emitters > need[1]))
    errors.push(r.type === "template" ? "recipe.emitters: a template is the whole effect; leave emitters out"
      : `recipe.emitters: a ${r.type} names ${need[1] === need[0] ? need[0] : `at least ${need[0]}`} emitter(s)`);
  if ("dependencies" in r) {
    if (!Array.isArray(r.dependencies)) errors.push("recipe.dependencies: an array (empty when there are none)");
    else r.dependencies.forEach((d, i) => {
      if (!isObject(d)) return errors.push(`recipe.dependencies[${i}]: an object`);
      only(d, FIELDS.dependency, `recipe.dependencies[${i}]`, errors);
      if (!["texture", "effect", "shader"].includes(d.kind)) errors.push(`recipe.dependencies[${i}].kind: texture, effect or shader`);
      if (!isText(d.name)) errors.push(`recipe.dependencies[${i}].name: required`);
    });
  }
  if ("expectedTextures" in r) {
    if (!Array.isArray(r.expectedTextures) || !r.expectedTextures.length) errors.push("recipe.expectedTextures: at least one texture");
    else r.expectedTextures.forEach((t, i) => {
      if (!isObject(t)) return errors.push(`recipe.expectedTextures[${i}]: an object`);
      only(t, FIELDS.expectedTexture, `recipe.expectedTextures[${i}]`, errors);
      if (!isText(t.canonicalName)) errors.push(`recipe.expectedTextures[${i}].canonicalName: required`);
      if (!HEX64.test(t.sha256 ?? "")) errors.push(`recipe.expectedTextures[${i}].sha256: 64 lowercase hex digits`);
    });
  }
  if ("safeTransforms" in r) {
    if (!Array.isArray(r.safeTransforms)) errors.push("recipe.safeTransforms: an array (empty when the recipe is reused as is)");
    else r.safeTransforms.forEach((t, i) => {
      if (!isObject(t)) return errors.push(`recipe.safeTransforms[${i}]: an object`);
      only(t, FIELDS.safeTransform, `recipe.safeTransforms[${i}]`, errors);
      if (!isText(t.field)) errors.push(`recipe.safeTransforms[${i}].field: required`);
      for (const k of ["min", "max"]) if (k in t && !Number.isFinite(t[k])) errors.push(`recipe.safeTransforms[${i}].${k}: a finite number`);
      if (Number.isFinite(t.min) && Number.isFinite(t.max) && t.min > t.max) errors.push(`recipe.safeTransforms[${i}]: min above max`);
      if ("note" in t && !(typeof t.note === "string" && t.note.length <= 400)) errors.push(`recipe.safeTransforms[${i}].note: at most 400 characters`);
    });
  }
  if (!errors.length && catalog) errors.push(...checkAgainstCatalog(r, catalog));
  return errors;
}

// The recipe must describe content the catalog's stack actually holds: its
// effect, that effect's emitters, and textures that effect uses.
function checkAgainstCatalog(r, catalog) {
  const errors = [];
  if (r.source.contentStackHash !== catalog.content.sha256)
    errors.push(`recipe.source.contentStackHash: the catalog covers stack ${catalog.content.sha256}`);
  const effect = catalog.effects.find((e) => e.file === r.source.effect);
  if (!effect) return [...errors, `recipe.source.effect: ${r.source.effect} is not in the catalog`];
  if (effect.sha256 !== r.source.sha256) errors.push(`recipe.source.sha256: the catalog's ${r.source.effect} hashes to ${effect.sha256}`);
  for (const name of r.emitters ?? [])
    if (!effect.emitterNames.includes(name)) errors.push(`recipe.emitters: no emitter "${name}" in ${r.source.effect}`);
  const used = new Set(catalog.assets.filter((a) => effect.textures.includes(a.id)).map((a) => `${a.canonicalName}#${a.identity.sha256}`));
  for (const t of r.expectedTextures)
    if (!used.has(`${t.canonicalName}#${t.sha256}`))
      errors.push(`recipe.expectedTextures: ${r.source.effect} does not use ${t.canonicalName} with that hash`);
  return errors;
}

export function registerRecipe(registryPath, recipe, { catalog = null, io = fs } = {}) {
  const errors = validateRecipe(recipe, { catalog });
  const registry = io.existsSync(registryPath) ? JSON.parse(io.readFileSync(registryPath, "utf8")) : { schema: 1, recipes: [] };
  if (registry.schema !== 1 || !Array.isArray(registry.recipes)) errors.push(`${registryPath}: not a recipe registry (schema 1)`);
  else if (registry.recipes.some((r) => r.id === recipe?.id)) errors.push(`recipe.id: "${recipe.id}" is already registered`);
  if (errors.length) return { registered: false, errors };
  registry.recipes = [...registry.recipes, recipe].sort((a, b) => a.id.localeCompare(b.id));
  const temp = `${registryPath}.tmp-${process.pid}`;
  io.writeFileSync(temp, JSON.stringify(registry, null, 2) + "\n");
  io.renameSync(temp, registryPath);
  return { registered: true, errors: [], count: registry.recipes.length };
}

export function main(argv = process.argv.slice(2), { io = fs, log = console.log } = {}) {
  const [verb, ...rest] = argv;
  const opts = {};
  for (let i = 0; i < rest.length; i += 2) {
    if (!["--registry", "--recipe", "--catalog"].includes(rest[i]) || rest[i + 1] == null)
      throw Object.assign(new Error(`unexpected argument ${rest[i]}`), { exitCode: 2 });
    opts[rest[i].slice(2)] = rest[i + 1];
  }
  if (verb !== "register-recipe" || !opts.registry || !opts.recipe)
    throw Object.assign(new Error("usage: recipes.mjs register-recipe --registry <recipes.json> --recipe <recipe.json> [--catalog <catalog.json>]"), { exitCode: 2 });
  const recipe = JSON.parse(io.readFileSync(opts.recipe, "utf8"));
  const catalog = opts.catalog ? JSON.parse(io.readFileSync(opts.catalog, "utf8")) : null;
  const result = registerRecipe(opts.registry, recipe, { catalog, io });
  if (!result.registered) {
    result.errors.forEach((e) => log(`rejected: ${e}`));
    throw Object.assign(new Error(`recipe rejected (${result.errors.length} problem(s))`), { exitCode: 5 });
  }
  log(`registered ${recipe.id} (${result.count} recipe(s) in ${opts.registry})`);
  return result;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
  try { main(); } catch (error) { console.error(`[vfx-index] ${error.message}`); process.exit(error.exitCode ?? 1); }
}
