import { test } from "node:test";
import assert from "node:assert/strict";
import { admitBase, admitRecipe, checkCorpus, checkRecipe, checkRoots, coveringRoots } from "./corpus.mjs";
import { inspectDiff, patchRequest, rolePath, verifyRecipe } from "./recipe.mjs";
import { caseIntent, checkTargets, freezeVariant, parseTargets } from "./freeze.mjs";
import { persistedText } from "./persisted.mjs";
import { RUBRIC } from "./schema-check.mjs";

const f = Math.fround;
const clone = (v) => structuredClone(v);
const group = (type = 0, minY = 0, maxY = 0) => ({ type, minX: 0, maxX: 0, minY, maxY, minZ: 0, maxZ: 0, sideLength: 0, sphereRadius: 0,
  sphereEdge: 0, cylinderRadius: 0, cylinderEdge: 0, cylinderHeight: 0, valX: 0, valY: 0, valZ: 0 });
const track = (keys = [[0, 1], [100, 1]], interpolation = 0) => ({ source: 0, explicitUnlock: false, interpolation, keys });
function emitter(index, name, over = {}) {
  const lifetime = over.lifetime ?? 1;
  return { index, name, parent: null, spawnDuringLife: null, spawnOnDeath: null, linkGroup: 0, colorTexture: `${name}.tga`, normalTexture: "",
    lifetime, initialDelay: 0, randomLifetimePerc: 0, randomScalePerc: 0, randomColors: [0, 0, 0, 0], randomRotation: false,
    randomRotationDirection: false, isWeatherParticle: false, useBursts: true, nParticlesPerBurst: 10, nParticlesPerSecond: 5, skipTime: 0,
    textureSize: 16, blendMode: 2, groups: [group(), group(1, lifetime, lifetime), group()],
    tracks: [track(), track(), track(), track(), track([[0, 20], [100, 20]]), track([[0, 0], [25, 1], [50, 2], [75, 3], [100, 3]], 2), track([[0, 0], [100, 0]])],
    ...over };
}
// Fireball (root) with a during-life Smoke child; Sparks root with a death child Embers.
function base() {
  const e = [emitter(0, "Fireball", { lifetime: 4, groups: [group(), group(1, 4, 4), group()] }), emitter(1, "Smoke", { initialDelay: f(0.2) }),
    emitter(2, "Sparks"), emitter(3, "Embers")];
  e[0].spawnDuringLife = 1; e[1].parent = 0; e[2].spawnOnDeath = 3; e[3].parent = 2;
  return { schema: 1, name: "effect", leaveParticles: true, linkExempts: [], sourceDocumentHash: "a".repeat(64), emitters: e };
}

test("inspectDiff ignores the source hash and names every differing path", () => {
  const a = base(), b = clone(a);
  b.sourceDocumentHash = "b".repeat(64);
  assert.deepEqual(inspectDiff(a, b), []);
  b.emitters[0].lifetime = 2;
  b.emitters[1].tracks[5].keys.push([100, 3]);
  assert.deepEqual(inspectDiff(a, b), [".emitters[0].lifetime", ".emitters[1].tracks[5].keys.length"]);
});

test("rolePath walks the spawn tree; patchRequest selects the target by it", () => {
  const b = base();
  assert.deepEqual(rolePath(b, 1), [{ role: "root", name: "Fireball" }, { role: "lifeChild", name: "Smoke" }]);
  assert.deepEqual(rolePath(b, 3), [{ role: "root", name: "Sparks" }, { role: "deathChild", name: "Embers" }]);
  const r = patchRequest({ target: { index: 1, name: "Smoke" }, mutations: [{ op: "setProperty", field: "initialDelay", value: 0.6 }] }, b);
  assert.equal(r.selector.sourceDocumentHash, b.sourceDocumentHash);
  assert.deepEqual(r.selector.expected, { name: "Smoke" });
});

