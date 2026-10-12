// [vfx-index] Catalog, search, recipes and build, over records in the native
// tool's exact shape (testing.mjs). Run: node --test scripts/vfx-index/
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import * as os from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";
import { buildCatalog, gridSide } from "./catalog.mjs";
import { search, main as searchMain } from "./search.mjs";
import { validateRecipe, registerRecipe, FIELDS, main as recipesMain } from "./recipes.mjs";
import { main as buildMain } from "./build.mjs";
import { record, recordsDoc, stockRecords, sha, STACK } from "./testing.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const scratch = () => fs.mkdtempSync(join(os.tmpdir(), "vfx-index-"));
const catalog = () => buildCatalog(recordsDoc(stockRecords()));
const asset = (c, name) => c.assets.find((a) => a.canonicalName === name);
const names = (results) => results.map((r) => r.canonicalName ?? r.id);
const quiet = () => {};
const TORCH = "DATA\\ART\\MODELS\\P_TORCH.ALO";

// ---- catalog -------------------------------------------------------------

test("assets are keyed by the identity triple, not the basename", () => {
  const one = (stack, bytes, request = "fire.tga") =>
    buildCatalog(recordsDoc([record("P_A.ALO", 0, "a", "color", [request, bytes])], { stack })).assets[0];
  const base = one(STACK, "base-bytes"), modded = one(sha("other stack"), "base-bytes"), other = one(STACK, "mod-bytes");
  assert.equal(base.canonicalName, modded.canonicalName);
  assert.notEqual(base.id, modded.id, "the same name and bytes under another stack is another asset");
  assert.notEqual(base.id, other.id, "the same name with other bytes is another asset");
  assert.deepEqual(base.identity, { canonicalName: "FIRE.TGA", sha256: sha("base-bytes"), contentStackHash: STACK });

  const spellings = buildCatalog(recordsDoc([record("P_A.ALO", 0, "a", "color", ["FIRE.TGA", "x"]),
    record("P_B.ALO", 0, "b", "color", ["Fire.tga", "x"])])).assets;
  assert.equal(spellings.length, 1, "two spellings of the same bytes are one asset");
  assert.deepEqual(spellings[0].requests, ["FIRE.TGA", "Fire.tga"]);
});

test("atlas interpretations are counted and a conflict is kept, never collapsed", () => {
  const c = catalog(), fire = asset(c, "P_FIRE.TGA");
  assert.deepEqual(fire.atlas.interpretations.map((i) => [i.side, i.count, i.textureSizes]), [[4, 2, [16]], [1, 1, [1]]]);
  assert.deepEqual(fire.atlas.conflict, ["4x4", "1x1"]);
  assert.deepEqual(fire.atlas.interpretations[0].observedCells, [0, 3, 15]);
  assert.deepEqual(fire.atlas.interpretations[0].outOfGrid, [], "cell 15 is the last of a 4x4 grid");
  assert.deepEqual(asset(c, "FIRE.TGA").atlas.interpretations[0].outOfGrid, [16], "cell 16 is past it");
  assert.deepEqual(fire.atlas.interpretations[1].outOfGrid, [17]);
  assert.equal(asset(c, "W_SMOKE.TGA").atlas.conflict, null);
  assert.deepEqual(c.assets.filter((a) => a.atlas.conflict).map((a) => a.canonicalName).sort(),
    ["P_FIRE.TGA", "P_PARTICLE_DEPTH_MASTER.TGA"], "a normal map shared across layouts conflicts too");
  assert.equal(c.summary.atlasConflicts, 2);
  assert.equal(gridSide(64), 8);
  assert.equal(gridSide(0), 1);
});

test("observed uses collect effects, emitters, blends and orientation", () => {
  const c = catalog(), master = asset(c, "P_PARTICLE_MASTER.TGA");
  assert.equal(master.uses, 9);
  assert.deepEqual(master.blendModes, [{ mode: 1, name: "additive", count: 7 }, { mode: 2, name: "transparent", count: 2 }]);
  assert.equal(master.effects.length, 7);
  assert.deepEqual(asset(c, "P_SHOCKWAVE.DDS").orientation, { world: 1, camera: 0 });
  assert.deepEqual(asset(c, "P_PARTICLE_DEPTH_MASTER.TGA").slots, { normal: 21 });
  assert.deepEqual(c.unresolved, [{ canonicalName: "P_MISSING_EMBERS.TGA", requests: ["p_missing_embers.tga"], uses: 1, effects: [TORCH] }]);
  const torch = c.effects.find((e) => e.file === TORCH);
  assert.deepEqual(torch.emitterNames, ["embers", "fire"], "an emitter whose texture is unresolved is still listed");
  assert.deepEqual(torch.textures, [asset(c, "P_FIRE.TGA").id, asset(c, "P_PARTICLE_DEPTH_MASTER.TGA").id].sort());
  assert.equal(c.content.sha256, STACK);
});

