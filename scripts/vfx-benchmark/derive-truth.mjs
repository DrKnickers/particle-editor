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
// an aligned frame changed by at least PERCEPTIBLE in some 16x16 block", not
// "a person saw it". The seed case needs a judgement of intent, so it is
// refused here: a corpus without a person declares its absence instead.
import fs from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { blockDiff } from "../lib/ssim.mjs";
import { TRANSPORTS, writeTruth } from "./blind-page.mjs";
import { positionOf } from "./recipe.mjs";
import { RUBRIC } from "./schema-check.mjs";
import { store } from "./store.mjs";

const CLASS = Object.fromEntries(RUBRIC.classes.map((c) => [c.id, c]));
const ADDITIVE = RUBRIC.categories["blend-mode"].directions["should-be-additive"][0].acceptable;
const ref = (e) => ({ index: e.index, name: e.name });
// The block luminance change (blockDiff, 0-255) from which an aligned frame's
// change is treated as one a person would see. Whole-frame SSIM averages a
// local change away (a flame dimmed behind smoke, plainly visible, scored
// 0.986), so the measure is local. Calibrated on stored cases: the controls
// peak at 1; a dark smoke cloud of uneven puffs, checked by eye and showing
// nothing, at 12; the weakest case an agent still found, at 19.
export const PERCEPTIBLE = 16;

// The planted diagnosis and correction of one field or permutation recipe.
// `peaks` holds each changed aligned frame's block difference.
export function plantedVariant(recipe, baseInspect, caseInspect, drift, peaks = {}) {
  const cls = CLASS[recipe.class];
  if (!cls) throw new Error(`unknown class ${recipe.class}`);
  if (recipe.kind === "permutation") return plantedReorder(recipe, cls, drift, peaks);
  if (recipe.kind !== "field") throw new Error(`${recipe.class}: only field and permutation recipes are derived (a ${recipe.kind} recipe needs a person)`);
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
  // An image-only change counts only if some aligned frame changed perceptibly.
  const perceptible = Object.values(peaks).some((v) => v >= PERCEPTIBLE);
  const window = { image: perceptible ? drift?.candidateImage ?? [] : [], telemetry: drift?.candidateTelemetry ?? [] };
  const any = window.image.length > 0 || window.telemetry.length > 0;
  const policy = cls.category === "missing-texture"
    ? { primary: "inspect", "bundle-only": null }
    : { primary: any ? "either" : "inspect", "bundle-only": any && cls.stratum === "subjective" ? "frames" : null };
  const variant = { bundleId: "bundle", category: cls.category, direction, emitters: [emitter],
    windows: { bundle: policy.primary === "inspect" ? { image: [], telemetry: [] } : window }, policy, correction };
  return { variant, visible: cls.category === "missing-texture" || any };
}

// A draw-order case: the recipe's relation (the emitter that should be in
// front, and the one behind, by case identity) and a reorder. Its evidence is
// the frames alone, and only the candidate frames (both roots live) whose
// image changed: a frame where the two do not overlap shows no order.
function plantedReorder(recipe, cls, drift, peaks) {
  const { front, back } = recipe.relation;
  const ssim = drift?.ssim ?? {};
  const image = (drift?.candidateImage ?? []).filter((f) => ssim[f] !== undefined && ssim[f] < 1);
  const perceptible = image.some((f) => peaks[f] >= PERCEPTIBLE);
  const window = { image: perceptible ? image : [], telemetry: [] };
  const variant = { bundleId: "bundle", category: cls.category, direction: cls.direction,
    emitters: [ref(front), ref(back)].sort((a, b) => a.index - b.index), relation: { front: ref(front), back: ref(back) },
    windows: { bundle: window }, policy: { primary: "frames", "bundle-only": null }, correction: { kind: "reorder" } };
  return { variant, visible: perceptible };
}

// The block difference of every aligned frame whose image changed (SSIM < 1).
function changedPeaks(s, c, drift, diff) {
  const peaks = {};
  if (!drift) return peaks;
  const frames = (dir) => new Map(s.json("bundles", dir, "capture.json").frames.map((f) => [f.frame, f.png]));
  const base = frames(`base-${c.base}`), kase = frames(c.id);
  for (const [frame, value] of Object.entries(drift.ssim ?? {})) {
    if (!(value < 1)) continue;
    const r = diff(join(s.at("bundles", `base-${c.base}`), base.get(Number(frame))), join(s.at("bundles", c.id), kase.get(Number(frame))));
    if (!r.ok) throw new Error(`${c.id} frame ${frame}: ${r.why}`);
    peaks[frame] = r.peak;
  }
  return peaks;
}

export function deriveTruth({ root, io = fs, say = console.log, diff = blockDiff }) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  for (const c of corpus.cases) {
    let truth;
    if (c.kind === "control") truth = { visible: false, variants: [] };
    else {
      const recipe = s.json("cases", c.id, "recipe.json");
      if (recipe.kind === "seed") throw new Error(`${c.id}: the seed case's answer is a judgement of intent; derive-truth refuses it`);
      const drift = s.exists("cases", c.id, "drift.json") ? s.json("cases", c.id, "drift.json") : null;
      const peaks = changedPeaks(s, c, drift, diff);
      const { variant, visible } = plantedVariant(recipe, s.json("bases", c.base, "inspect.json"), s.json("cases", c.id, "inspect.json"), drift, peaks);
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