test("verifyRecipe: a field case must equal the patched base", () => {
  const b = base(), c = clone(b);
  c.emitters[0].lifetime = 2; c.emitters[0].groups[1].minY = 2; c.emitters[0].groups[1].maxY = 2;
  const recipe = { kind: "field" };
  assert.deepEqual(verifyRecipe({ recipe, baseInspect: b, caseInspect: c, patchedInspect: clone(c) }), []);
  const other = clone(c); other.emitters[0].groups[1].minY = 3;
  assert.match(verifyRecipe({ recipe, baseInspect: b, caseInspect: c, patchedInspect: other }).join(), /minY differs/);
  assert.match(verifyRecipe({ recipe, baseInspect: b, caseInspect: clone(b), patchedInspect: clone(b) }).join(), /equals its base/);
  assert.match(verifyRecipe({ recipe, baseInspect: b, caseInspect: c }).join(), /needs the patched/);
});

function swapped() {
  // Two childless roots A, B swapped by the root reorder: positions and indices follow.
  const b = { schema: 1, name: "effect", leaveParticles: true, linkExempts: [], sourceDocumentHash: "a".repeat(64), emitters: [emitter(0, "A"), emitter(1, "B", { blendMode: 1 })] };
  const c = clone(b);
  c.emitters = [{ ...clone(b.emitters[1]), index: 0 }, { ...clone(b.emitters[0]), index: 1 }];
  // The relation names case identities: B now sits at index 0.
  return { b, c, recipe: { kind: "permutation", map: [1, 0], relation: { front: { index: 0, name: "B" }, back: { index: 1, name: "A" } } } };
}

test("verifyRecipe: a permutation allows exactly the derived index and link changes", () => {
  const { b, c, recipe } = swapped();
  assert.deepEqual(verifyRecipe({ recipe, baseInspect: b, caseInspect: c }), []);
  const payload = clone(c); payload.emitters[0].lifetime = 2;
  assert.match(verifyRecipe({ recipe, baseInspect: b, caseInspect: payload }).join(), /lifetime differs/);
  const index = clone(c); index.emitters[0].index = 1;
  assert.match(verifyRecipe({ recipe, baseInspect: b, caseInspect: index }).join(), /derived/);
  assert.match(verifyRecipe({ recipe: { ...recipe, map: [0, 1] }, baseInspect: b, caseInspect: b }).join(), /identity/);
  assert.match(verifyRecipe({ recipe: { ...recipe, map: [0, 0] }, baseInspect: b, caseInspect: c }).join(), /bijection/);
});

test("admitRecipe: the draw-order pair are childless roots, named as the case names them", () => {
  const { b, c, recipe } = swapped();
  assert.deepEqual(admitRecipe(recipe, b, c), []);
  assert.match(admitRecipe({ ...recipe, relation: { front: { index: 1, name: "B" }, back: { index: 0, name: "A" } } }, b, c).join(), /no emitter/);
  const parented = clone(c); parented.emitters[0].spawnOnDeath = 1; parented.emitters[1].parent = 0;
  assert.match(admitRecipe(recipe, b, parented).join(), /childless root/);
});

test("verifyRecipe: seed and control cases have an empty inspect diff", () => {
  const b = base();
  assert.deepEqual(verifyRecipe({ recipe: { kind: "none" }, baseInspect: b, caseInspect: clone(b) }), []);
  const c = clone(b); c.emitters[2].lifetime = 3;
  assert.match(verifyRecipe({ recipe: { kind: "seed" }, baseInspect: b, caseInspect: c }).join(), /differs from the base/);
});

test("roots: never under or around the checkout; stores, runs and CLI apart; content may nest", () => {
  const repo = "C:\\Work\\editor-repo";
  const ok = { repo, stores: "C:\\Work\\vfx-benchmark-stores", runs: "C:\\Work\\vfx-benchmark-runs", cli: "C:\\Work\\vfx-benchmark-cli",
    content: ["D:\\Game\\Mods\\Mod", "D:\\Game"] };
  assert.deepEqual(checkRoots(ok), []);
  assert.match(checkRoots({ ...ok, runs: "C:\\Work\\editor-repo\\runs" }).join(), /under the repository/);
  assert.match(checkRoots({ ...ok, content: ["C:\\Work"] }).join(), /encloses the repository/);
  assert.match(checkRoots({ ...ok, runs: "C:/Work/vfx-benchmark-stores/runs" }).join(), /overlap/);
  assert.match(checkRoots({ ...ok, content: ["C:\\Work\\vfx-benchmark-stores\\x"] }).join(), /overlaps the stores/);
  assert.match(checkRoots({ ...ok, content: ["Game"] }).join(), /not absolute/);
  assert.deepEqual(coveringRoots(ok.content), ["D:\\Game"]);
});