test("suggested uses follow the distribution, each with its share", () => {
  const c = catalog(), says = (name) => asset(c, name).suggestedUses;
  assert.ok(says("P_PARTICLE_MASTER.TGA").includes("additive blending (7 of 9 uses)"), says("P_PARTICLE_MASTER.TGA").join("; "));
  assert.ok(says("P_FIRE.TGA").includes("flipbook on a 4x4 atlas (2 of 2 uses animate the Index track; also used as 1x1)"),
    says("P_FIRE.TGA").join("; "));
  assert.ok(says("P_DEBRIS_ATLAS.TGA").includes("4x4 atlas, one cell per emitter (1 uses)"), says("P_DEBRIS_ATLAS.TGA").join("; "));
  assert.ok(says("P_SHOCKWAVE.DDS").some((u) => u.startsWith("world-oriented quads")));
  assert.ok(!says("W_SMOKE.TGA").some((u) => u.startsWith("world-oriented")));
  assert.ok(says("P_PARTICLE_DEPTH_MASTER.TGA")[0].startsWith("normal map (21 of 21 uses)"));
  assert.ok(!says("W_SMOKE.TGA").some((u) => u.startsWith("normal map")));
});

test("a suggestion needs a majority, a grid, or a name more than one use shares", () => {
  const t = ["t.tga", "t"];
  const a = buildCatalog(recordsDoc([record("P_GLOW_A.ALO", 0, "glow core", "color", t, { world: true }),
    record("P_GLOW_B.ALO", 0, "glow halo", "color", t, { blendMode: 2 }),
    record("P_PUFF.ALO", 0, "puff", "normal", t, { blendMode: 3 }),
    record("P_PUFF.ALO", 1, "wisp", "normal", t, { blendMode: 4 })])).assets[0];
  assert.deepEqual(a.suggestedUses, ["glow (2 emitter or effect names)", "puff (2 emitter or effect names)"],
    "half the uses normal, one blend each, one world quad and a 1x1 grid suggest nothing");
  assert.deepEqual(a.useTerms.map((u) => [u.term, u.count]), [["glow", 2], ["puff", 2], ["core", 1], ["halo", 1], ["wisp", 1]],
    "effect names count, once a use");
});

test("loaderDefault is true only when every use was a loader-default normal map", () => {
  const depth = ["p_particle_depth_master.tga", "d"];
  const flag = (...flags) => buildCatalog(recordsDoc(flags.map((f, i) => record(`P_${i}.ALO`, 0, "e", "normal", depth, { loaderDefault: f }))))
    .assets[0].loaderDefault;
  assert.equal(flag(true, true), true);
  assert.equal(flag(true, false), false);
});

test("the catalog refuses what is not an index document, or one the tool could not vouch for", () => {
  assert.throws(() => buildCatalog({ ...recordsDoc(stockRecords()), verb: "inspect" }), { exitCode: 2 });
  assert.throws(() => buildCatalog({ ...recordsDoc(stockRecords()), schema: 2 }), { exitCode: 2 });
  assert.throws(() => buildCatalog(recordsDoc([], { unverifiable: "short read: X" })), { exitCode: 3 });
});

// ---- search: the acceptance requests (§25) ---------------------------------

