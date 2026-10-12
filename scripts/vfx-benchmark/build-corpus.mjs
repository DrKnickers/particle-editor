// build-corpus (plan 3.4): draft a corpus from the owner's authored cases,
// then lock it after the owner's blind pass.
//
//   node scripts/vfx-benchmark/build-corpus.mjs --draft <store> [--exe <path>]
//   node scripts/vfx-benchmark/build-corpus.mjs --lock <store>
//
// Store layout (the owner provides the inputs; everything else is written):
//   corpus.json                    the skeleton: version, provenance.contentRoots, targets,
//                                  attempts, bases, requiredClasses, seedSearch | seedAbsence
//   bases/<id>/effect.alo          canonical base (canonicalise.mjs proved it stable)
//   bases/<id>/intent.md           what the effect is, with its ## Targets section
//   bases/<id>/observation.json    the base's frozen observation spec (aloPath is filled in)
//   authoring/<name>/effect.alo    a hand-authored case, with its recipe.json
// Written by --draft:
//   bases/<id>/inspect.json  cases/<id>/{effect.alo,recipe.json,inspect.json,observation.json,drift.json}
//   bundles/<id>[/<label>]/  packs/<id>/  packs-bundle-only/<id>/  assets.json  provenance.json
// The blind pass writes cases/<id>/truth.json; --lock freezes it.
import fs from "node:fs";
import { randomBytes } from "node:crypto";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnWithDeadline } from "../observe.mjs";
import { ssim as ffmpegSsim } from "../lib/ssim.mjs";
import { admitBase, admitRecipe, checkCorpus, checkRecipe, checkRoots } from "./corpus.mjs";
import { inspectDiff, patchRequest, positionOf, verifyRecipe } from "./recipe.mjs";
import { checkTargets, freezeVariant, parseTargets } from "./freeze.mjs";
import { persistedText } from "./persisted.mjs";
import { driftFloor, windowErrors } from "./drift.mjs";
import { checkAttestation, checkBundle, checkDependencies, assetSet } from "./provenance.mjs";
import { labelTrio, prng, seedSearch } from "./seed.mjs";
import { allowlist, buildPack, inventory } from "./pack.mjs";
import { rulesMarkdown } from "./rules.mjs";
import { lockAnswerCheck } from "./score.mjs";
import { contextFor } from "./context.mjs";
import { RUBRIC } from "./schema-check.mjs";
import { DEFAULT_EXE, exeLedger, pinFiles, readLines, repoRoot, serialise, sha256, store, tools } from "./store.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const CLASS = Object.fromEntries(RUBRIC.classes.map((c) => [c.id, c]));
export const SEED_COUNT = 50;

class CorpusError extends Error {}
const refuse = (errors) => { if (errors.length) throw new CorpusError(errors.join("\n")); };

export function sharedFiles(intent, textures) {
  return {
    "prompt.md": fs.readFileSync(join(here, "prompt.md")),
    "rules.md": rulesMarkdown(),
    "findings.schema.json": fs.readFileSync(join(here, "findings.schema.json")),
    "intent.md": intent,
    "textures.json": `${JSON.stringify(textures, null, 2)}\n`,
  };
}

function specFor(baseSpec, aloPath, seed = null) {
  const spec = structuredClone(baseSpec);
  spec.scenario.aloPath = aloPath;
  if (seed !== null) spec.scenario.seed = seed;
  return spec;
}

// SSIM < 1 per aligned frame; equal PNG hashes are identical without a decode.
async function alignedSsim(baseDir, caseDir, baseCapture, caseCapture, ssim) {
  const caseFrames = new Map(caseCapture.frames.map((f) => [f.frame, f]));
  const out = {};
  for (const b of baseCapture.frames) {
    const c = caseFrames.get(b.frame);
    if (!c) continue;
    if (b.sha256 === c.sha256) { out[b.frame] = 1; continue; }
    const r = await ssim(join(baseDir, b.png), join(caseDir, c.png));
    if (!r.ok) throw new CorpusError(`ssim ${b.frame}: ${r.why}`);
    out[b.frame] = Math.min(r.all, 0.999999);   // bytes differ: never report identity
  }
  return out;
}