function corpus() {
  const cases = RUBRIC.classes.filter((c) => !c.optional).map((c, i) => ({ id: `k${i}`, base: "A", kind: "defect", shape: "single", class: c.id }));
  cases.push({ id: "c0", base: "A", kind: "control", shape: "single" });
  return { schemaVersion: 1, version: "corpus-v1", state: "draft", provenance: { exeSha256: "e".repeat(64), contentRoots: ["D:\\Game"] },
    targets: { objective: 80, subjective: 65, controlFlag: 15, fdp: 30, completion: 90 }, attempts: 3,
    bases: [{ id: "A" }], cases, requiredClasses: RUBRIC.classes.filter((c) => !c.optional).map((c) => c.id), seedAbsence: "b" };
}

test("checkCorpus refuses unknown keys, missing strata and unpaired seed cases", () => {
  assert.deepEqual(checkCorpus(corpus()), []);
  assert.match(checkCorpus({ ...corpus(), extra: 1 }).join(), /unknown key extra/);
  const noControl = corpus(); noControl.cases = noControl.cases.filter((c) => c.kind !== "control");
  assert.match(checkCorpus(noControl).join(), /no single-bundle control/);
  const missing = corpus(); missing.cases = missing.cases.filter((c) => c.class !== "draw-order");
  assert.match(checkCorpus(missing).join(), /draw-order has no retained case/);
  assert.deepEqual(checkCorpus({ ...missing, reducedClassList: ["draw-order"] }), []);
  const seed = corpus(); seed.cases.push({ id: "s", base: "A", kind: "defect", shape: "three", class: "seed" });
  assert.match(checkCorpus(seed).join(), /retained or dropped together/);
  assert.match(checkCorpus({ ...corpus(), seedAbsence: undefined }).join(), /absence reason/);
  assert.match(checkCorpus({ ...corpus(), attempts: 2 }).join(), /three scheduled attempts/);
});

test("checkRecipe refuses renames and incomplete recipes", () => {
  assert.deepEqual(checkRecipe({ schemaVersion: 1, base: "A", kind: "field", target: { index: 0, name: "x" }, mutations: [{ op: "setProperty" }] }), []);
  assert.match(checkRecipe({ schemaVersion: 1, base: "A", kind: "field", target: {}, mutations: [{ op: "rename" }] }).join(), /renames/);
  assert.match(checkRecipe({ schemaVersion: 1, base: "A", kind: "permutation" }).join(), /map and relation/);
  assert.match(checkRecipe({ schemaVersion: 1, base: "A", kind: "field", answer: 1 }).join(), /unknown key answer/);
});

test("a decoy is a permutation with a map and no class or relation, verified and admitted without one", () => {
  const decoy = { schemaVersion: 1, base: "A", kind: "permutation", map: [1, 0], decoy: true };
  assert.deepEqual(checkRecipe(decoy), []);
  assert.match(checkRecipe({ ...decoy, map: undefined }).join(), /map and relation/);
  assert.match(checkRecipe({ ...decoy, class: "draw-order" }).join(), /decoy is a permutation with no class or relation/);
  assert.match(checkRecipe({ ...decoy, relation: { front: {}, back: {} } }).join(), /decoy is a permutation with no class or relation/);
  assert.match(checkRecipe({ ...decoy, kind: "field", target: {}, mutations: [{}] }).join(), /decoy is a permutation/);
  assert.match(checkRecipe({ ...decoy, decoy: false }).join(), /decoy is a permutation/);
  const { b, c, recipe } = swapped();
  const asDecoy = { kind: "permutation", map: recipe.map, decoy: true };
  assert.deepEqual(verifyRecipe({ recipe: asDecoy, baseInspect: b, caseInspect: c }), []);
  assert.deepEqual(admitRecipe(asDecoy, b, c), []);
  assert.match(verifyRecipe({ recipe: { kind: "permutation", map: recipe.map }, baseInspect: b, caseInspect: c }).join(), /relation/);
  // In the corpus a decoy is a single-bundle control.
  const withDecoy = corpus(); withDecoy.cases.push({ id: "d0", base: "A", kind: "control", shape: "single", decoy: true });
  assert.deepEqual(checkCorpus(withDecoy), []);
  withDecoy.cases.at(-1).kind = "defect";
  assert.match(checkCorpus(withDecoy).join(), /a decoy is a single-bundle control/);
});