// Each request ranks at least two candidates; the expected ones come first, in order.
const ACCEPTANCE = [
  ["filename: exact stem, then prefix, then substring", { filename: "fire" }, ["FIRE.TGA", "FIRE_RING.TGA", "P_FIRE.TGA"]],
  ["emitter, case-insensitive: all uses before some", { emitter: "SPARK" }, ["P_SPARK_ATLAS.DDS", "P_PARTICLE_MASTER.TGA"]],
  ["effect", { effect: "med01" }, ["P_SPARK_ATLAS.DDS", "P_FIRE.TGA"]],
  ["blend: share plus volume, so 3 of 3, then 7 of 9 above 2 of 2", { blend: "additive" }, ["P_FIRE.TGA", "P_PARTICLE_MASTER.TGA", "P_SPARK_ATLAS.DDS"]],
  ["observed use: every use in smoke before a third", { use: "smoke" }, ["W_SMOKE.TGA", "P_DEBRIS_ATLAS.TGA", "P_PARTICLE_MASTER.TGA"]],
  ["observed use, a texture not named for it: 6 of 9 just outranks 1 of 1", { use: "debris" }, ["P_PARTICLE_MASTER.TGA", "P_DEBRIS_ATLAS.TGA"]],
  ["two facets: every use matching both before two of three", { blend: "additive", use: "fire" }, ["FIRE.TGA", "P_FIRE.TGA"]],
];
for (const [name, query, expected] of ACCEPTANCE)
  test(`acceptance: ${name}`, () => {
    const got = names(search(catalog(), query));
    assert.deepEqual(got.slice(0, expected.length), expected, got.join(", "));
    assert.ok(got.length >= 2, `a ranking needs a competitor: ${got.join(", ")}`);
  });