export async function draft({ root, exe = DEFAULT_EXE, run = spawnWithDeadline, ssim = ffmpegSsim, io = fs, say = console.log, ids = () => randomBytes(4).toString("hex") }) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  if (corpus.state !== "draft") throw new CorpusError("a locked corpus is immutable; start a new version");
  const contentRoots = corpus.provenance.contentRoots;
  refuse(checkRoots({ repo: repoRoot, stores: dirname(root), content: contentRoots }));
  const ledger = exeLedger(exe, io);
  const t = tools({ exe, run, roots: contentRoots, io });
  const provenance = { contentRoots, render: null, assets: new Map() };
  const observeAt = async (label, specPath, dir) => {
    const capture = await ledger.around(label, () => t.observe(specPath, dir));
    return capture;
  };

  // Bases.
  const bases = {};
  for (const b of corpus.bases) {
    if (!b.canonical || b.canonical !== s.hash("bases", b.id, "effect.alo")) throw new CorpusError(`base ${b.id}: no canonicalisation proof for this effect.alo (run canonicalise.mjs)`);
    const inspect = await t.inspect(s.at("bases", b.id, "effect.alo"), s.at("bases", b.id, "inspect.json"));
    refuse(admitBase(inspect, b).map((e) => `base ${b.id}: ${e}`));
    const intent = s.text("bases", b.id, "intent.md");
    const { targets, errors } = parseTargets(intent);
    refuse([...errors, ...checkTargets(targets, inspect, persistedText)].map((e) => `base ${b.id}: ${e}`));
    const baseSpec = s.json("bases", b.id, "observation.json");
    const specPath = s.writeJson(["bundles", `base-${b.id}.observation.json`], specFor(baseSpec, s.at("bases", b.id, "effect.alo")));
    const capture = await observeAt(`base ${b.id}`, specPath, s.at("bundles", `base-${b.id}`));
    refuse([...checkBundle(capture, provenance, { label: `base ${b.id}` }), ...checkAttestation(capture, inspect, { label: `base ${b.id}` })]);
    bases[b.id] = { ...b, inspect, intent, targets, spec: baseSpec, capture, dir: s.at("bundles", `base-${b.id}`), assets: assetSet(capture) };
    say(`[bench] base ${b.id}: inspected, observed, ${capture.content.resolvedTextures.length} textures resolved`);
  }
  const textures = inventory(Object.values(bases).map((b) => b.capture.content.resolvedTextures));

  // Defect cases, then the seed pair, then the controls.
  const cases = [];
  for (const name of s.list("authoring")) {
    const recipe = JSON.parse(io.readFileSync(s.at("authoring", name, "recipe.json"), "utf8"));
    refuse(checkRecipe(recipe).map((e) => `${name}: ${e}`));
    const base = bases[recipe.base];
    if (!base) throw new CorpusError(`${name}: unknown base ${recipe.base}`);
    if (!CLASS[recipe.class]) throw new CorpusError(`${name}: unknown class ${recipe.class}`);
    cases.push({ name, recipe, base, kind: "defect", shape: "single" });
  }
  let seedAbsence = corpus.seedAbsence ?? null, seedRecord = null;
  if (corpus.seedSearch) {
    const base = bases[corpus.seedSearch.base];
    const runs = [];
    for (let seed = 1; seed <= SEED_COUNT; seed++) {
      const spec = specFor(base.spec, s.at("bases", base.id, "effect.alo"), seed);
      spec.output.strideInterval = 60; spec.output.playbackStride = 0;
      const specPath = s.writeJson(["bundles", "seed-search", `${seed}.observation.json`], spec);
      const dir = s.at("bundles", "seed-search", String(seed));
      const capture = await observeAt(`seed ${seed}`, specPath, dir);
      refuse(checkBundle(capture, provenance, { label: `seed ${seed}` }));
      const lines = readLines(join(dir, "telemetry", "frames.jsonl"), io);
      const l = capture.lifecycle;
      runs.push({ seed, status: l.status, refusals: { runtimeGuard: l.limits.runtimeGuard.total, hardGuard: l.limits.hardGuard.total, indexCap: l.limits.indexCap.total },
        peakParticles: lines.length ? Math.max(...lines.map((x) => x.particles)) : null, lastActive: l.lastActive });
    }
    seedRecord = seedSearch(runs, { drawSeed: corpus.seedSearch.drawSeed });
    s.writeJson(["seed-search.json"], seedRecord);
    if (seedRecord.outlier === null) seedAbsence = seedRecord.absence;
    else {
      const random = prng(corpus.seedSearch.labelSeed);
      const defect = labelTrio(seedRecord.defectTrio, random), control = labelTrio(seedRecord.controlTrio, random);
      const outlier = Object.keys(defect).find((k) => defect[k] === seedRecord.outlier);
      cases.push({ name: "seed", recipe: { schemaVersion: 1, base: base.id, kind: "seed", class: "seed", seeds: defect, outlier }, base, kind: "defect", shape: "three" });
      cases.push({ name: "seed-control", recipe: { schemaVersion: 1, base: base.id, kind: "none", seeds: control }, base, kind: "control", shape: "three" });
    }
  }

  // One single-bundle control for every base a retained case uses, the seed base included.
  for (const b of new Set(cases.filter((c) => c.kind === "defect").map((c) => c.base.id))) {
    cases.push({ name: `control-${b}`, recipe: { schemaVersion: 1, base: b, kind: "none" }, base: bases[b], kind: "control", shape: "single" });
  }

  // One opaque id per case and control, in one recorded random order.
  for (const c of cases) c.id = ids();
  if (new Set(cases.map((c) => c.id)).size !== cases.length) throw new CorpusError("opaque id collision; run again");
  const order = cases.map((c) => c.id).sort();

  for (const c of cases) {
    const caseDir = ["cases", c.id];
    const source = c.kind === "control" || c.recipe.kind === "seed" ? s.at("bases", c.base.id, "effect.alo") : s.at("authoring", c.name, "effect.alo");
    s.write([...caseDir, "effect.alo"], io.readFileSync(source));
    s.writeJson([...caseDir, "recipe.json"], c.recipe);
    const alo = s.at(...caseDir, "effect.alo");
    const inspect = await t.inspect(alo, s.at(...caseDir, "inspect.json"));
    c.inspect = inspect;
    if (c.kind === "control" || c.recipe.kind === "seed") {
      if (s.hash(...caseDir, "effect.alo") !== s.hash("bases", c.base.id, "effect.alo")) throw new CorpusError(`${c.name}: not byte-identical to its base`);
    }
    let patchedInspect = null;
    if (c.recipe.kind === "field") {
      const request = s.writeJson(["work", c.id, "request.json"], patchRequest(c.recipe, c.base.inspect));
      await t.patch(s.at("bases", c.base.id, "effect.alo"), request, s.at("work", c.id, "patched.alo"), s.at("work", c.id, "patch-report.json"));
      patchedInspect = await t.inspect(s.at("work", c.id, "patched.alo"), s.at("work", c.id, "patched.inspect.json"));
    }
    refuse([...admitRecipe(c.recipe, c.base.inspect, inspect), ...verifyRecipe({ recipe: c.recipe, baseInspect: c.base.inspect, caseInspect: inspect, patchedInspect })]
      .map((e) => `${c.name}: ${e}`));

    // Observation: one bundle, or three labelled seed bundles.
    const labels = c.recipe.seeds ? Object.keys(c.recipe.seeds).sort() : [null];
    c.captures = {};
    for (const label of labels) {
      const dirParts = label ? ["bundles", c.id, label] : ["bundles", c.id];
      const spec = specFor(c.base.spec, alo, label ? c.recipe.seeds[label] : null);
      const specPath = s.writeJson([...caseDir, label ? `observation.${label}.json` : "observation.json"], spec);
      const capture = await observeAt(`${c.name}${label ? ` ${label}` : ""}`, specPath, s.at(...dirParts));
      const plantedName = c.recipe.class === "missing-texture" ? inspect.emitters[positionOf(inspect, c.recipe.target)].colorTexture : null;
      const allow = plantedName ? [`texture:${plantedName.toUpperCase()}`, ...c.base.assets.filter((k) => !assetSet(capture).includes(k))] : [];
      refuse([
        ...checkBundle(capture, provenance, { label: c.name, allowAssets: allow }),
        ...checkDependencies(capture, c.base.assets, { label: c.name, allow }),
        ...checkAttestation(capture, inspect, { label: c.name, base: plantedName ? null : c.base.capture, planted: plantedName }),
      ]);
      if (plantedName && textures.includes(plantedName)) throw new CorpusError(`${c.name}: the planted name ${plantedName} is in the texture inventory`);
      c.captures[label ?? "bundle"] = { capture, dir: s.at(...dirParts) };
    }

    // The drift floor (defect cases with one bundle; the seed case is decided by the blind pass).
    if (c.kind === "defect" && c.shape === "single") {
      const caseCapture = c.captures.bundle.capture;
      const ssimValues = await alignedSsim(c.base.dir, c.captures.bundle.dir, c.base.capture, caseCapture, ssim);
      const delayTarget = c.recipe.kind === "field" && c.base.inspect.emitters[positionOf(c.base.inspect, c.recipe.target)].initialDelay !== inspect.emitters[positionOf(inspect, c.recipe.target)].initialDelay
        ? positionOf(inspect, c.recipe.target) : undefined;
      const drift = driftFloor({ recipe: c.recipe, base: { capture: c.base.capture, lines: readLines(join(c.base.dir, "telemetry", "frames.jsonl"), io) },
        case: { capture: caseCapture, lines: readLines(join(c.captures.bundle.dir, "telemetry", "frames.jsonl"), io) },
        ssim: ssimValues, inspectDiffEmpty: inspectDiff(c.base.inspect, inspect).length === 0, subjective: CLASS[c.recipe.class].stratum === "subjective", delayTarget });
      s.writeJson([...caseDir, "drift.json"], { ...drift, ssim: ssimValues });
      refuse(drift.refusals.map((e) => `${c.name}: ${e}`));
      for (const w of drift.warnings) say(`[bench] ${c.name}: warning: ${w}`);
    }

    // Packs: primary (with inspect.json) for every case; bundle-only for subjective cases and controls.
    const bundles = Object.fromEntries(Object.entries(c.captures).map(([label, v]) => [label, v.dir]));
    const shared = sharedFiles(c.base.intent, textures);
    const primary = await buildPack({ out: s.at("packs", c.id), bundles, shared, inspectJson: s.read(...caseDir, "inspect.json"), run, io });
    s.writeJson(["packs", `${c.id}.manifest.json`], primary.manifest);
    const stratum = c.kind === "defect" ? CLASS[c.recipe.class].stratum : null;
    if (c.kind === "control" || stratum === "subjective") {
      const secondary = await buildPack({ out: s.at("packs-bundle-only", c.id), bundles, shared, run, io });
      s.writeJson(["packs-bundle-only", `${c.id}.manifest.json`], secondary.manifest);
    }
    say(`[bench] ${c.name} -> ${c.id}: ${c.kind}, observed, packed`);
  }

  s.writeJson(["assets.json"], Object.fromEntries([...provenance.assets.entries()].sort(([a], [b]) => (a < b ? -1 : 1))));
  s.writeJson(["provenance.json"], { exeSha256: ledger.sha256, contentRoots, render: provenance.render, generations: ledger.entries });
  const next = {
    ...corpus,
    provenance: { ...corpus.provenance, exeSha256: ledger.sha256, render: provenance.render },
    cases: cases.map((c) => ({ id: c.id, base: c.base.id, kind: c.kind, shape: c.shape,
      ...(c.kind === "defect" ? { class: c.recipe.class, stratum: CLASS[c.recipe.class].stratum } : {}) })),
    order,
    ...(seedAbsence ? { seedAbsence } : {}),
    hashes: {
      rubric: sha256(fs.readFileSync(join(here, "rubric.json"))), prompt: sha256(fs.readFileSync(join(here, "prompt.md"))),
      schema: sha256(fs.readFileSync(join(here, "findings.schema.json"))), rules: sha256(rulesMarkdown()),
    },
  };
  io.rmSync(s.at("work"), { recursive: true, force: true });
  next.pins = pinFiles(root, io);
  refuse(checkCorpus(next));
  s.writeJson(["corpus.json"], next);
  // The private name -> id map stays in the store, beside the authoring folders.
  s.writeJson(["authoring-map.json"], Object.fromEntries(cases.map((c) => [c.name, c.id])));
  say(`[bench] draft: ${cases.length} packs in ${order.length} ids; next: the owner's blind pass, then --lock`);
  return next;
}