test("admitBase: empty colour textures, seed and draw-order rules", () => {
  const b = base();
  assert.deepEqual(admitBase(b), []);
  b.emitters[2].colorTexture = "";
  assert.match(admitBase(b).join(), /empty colorTexture/);
  assert.match(admitBase(base(), { seedBase: true }).join(), /randomLifetimePerc/);
  const d = base();
  d.emitters[0].groups[2].type = 3;
  d.emitters[1].randomColors = [0, 0.1, 0, 0];
  d.emitters[2].isWeatherParticle = true;
  d.emitters[3].linkGroup = 5;
  const errors = admitBase(d, { drawOrderBase: true }).join("\n");
  for (const re of [/non-exact/, /random fields/, /weather/, /linked/]) assert.match(errors, re);
});

test("admitRecipe: link, rename, single emitter and magnitude rules", () => {
  const b = base();
  const short = (lifetime) => { const c = clone(b); c.emitters[0].lifetime = lifetime; return c; };
  const recipe = { kind: "field", class: "short-lifetime", target: { index: 0, name: "Fireball" } };
  assert.deepEqual(admitRecipe(recipe, b, short(1.6)), []);
  assert.match(admitRecipe(recipe, b, short(2.4)).join(), /outside 0.3-0.5/);
  const linked = clone(b); linked.emitters[0].linkGroup = 2;
  assert.match(admitRecipe(recipe, linked, short(1.6)).join(), /linked/);
  const renamed = short(1.6); renamed.emitters[2].name = "Sparkz";
  assert.match(admitRecipe(recipe, b, renamed).join(), /renames/);
  const two = short(1.6); two.emitters[2].lifetime = 9;
  assert.match(admitRecipe(recipe, b, two).join(), /more than one emitter/);
});

test("admitRecipe: count recipes edit the live field and never a weather emitter", () => {
  const b = base();
  const recipe = { kind: "field", class: "particle-count", target: { index: 2, name: "Sparks" } };
  const burst = clone(b); burst.emitters[2].nParticlesPerBurst = 80;
  assert.deepEqual(admitRecipe(recipe, b, burst), []);
  const rate = clone(b); rate.emitters[2].nParticlesPerSecond = 40;
  assert.match(admitRecipe(recipe, b, rate).join(), /must edit nParticlesPerBurst/);
  const weather = clone(b); weather.emitters[2].isWeatherParticle = true;
  const wc = clone(weather); wc.emitters[2].nParticlesPerBurst = 80;
  assert.match(admitRecipe(recipe, weather, wc).join(), /weather/);
});

test("admitRecipe: delay targets are never death children, and a child's delay stays inside the parent's float32 minimum", () => {
  const b = base();
  const death = clone(b); death.emitters[3].initialDelay = 0.5;
  assert.match(admitRecipe({ kind: "field", class: "late-delay", target: { index: 3, name: "Embers" } }, b, death).join(), /death child/);
  // Parent lifetime 1 with randomLifetimePerc fround(0.1): the serialiser's minY is fround(0.9), which equals the delay.
  const tight = base();
  tight.emitters[0].lifetime = 1; tight.emitters[0].groups[1].minY = f(1 * f(1 - f(0.1)));
  const edited = clone(tight); edited.emitters[1].initialDelay = f(0.9);
  const child = { kind: "field", class: "child-timing", target: { index: 1, name: "Smoke" } };
  assert.match(admitRecipe(child, tight, edited).join(), /minimum lifetime does not exceed/);
  const fine = clone(b); fine.emitters[1].initialDelay = f(0.6);
  assert.deepEqual(admitRecipe(child, b, fine), []);
});