test("a use word can name how a use is drawn: blend, atlas, flipbook, world-oriented", () => {
  const c = catalog();
  assert.deepEqual(names(search(c, { use: "world-oriented" })), ["P_SHOCKWAVE.DDS"]);
  assert.deepEqual(names(search(c, { use: "flipbook" })), ["W_SMOKE.TGA", "P_SPARK_ATLAS.DDS", "P_PARTICLE_MASTER.TGA", "P_FIRE.TGA"],
    "one cell per emitter is an atlas, not a flipbook");
  assert.ok(names(search(c, { use: "atlas" })).includes("P_DEBRIS_ATLAS.TGA"));
  assert.match(search(c, { use: "transparent smoke" })[0].why[0], /\(3 of 3 uses/);
});

test("a use is counted in the slot searched, never borrowed from another", () => {
  const t = ["t.tga", "t"];
  const c = buildCatalog(recordsDoc([record("P_A.ALO", 0, "flame", "color", t),
    record("P_B.ALO", 0, "smoke", "normal", t), record("P_C.ALO", 0, "smoke", "normal", t)]));
  assert.deepEqual(search(c, { use: "smoke" }), [], "no colour use is named smoke");
  assert.match(search(c, { use: "smoke", slot: "any" })[0].why[0], /\(2 of 3 uses/);
});

test("a blend is the same query by name or by mode, and only 0-13", () => {
  assert.deepEqual(search(catalog(), { blend: "1" }), search(catalog(), { blend: "additive" }));
  assert.throws(() => search(catalog(), { blend: "99" }), { exitCode: 2 });
  assert.throws(() => search(catalog(), { blend: "glow" }), { exitCode: 2 });
});

test("every facet must match, and every word of a use", () => {
  const c = catalog();
  assert.deepEqual(search(c, { blend: "transparent", use: "shockwave" }), []);
  assert.ok(!names(search(c, { emitter: "spark" })).includes("W_SMOKE.TGA"));
  assert.deepEqual(names(search(c, { use: "smoke lit" })), ["P_PARTICLE_MASTER.TGA"]);
  assert.match(search(c, { use: "smoke lit" })[0].why[0], /\(1 of 9 uses/, "the word the fewest uses carry scores");
});

test("a use word matches names, not the share text, and not inside a longer word", () => {
  const c = catalog();
  for (const word of ["uses", "emitters", "track", "medallion", "litter", "cell", "per", "one", "blending", "ark", "plosion", "explosionsss"])
    assert.deepEqual(search(c, { use: word }), [], word);
  const explosion = names(search(c, { use: "explosion" }));
  assert.ok(explosion.length >= 3);
  assert.deepEqual(names(search(c, { use: "explo" })), explosion, "a word finds the names it begins");
  assert.deepEqual(names(search(c, { use: "explosions" })), explosion, "a plural finds the singular");
  assert.ok(names(search(c, { use: "sparks" })).includes("P_PARTICLE_MASTER.TGA"), "the master's only spark emitter is 'spark glints'");
  assert.throws(() => search(c, { use: "fx" }), { exitCode: 2 });
});

test("usage counts the colour slot unless asked; a loader-default normal map ranks a quarter as high", () => {
  const c = catalog();
  assert.ok(!names(search(c, { use: "smoke" })).includes("P_PARTICLE_DEPTH_MASTER.TGA"), "a normal map is not what an emitter looks like");
  assert.equal(names(search(c, { use: "smoke", slot: "normal" }))[0], "P_PARTICLE_DEPTH_MASTER.TGA");
  const any = search(c, { emitter: "debris", slot: "any" });
  const master = any.find((r) => r.canonicalName === "P_PARTICLE_MASTER.TGA");
  const depth = any.find((r) => r.canonicalName === "P_PARTICLE_DEPTH_MASTER.TGA");
  assert.ok(master && depth && depth.score < master.score / 2, `${depth?.score} vs ${master?.score}`);
  const twins = buildCatalog(recordsDoc([record("P_A.ALO", 0, "glow", "normal", ["n1.tga", "a"], { loaderDefault: true }),
    record("P_B.ALO", 0, "glow", "normal", ["n2.tga", "b"])]));
  const [plain, loader] = search(twins, { emitter: "glow", slot: "normal" });
  assert.equal(loader.canonicalName, "N1.TGA");
  assert.ok(Math.abs(loader.score / plain.score - 0.25) < 1e-3, `${loader.score} / ${plain.score}`);
  assert.throws(() => search(c, { use: "smoke", slot: "diffuse" }), { exitCode: 2 });
  assert.throws(() => search(c, {}), { exitCode: 2 });
});

// ---- recipes ---------------------------------------------------------------

function sparkRecipe(c, overrides = {}) {
  const spark = asset(c, "P_SPARK_ATLAS.DDS");
  const effect = c.effects.find((e) => e.file.endsWith("P_EXPLOSION_MED01.ALO"));
  return { schema: 1, id: "medium-explosion-sparks", type: "layer-group", title: "Medium explosion sparks",
    description: "Two additive spark layers, flipbook animated", tags: ["sparks", "explosion", "fireworks"],
    source: { effect: effect.file, sha256: effect.sha256, contentStackHash: c.content.sha256 },
    emitters: ["sparks", "spark trails"], dependencies: [{ kind: "texture", name: "p_spark_atlas.dds" }],
    expectedTextures: [{ canonicalName: spark.canonicalName, sha256: spark.identity.sha256 }],
    safeTransforms: [{ field: "lifetime", min: 0.2, max: 2 }], ...overrides };
}

test("the validator accepts exactly the fields recipe.schema.json lists", () => {
  const schema = JSON.parse(fs.readFileSync(join(here, "recipe.schema.json"), "utf8"));
  const keys = (o) => Object.keys(o.properties).sort();
  assert.deepEqual([...FIELDS.recipe].sort(), keys(schema));
  assert.deepEqual([...FIELDS.source].sort(), keys(schema.properties.source));
  assert.deepEqual([...FIELDS.dependency].sort(), keys(schema.properties.dependencies.items));
  assert.deepEqual([...FIELDS.expectedTexture].sort(), keys(schema.properties.expectedTextures.items));
  assert.deepEqual([...FIELDS.safeTransform].sort(), keys(schema.properties.safeTransforms.items));
  assert.deepEqual([...schema.required].sort(), ["dependencies", "expectedTextures", "id", "safeTransforms", "schema", "source", "title", "type"]);
  const c = catalog();
  assert.deepEqual(validateRecipe(sparkRecipe(c), { catalog: c }), []);
});

test("register-recipe rejects a recipe missing any required part, or malformed", () => {
  const c = catalog();
  for (const field of ["type", "dependencies", "source", "expectedTextures", "safeTransforms", "id", "title", "schema"]) {
    const r = sparkRecipe(c);
    delete r[field];
    assert.ok(validateRecipe(r).some((e) => e.startsWith(`recipe.${field}`)), `missing ${field}`);
  }
  const source = sparkRecipe(c).source, texture = sparkRecipe(c).expectedTextures[0];
  const bad = (overrides, prefix) => assert.ok(validateRecipe(sparkRecipe(c, overrides)).some((e) => e.startsWith(prefix)),
    `${JSON.stringify(overrides)} → ${prefix}`);
  bad({ type: "mesh" }, "recipe.type");
  bad({ id: "Bad Id" }, "recipe.id");
  bad({ source: { ...source, sha256: "nothex" } }, "recipe.source.sha256");
  bad({ source: { ...source, contentStackHash: "ABC" } }, "recipe.source.contentStackHash");
  bad({ source: { ...source, extra: 1 } }, "recipe.source.extra");
  bad({ expectedTextures: [] }, "recipe.expectedTextures");
  bad({ expectedTextures: [{ ...texture, sha256: texture.sha256.toUpperCase() }] }, "recipe.expectedTextures[0].sha256");
  bad({ safeTransforms: [{ field: "lifetime", min: 3, max: 1 }] }, "recipe.safeTransforms[0]");
  bad({ safeTransforms: [{ field: "lifetime", min: Infinity }] }, "recipe.safeTransforms[0].min");
  bad({ dependencies: [{ kind: "sound", name: "x" }] }, "recipe.dependencies[0].kind");
  bad({ dependencies: [{ kind: "texture" }] }, "recipe.dependencies[0].name");
  bad({ tags: Array.from({ length: 21 }, (_, i) => `t${i}`) }, "recipe.tags");
  bad({ tags: ["x".repeat(41)] }, "recipe.tags");
  bad({ extra: true }, "recipe.extra");
  bad({ schema: 2 }, "recipe.schema");
  bad({ id: "a".repeat(81) }, "recipe.id");
  bad({ title: "x".repeat(121) }, "recipe.title");
  bad({ description: "x".repeat(2001) }, "recipe.description");
  bad({ emitters: ["sparks", 7] }, "recipe.emitters");
  bad({ source: { sha256: source.sha256, contentStackHash: source.contentStackHash } }, "recipe.source.effect");
  bad({ expectedTextures: [{ ...texture, extra: 1 }] }, "recipe.expectedTextures[0].extra");
  bad({ dependencies: [{ kind: "texture", name: "x", extra: 1 }] }, "recipe.dependencies[0].extra");
  bad({ safeTransforms: [{ field: "lifetime", extra: 1 }] }, "recipe.safeTransforms[0].extra");
  bad({ safeTransforms: [{ min: 1 }] }, "recipe.safeTransforms[0].field");
  bad({ safeTransforms: [{ field: "lifetime", note: "x".repeat(401) }] }, "recipe.safeTransforms[0].note");
});

test("each recipe type names the emitters it needs", () => {
  const c = catalog();
  const bad = (type, emitters) => assert.ok(validateRecipe(sparkRecipe(c, { type, emitters })).some((e) => e.startsWith("recipe.emitters")),
    `${type} with ${emitters.length} emitters`);
  bad("single-emitter", ["sparks", "spark trails"]);
  bad("single-emitter", []);
  bad("layer-group", ["sparks"]);
  bad("subtree", []);
  bad("template", ["sparks"]);
  for (const [type, emitters] of [["single-emitter", ["sparks"]], ["subtree", ["sparks"]], ["subtree", ["sparks", "spark trails"]], ["layer-group", ["sparks", "fire"]], ["template", null]]) {
    const r = sparkRecipe(c, { type });
    if (emitters) r.emitters = emitters; else delete r.emitters;
    assert.deepEqual(validateRecipe(r, { catalog: c }), [], type);
  }
});

test("register-recipe checks the recipe against the catalog's stack and its own effect", () => {
  const c = catalog(), source = sparkRecipe(c).source;
  const errors = (overrides) => validateRecipe(sparkRecipe(c, overrides), { catalog: c });
  assert.match(errors({ source: { ...source, contentStackHash: sha("other") } })[0], /covers stack/);
  assert.match(errors({ source: { ...source, sha256: sha("other") } })[0], /hashes to/);
  assert.match(errors({ source: { ...source, effect: "DATA\\ART\\MODELS\\P_NONE.ALO" } })[0], /not in the catalog/);
  assert.match(errors({ emitters: ["sparks", "ghost"] })[0], /ghost/);
  const smoke = asset(c, "W_SMOKE.TGA");
  assert.match(errors({ expectedTextures: [{ canonicalName: smoke.canonicalName, sha256: smoke.identity.sha256 }] })[0],
    /does not use W_SMOKE\.TGA/, "a texture another effect uses is not this recipe's");
  assert.match(errors({ expectedTextures: [{ canonicalName: "P_SPARK_ATLAS.DDS", sha256: sha("other") }] })[0], /does not use/);

  const torch = c.effects.find((e) => e.file === TORCH), fire = asset(c, "P_FIRE.TGA");
  const embers = { schema: 1, id: "torch-embers", type: "single-emitter", title: "Torch embers",
    source: { effect: TORCH, sha256: torch.sha256, contentStackHash: STACK }, emitters: ["embers"], dependencies: [],
    expectedTextures: [{ canonicalName: fire.canonicalName, sha256: fire.identity.sha256 }], safeTransforms: [] };
  assert.deepEqual(validateRecipe(embers, { catalog: c }), [], "an emitter whose texture is unresolved can be a recipe");
});

test("the registry keeps one recipe per id, sorted, and a rejection leaves it untouched", () => {
  const dir = scratch(), registry = join(dir, "recipes.json"), c = catalog();
  try {
    assert.equal(registerRecipe(registry, sparkRecipe(c), { catalog: c }).registered, true);
    assert.equal(registerRecipe(registry, sparkRecipe(c, { id: "a-first" }), { catalog: c }).registered, true);
    const before = fs.readFileSync(registry, "utf8");
    const again = registerRecipe(registry, sparkRecipe(c), { catalog: c });
    assert.equal(again.registered, false);
    assert.match(again.errors[0], /already registered/);
    assert.equal(registerRecipe(registry, sparkRecipe(c, { id: "Bad Id" })).registered, false);
    assert.equal(fs.readFileSync(registry, "utf8"), before);
    assert.deepEqual(JSON.parse(before).recipes.map((r) => r.id), ["a-first", "medium-explosion-sparks"]);
    const other = join(dir, "other.json");
    fs.writeFileSync(other, JSON.stringify({ schema: 2, recipes: [] }));
    assert.match(registerRecipe(other, sparkRecipe(c), { catalog: c }).errors[0], /not a recipe registry/);
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test("search surfaces registered recipes beside assets, on every word and facet", () => {
  const c = catalog(), recipes = [sparkRecipe(c)];
  const ids = (query) => search(c, query, { recipes }).filter((r) => r.kind === "recipe").map((r) => r.id);
  assert.deepEqual(ids({ use: "spark layers" }), ["medium-explosion-sparks"]);
  assert.deepEqual(ids({ use: "spark shimmer" }), [], "every word must describe the recipe");
  assert.deepEqual(ids({ filename: "spark_atlas" }), ["medium-explosion-sparks"]);
  assert.deepEqual(ids({ blend: "additive", emitter: "spark" }), ["medium-explosion-sparks"]);
  assert.deepEqual(ids({ blend: "transparent", emitter: "spark" }), []);
  assert.deepEqual(ids({ use: "fireworks" }), ["medium-explosion-sparks"], "tags describe the recipe");
  assert.deepEqual(ids({ emitter: "SPARK TR" }), ["medium-explosion-sparks"]);
  assert.deepEqual(ids({ emitter: "smoke" }), []);
  assert.deepEqual(ids({ effect: "med01" }), ["medium-explosion-sparks"]);
  assert.deepEqual(ids({ effect: "torch" }), []);
  assert.deepEqual(ids({ filename: "p_fire" }), []);
  const mixed = search(c, { emitter: "spark" }, { recipes });
  assert.equal(mixed[0].canonicalName, "P_SPARK_ATLAS.DDS", "an asset all of whose uses match outranks a recipe");
});

// ---- the CLIs ----------------------------------------------------------------

test("build: records to catalog, or --tool index first with layers before roots", () => {
  const dir = scratch();
  try {
    const records = join(dir, "records.json"), out = join(dir, "catalog.json");
    fs.writeFileSync(records, JSON.stringify(recordsDoc(stockRecords())));
    buildMain(["--records", records, "--out", out], { log: quiet });
    assert.equal(JSON.parse(fs.readFileSync(out, "utf8")).summary.assets, 9);

    const calls = [];
    const run = (bin, args) => {
      calls.push([bin, ...args]);
      fs.writeFileSync(args[3], JSON.stringify(recordsDoc(stockRecords())));
      return { status: 0, stderr: "[vfx-index] models=1\n" };
    };
    buildMain(["--exe", "pe.exe", "--game-root", "D:/g/corruption", "--game-root", "D:/g/GameData", "--layer", "D:/m/A",
      "--out", out], { run, log: quiet });
    assert.deepEqual(calls[0], ["pe.exe", "--tool", "index", "--out", join(dir, "catalog.records.json"), "--layer", "D:/m/A",
      "D:/g/corruption", "D:/g/GameData"]);

    assert.throws(() => buildMain(["--exe", "pe.exe", "--game-root", "D:/g", "--out", out],
      { run: () => ({ status: 3, stderr: "[tool] verb=index exit=3 no game root lists a MEG" }), log: quiet }),
      (e) => e.exitCode === 3 && /lists a MEG/.test(e.message));
    assert.throws(() => buildMain(["--out", out], { log: quiet }), { exitCode: 2 });
    assert.throws(() => buildMain(["--records", records, "--layer", "x", "--out", out], { log: quiet }), { exitCode: 2 });
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test("search and register-recipe CLIs", () => {
  const dir = scratch();
  try {
    const cat = join(dir, "catalog.json"), registry = join(dir, "recipes.json"), recipe = join(dir, "recipe.json");
    const c = catalog();
    fs.writeFileSync(cat, JSON.stringify(c));
    fs.writeFileSync(recipe, JSON.stringify(sparkRecipe(c)));
    recipesMain(["register-recipe", "--registry", registry, "--recipe", recipe, "--catalog", cat], { log: quiet });
    const lines = [];
    const results = searchMain(["--catalog", cat, "--recipes", registry, "--emitter", "spark", "--limit", "2"], { log: (l) => lines.push(l) });
    assert.equal(results.length, 2);
    assert.match(lines[0], /^1\. /);
    assert.throws(() => recipesMain(["register-recipe", "--registry", registry, "--recipe", recipe], { log: quiet }), { exitCode: 5 });
    assert.throws(() => searchMain(["--catalog", cat], { log: quiet }), { exitCode: 2 });
    assert.throws(() => searchMain(["--catalog", cat, "--recipes", join(dir, "missing.json"), "--use", "smoke"], { log: quiet }),
      /ENOENT/, "a mistyped registry path is an error, not an empty registry");
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

// ---- local only (§24.3) ---------------------------------------------------------

test("no network: the index scripts name no network module or API and make no connection", () => {
  const NETWORK = /(["'])(node:)?(https?|http2|net|tls|dns|dgram|undici)\1|\bfetch\b|WebSocket|XMLHttpRequest|EventSource/;
  for (const file of fs.readdirSync(here).filter((f) => f.endsWith(".mjs") && !f.endsWith(".test.mjs")))
    assert.doesNotMatch(fs.readFileSync(join(here, file), "utf8"), NETWORK, file);
  for (const probe of ['import * as http from "http";', 'await import("node:https")', 'globalThis["fetch"](u)', 'require("net")'])
    assert.match(probe, NETWORK, `the guard catches ${probe}`);

  // The CommonJS module objects are the mutable ones every importer shares.
  const require = createRequire(import.meta.url);
  const net = require("node:net"), http = require("node:http"), https = require("node:https"), dns = require("node:dns");
  const attempts = [];
  const trap = (name) => () => { attempts.push(name); throw new Error(`network: ${name}`); };
  const saved = { connect: net.Socket.prototype.connect, http: http.request, https: https.request, lookup: dns.lookup, fetch: globalThis.fetch };
  net.Socket.prototype.connect = trap("socket");
  http.request = trap("http"); https.request = trap("https"); dns.lookup = trap("dns"); globalThis.fetch = trap("fetch");
  const dir = scratch();
  try {
    const records = join(dir, "records.json"), out = join(dir, "catalog.json"), registry = join(dir, "recipes.json"), recipe = join(dir, "recipe.json");
    fs.writeFileSync(records, JSON.stringify(recordsDoc(stockRecords())));
    buildMain(["--records", records, "--out", out], { log: quiet });
    fs.writeFileSync(recipe, JSON.stringify(sparkRecipe(JSON.parse(fs.readFileSync(out, "utf8")))));
    recipesMain(["register-recipe", "--registry", registry, "--recipe", recipe, "--catalog", out], { log: quiet });
    searchMain(["--catalog", out, "--recipes", registry, "--use", "smoke"], { log: quiet });
  } finally {
    net.Socket.prototype.connect = saved.connect;
    http.request = saved.http; https.request = saved.https; dns.lookup = saved.lookup; globalThis.fetch = saved.fetch;
    fs.rmSync(dir, { recursive: true, force: true });
  }
  assert.deepEqual(attempts, []);
});
