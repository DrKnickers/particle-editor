// derive-truth: the scripted alternative to the owner's blind pass. Every
// defect case's answer is what its recipe planted, and its evidence window is
// the drift floor's candidate frames; a control carries no answer. It writes
// through blind-page's own write path, so every answer is a complete variant,
// and --lock then checks each one exactly as it checks the owner's.
//
//   node scripts/vfx-benchmark/derive-truth.mjs <store>
//
// What this gives up (the report says so): there is no human baseline, no
// alternative answers (a finding must name the planted correction), and
// visibility means "the drift floor measured a change in the telemetry, or
// an image change below PERCEPTIBLE SSIM", not "a person saw it". The seed case needs a judgement of intent, so it is refused here: a
// corpus without a person declares its absence instead.
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { TRANSPORTS, writeTruth } from "./blind-page.mjs";
import { positionOf } from "./recipe.mjs";
import { RUBRIC } from "./schema-check.mjs";
import { store } from "./store.mjs";

const CLASS = Object.fromEntries(RUBRIC.classes.map((c) => [c.id, c]));
const ADDITIVE = RUBRIC.categories["blend-mode"].directions["should-be-additive"][0].acceptable;
const ref = (e) => ({ index: e.index, name: e.name });
// SSIM below which an aligned frame's change is treated as one a person would see.
// Frames are mostly black, so SSIM is generous: a 0.974 frame of dark smoke on
// the dark background was checked by eye and showed nothing.
export const PERCEPTIBLE = 0.95;

// The planted diagnosis and correction of one field recipe.
export function plantedVariant(recipe, baseInspect, caseInspect, drift) {
  const cls = CLASS[recipe.class];
  if (!cls) throw new Error(`unknown class ${recipe.class}`);
  if (recipe.kind !== "field") throw new Error(`${recipe.class}: only field recipes are derived (a ${recipe.kind} recipe needs a person)`);
  const target = caseInspect.emitters[positionOf(caseInspect, recipe.target)];
  const emitter = ref(target);
  const m = recipe.mutations[0];
  let direction = cls.direction, correction;
  switch (m.op) {
    case "setProperty": correction = { kind: "scalar", emitter, field: m.field }; break;
    case "setColorTexture": correction = { kind: "texture", emitter, field: "colorTexture" }; break;
    case "setNormalTexture": correction = { kind: "texture", emitter, field: "normalTexture" }; break;
    case "setBlendMode":
      direction = ADDITIVE.includes(baseInspect.emitters[positionOf(baseInspect, recipe.target)].blendMode) ? "should-be-additive" : "should-be-alpha";
      correction = { kind: "enum", emitter, field: "blendMode" };
      break;
    case "replaceTrack": correction = { kind: "track", emitter, track: m.track }; break;
    default: throw new Error(`${recipe.class}: no derived correction for ${m.op}`);
  }
  // An image-only change counts only if some aligned frame drops below PERCEPTIBLE.
  const perceptible = Object.values(drift?.ssim ?? {}).some((v) => v < PERCEPTIBLE);
  const window = { image: perceptible ? drift?.candidateImage ?? [] : [], telemetry: drift?.candidateTelemetry ?? [] };
  const any = window.image.length > 0 || window.telemetry.length > 0;
  const policy = cls.category === "missing-texture"
    ? { primary: "inspect", "bundle-only": null }
    : { primary: any ? "either" : "inspect", "bundle-only": any && cls.stratum === "subjective" ? "frames" : null };
  const variant = { bundleId: "bundle", category: cls.category, direction, emitters: [emitter],
    windows: { bundle: policy.primary === "inspect" ? { image: [], telemetry: [] } : window }, policy, correction };
  return { variant, visible: cls.category === "missing-texture" || any };
}

export function deriveTruth({ root, io = fs, say = console.log }) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  for (const c of corpus.cases) {
    let truth;
    if (c.kind === "control") truth = { visible: false, variants: [] };
    else {
      const recipe = s.json("cases", c.id, "recipe.json");
      if (recipe.kind === "seed") throw new Error(`${c.id}: the seed case's answer is a judgement of intent; derive-truth refuses it`);
      const drift = s.exists("cases", c.id, "drift.json") ? s.json("cases", c.id, "drift.json") : null;
      const { variant, visible } = plantedVariant(recipe, s.json("bases", c.base, "inspect.json"), s.json("cases", c.id, "inspect.json"), drift);
      truth = { visible, variants: visible ? [variant] : [] };
    }
    writeTruth(root, c.id, { transports: Object.fromEntries(TRANSPORTS.map((t) => [t, truth])) }, io);
    say(`[bench] ${c.id}: ${c.kind === "control" ? "control, no answer" : truth.visible ? `${truth.variants[0].category}/${truth.variants[0].direction}` : "not visible"}`);
  }
}

export function main(argv = process.argv.slice(2)) {
  if (argv.length !== 1) { console.error("usage: derive-truth.mjs <store>"); return 2; }
  try { deriveTruth({ root: argv[0] }); return 0; } catch (error) { console.error(`[bench] refused: ${error.message}`); return 3; }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main();