test("admitRecipe: flipbook bases are step, square and in-grid, and the plant must play differently", () => {
  const b = base();
  const recipe = { kind: "field", class: "reversed-flipbook", target: { index: 0, name: "Fireball" } };
  const reversed = clone(b); reversed.emitters[0].tracks[5].keys = [[0, 3], [25, 2], [50, 1], [75, 0], [100, 0]];
  assert.deepEqual(admitRecipe(recipe, b, reversed), []);
  const linear = clone(b); linear.emitters[0].tracks[5].interpolation = 0;
  assert.match(admitRecipe(recipe, linear, reversed).join(), /step-interpolated/);
  const six = clone(b); six.emitters[0].textureSize = 6;
  assert.match(admitRecipe(recipe, six, reversed).join(), /perfect square/);
  // The fractional base [0.1, 1.2, 0.3, 1.4, 0.5] plays [0, 1, 0, 1, 0] reversed or not.
  const frac = clone(b); frac.emitters[0].textureSize = 4;
  frac.emitters[0].tracks[5].keys = [[0, 0.1], [25, 1.2], [50, 0.3], [75, 1.4], [100, 0.5]];
  const fracCase = clone(frac); fracCase.emitters[0].tracks[5].keys = [[0, 0.5], [25, 1.4], [50, 0.3], [75, 1.2], [100, 0.1]];
  assert.match(admitRecipe(recipe, frac, fracCase).join(), /plays like the base/);
});