// --lock: freeze every variant, check windows and policies, run the answer check, seal.
export function lock({ root, io = fs, say = console.log, transports = ["frames", "contact-sheets"] }) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  if (corpus.state !== "draft") throw new CorpusError("already locked");
  const textures = JSON.parse(io.readFileSync(s.at("packs", corpus.cases[0].id, "textures.json"), "utf8"));
  const errors = [];
  const truths = {};
  for (const c of corpus.cases) {
    if (!s.exists("cases", c.id, "truth.json")) { errors.push(`${c.id}: no truth.json (blind pass incomplete)`); continue; }
    const truth = s.json("cases", c.id, "truth.json");
    truths[c.id] = truth;
    const recipe = s.json("cases", c.id, "recipe.json");
    const baseInspect = s.json("bases", c.base, "inspect.json"), caseInspect = s.json("cases", c.id, "inspect.json");
    const { targets } = parseTargets(s.text("bases", c.base, "intent.md"));
    const drift = c.kind === "defect" && c.shape === "single" ? s.json("cases", c.id, "drift.json") : null;
    const frozen = {};
    for (const transport of transports) {
      const set = truth.transports?.[transport];
      if (!set) { errors.push(`${c.id}: no ${transport} annotation`); continue; }
      if (c.kind === "control") {
        if (set.variants.length) errors.push(`${c.id}: the owner flagged a control under ${transport}; its base is unsuitable`);
        frozen[transport] = { visible: false, variants: [] };
        continue;
      }
      if (!set.visible && c.stratum === "objective") errors.push(`${c.id}: not visible under ${transport}`);
      const variants = set.variants.map((v, id) => {
        const f = freezeVariant({ ...v, id }, { recipe, baseInspect, caseInspect, targets, rubric: RUBRIC, isRecipeVariant: id === 0 });
        if (f.error) errors.push(`${c.id} ${transport} variant ${id}: ${f.error}`);
        if (drift && c.stratum === "objective") {
          for (const [bundle, w] of Object.entries(v.windows)) {
            for (const e of windowErrors(w, { image: drift.candidateImage, telemetry: drift.candidateTelemetry })) errors.push(`${c.id} ${transport} variant ${id} ${bundle}: ${e}`);
          }
        }
        return f;
      });
      if (variants.some((v) => v.error)) continue;
      const contexts = { primary: contextFor(s, c, "primary", textures) };
      if (s.exists("packs-bundle-only", c.id)) contexts["bundle-only"] = contextFor(s, c, "bundle-only", textures);
      for (const e of lockAnswerCheck({ caseInfo: { kind: "defect" }, variants, contexts, rubric: RUBRIC })) errors.push(`${c.id} ${transport}: ${e}`);
      frozen[transport] = { visible: set.visible, variants };
    }
    if (c.kind === "control") {
      const contexts = { primary: contextFor(s, c, "primary", textures), "bundle-only": contextFor(s, c, "bundle-only", textures) };
      for (const e of lockAnswerCheck({ caseInfo: { kind: "control" }, variants: [], contexts, rubric: RUBRIC })) errors.push(`${c.id}: ${e}`);
    }
    c.secondaryEligible = c.kind === "defect" && c.stratum === "subjective" &&
      Object.values(frozen).some((f) => f.variants?.some((v) => v.policy?.["bundle-only"]));
    s.writeJson(["cases", c.id, "answers.json"], frozen);
  }
  refuse(errors);
  const truthHash = sha256(serialise(Object.fromEntries(Object.entries(truths).sort(([a], [b]) => (a < b ? -1 : 1)))));
  const locked = { ...corpus, state: "locked", hashes: { ...corpus.hashes, truth: truthHash }, pins: pinFiles(root, io), lock: { at: new Date().toISOString() } };
  refuse(checkCorpus(locked));
  s.writeJson(["corpus.json"], locked);
  say(`[bench] locked ${corpus.version}: ${corpus.cases.length} cases, truth ${truthHash.slice(0, 8)}`);
  return locked;
}

export async function main(argv = process.argv.slice(2)) {
  const [mode, root, ...rest] = argv;
  const exeAt = rest.indexOf("--exe");
  try {
    if (mode === "--draft" && root) await draft({ root, exe: exeAt >= 0 ? rest[exeAt + 1] : DEFAULT_EXE });
    else if (mode === "--lock" && root) lock({ root });
    else { console.error("usage: build-corpus.mjs --draft <store> [--exe <path>] | --lock <store>"); return 2; }
    return 0;
  } catch (error) {
    console.error(`[bench] ${error instanceof CorpusError ? "refused" : "failed"}: ${error.message}`);
    return error instanceof CorpusError ? 3 : 1;
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = await main();