test("intent targets: parsed, and equal to the persisted base exactly", () => {
  const { targets, errors } = parseTargets("# Fireball\n\n## Targets\n\n- Fireball (0) lifetime: 4\n- Smoke (1) initialDelay: 0.2\n");
  assert.deepEqual(errors, []);
  assert.deepEqual(checkTargets(targets, base(), persistedText), []);
  const wrong = parseTargets("## Targets\n- Fireball (0) lifetime: 4.0\n").targets;
  assert.match(checkTargets(wrong, base(), persistedText).join(), /not the persisted 4/);
  const count = base(); count.emitters[2].nParticlesPerBurst = 16777217;
  const close = parseTargets("## Targets\n- Sparks (2) nParticlesPerBurst: 16777216\n").targets;
  assert.match(checkTargets(close, count, persistedText).join(), /not the persisted 16777217/);
  assert.match(parseTargets("# no targets\n").errors.join(), /no ## Targets/);
});

test("a permutation case's intent states its targets in the case's numbering and order", () => {
  const intent = "# Campfire\n\nThe flame glows in front of the smoke.\n\n## Targets\n\n- smoke (0) lifetime: 2\n- smoke (0) initialDelay: 0\n- fire (1) lifetime: 0.5\n- soft (2) lifetime: 1\n\n## Notes\n\n- kept (0)\n";
  const swapped = caseIntent(intent, { kind: "permutation", map: [1, 0, 2] });
  assert.equal(swapped, "# Campfire\n\nThe flame glows in front of the smoke.\n\n## Targets\n\n- fire (0) lifetime: 0.5\n- smoke (1) lifetime: 2\n- smoke (1) initialDelay: 0\n- soft (2) lifetime: 1\n\n## Notes\n\n- kept (0)\n");
  assert.deepEqual(parseTargets(swapped).errors, []);
  assert.equal(caseIntent(intent, { kind: "field" }), intent, "a field case keeps the base's intent");
  assert.equal(caseIntent(intent.replace(/\n/g, "\r\n"), { kind: "permutation", map: [1, 0, 2] }), swapped.replace(/\n/g, "\r\n"));
  assert.throws(() => caseIntent("## Targets\n- smoke (3) lifetime: 2\n", { kind: "permutation", map: [1, 0, 2] }), /unreadable target line/);
});

const TARGETS = parseTargets("## Targets\n- Fireball (0) lifetime: 4\n- Smoke (1) initialDelay: 0.2\n").targets;
const ctx = (over = {}) => {
  const b = base(), c = clone(b);
  c.emitters[0].lifetime = 2;
  return { recipe: { kind: "field" }, baseInspect: b, caseInspect: c, targets: TARGETS, rubric: RUBRIC, isRecipeVariant: true, ...over };
};
const variant = (over = {}) => ({ id: 0, bundleId: "bundle", category: "duration", direction: "too-short", emitters: [{ index: 0, name: "Fireball" }],
  windows: { bundle: { image: [3], telemetry: [] } }, policy: { primary: "frames", "bundle-only": "frames" },
  correction: { kind: "scalar", emitter: { index: 0, name: "Fireball" }, field: "lifetime" }, ...over });

test("freezeVariant derives restore for the recipe's answer and intervene for alternatives", () => {
  const v0 = freezeVariant(variant(), ctx());
  assert.equal(v0.correction.accept.rule, "restore");
  assert.deepEqual(v0.correction.accept.interval, [2.5, 4]);
  assert.equal(v0.patchEligible, true);
  const alt = freezeVariant(variant({ id: 1, category: "timing", direction: "too-late", emitters: [{ index: 1, name: "Smoke" }],
    correction: { kind: "scalar", emitter: { index: 1, name: "Smoke" }, field: "initialDelay" } }), ctx({ isRecipeVariant: false }));
  assert.equal(alt.correction.accept.rule, "intervene");
  assert.deepEqual(alt.correction.accept.interval, [f(f(0.2) - 0.5 * f(0.2)), f(f(0.2) - 0.1 * f(0.2))]);
});

test("freezeVariant refuses what decision 11 forbids", () => {
  assert.match(freezeVariant(variant({ correction: { kind: "scalar", emitter: { index: 1, name: "Smoke" }, field: "initialDelay" }, category: "timing", direction: "too-late" }), ctx()).error, /planted delta/);
  assert.match(freezeVariant(variant({ id: 2 }), ctx({ isRecipeVariant: false })).error, /planted field/);
  assert.match(freezeVariant(variant(), ctx({ targets: [] })).error, /no stated target/);
  assert.match(freezeVariant(variant({ correction: { kind: "scalar", emitter: { index: 0, name: "Fireball" }, field: "randomScalePerc" } }), ctx()).error, /not a correction option/);
});

test("freezeVariant: enum, track and seed contracts", () => {
  const blend = freezeVariant(variant({ category: "blend-mode", direction: "should-be-additive", correction: { kind: "enum", emitter: { index: 0, name: "Fireball" }, field: "blendMode" } }), ctx());
  assert.deepEqual(blend.correction.acceptable, [1, 4]);
  const already = ctx(); already.caseInspect.emitters[0].blendMode = 1;
  assert.match(freezeVariant(variant({ category: "blend-mode", direction: "should-be-additive", correction: { kind: "enum", emitter: { index: 0, name: "Fireball" }, field: "blendMode" } }), already).error, /already acceptable/);
  const flip = ctx(); flip.caseInspect.emitters[0].tracks[5].keys = [[0, 3], [25, 2], [50, 1], [75, 0], [100, 0]];
  const t = freezeVariant(variant({ category: "flipbook", direction: "reversed", correction: { kind: "track", emitter: { index: 0, name: "Fireball" }, track: 5 } }), flip);
  assert.equal(t.correction.cells, 16);
  const seedCtx = ctx({ recipe: { kind: "seed" } });
  seedCtx.caseInspect = clone(seedCtx.baseInspect);
  seedCtx.baseInspect.emitters[1].randomLifetimePerc = f(0.1); seedCtx.caseInspect.emitters[1].randomLifetimePerc = f(0.1);
  const s = freezeVariant(variant({ bundleId: "bundle-b", category: "variation", direction: "too-much-variation", emitters: [{ index: 1, name: "Smoke" }],
    correction: { kind: "scalar", emitter: { index: 1, name: "Smoke" }, field: "randomLifetimePerc" } }), seedCtx);
  assert.equal(s.correction.accept.rule, "seed");
  assert.deepEqual(s.correction.accept.interval, [0, f(0.075)]);
  const order = freezeVariant(variant({ category: "draw-order", direction: "wrong-order", relation: { front: { index: 1, name: "B" }, back: { index: 0, name: "A" } }, correction: { kind: "reorder" } }), ctx());
  assert.equal(order.patchEligible, false);
});
