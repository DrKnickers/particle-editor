import { test } from "node:test";
import assert from "node:assert/strict";
import { PERCEPTIBLE, plantedVariant } from "./derive-truth.mjs";
import { emitter } from "./testing.mjs";

const inspect = (over = {}) => ({ emitters: [emitter(0, "Fireball"), emitter(1, "Smoke", over)] });
const short = { class: "short-lifetime", kind: "field", target: { index: 1, name: "Smoke" }, mutations: [{ op: "setProperty", field: "lifetime", value: 0.4 }] };

test("an image-only change is visible only when some changed frame's block difference reaches PERCEPTIBLE", () => {
  const drift = { ssim: { 12: 0.99, 15: 0.99 }, candidateImage: [12, 15], candidateTelemetry: [] };
  const faint = plantedVariant(short, inspect(), inspect({ lifetime: 0.4 }), drift, { 12: PERCEPTIBLE - 1, 15: 3 });
  assert.equal(faint.visible, false);
  const seen = plantedVariant(short, inspect(), inspect({ lifetime: 0.4 }), drift, { 12: PERCEPTIBLE, 15: 3 });
  assert.equal(seen.visible, true);
  assert.deepEqual(seen.variant.windows.bundle, { image: [12, 15], telemetry: [] });
  assert.deepEqual(seen.variant.policy, { primary: "either", "bundle-only": "frames" });
  assert.deepEqual(seen.variant.correction, { kind: "scalar", emitter: { index: 1, name: "Smoke" }, field: "lifetime" });
});

test("a telemetry change is visible without an image change; a missing texture is visible from inspect alone", () => {
  const telemetry = plantedVariant(short, inspect(), inspect({ lifetime: 0.4 }), { ssim: { 12: 1 }, candidateImage: [12], candidateTelemetry: [9] });
  assert.deepEqual([telemetry.visible, telemetry.variant.windows.bundle], [true, { image: [], telemetry: [9] }]);
  const texture = { class: "missing-texture", kind: "field", target: { index: 1, name: "Smoke" }, mutations: [{ op: "setColorTexture", name: "absent.tga" }] };
  const missing = plantedVariant(texture, inspect(), inspect({ colorTexture: "absent.tga" }), null);
  assert.deepEqual([missing.visible, missing.variant.policy], [true, { primary: "inspect", "bundle-only": null }]);
});

test("the blend direction follows the base's mode, and a non-field recipe is refused", () => {
  const blend = { class: "blend-mode", kind: "field", target: { index: 1, name: "Smoke" }, mutations: [{ op: "setBlendMode", value: 1 }] };
  const drift = { ssim: { 3: 0.5 }, candidateImage: [3], candidateTelemetry: [] };
  assert.equal(plantedVariant(blend, inspect({ blendMode: 2 }), inspect({ blendMode: 1 }), drift).variant.direction, "should-be-alpha");
  assert.equal(plantedVariant(blend, inspect({ blendMode: 1 }), inspect({ blendMode: 2 }), drift).variant.direction, "should-be-additive");
  assert.throws(() => plantedVariant({ ...short, kind: "seed" }, inspect(), inspect(), null), /needs a person/);
});

test("a draw-order case answers with its relation and a reorder, on the candidate frames whose image changed", () => {
  const swap = { class: "draw-order", kind: "permutation", map: [1, 0], relation: { front: { index: 1, name: "Smoke" }, back: { index: 0, name: "Fireball" } } };
  const drift = { ssim: { 10: 1, 20: 0.99, 30: 0.999, 40: 0.5 }, candidateImage: [10, 20, 30], candidateTelemetry: [] };
  // Frame 40 changed most, but only one root is live there: it shows no order.
  const seen = plantedVariant(swap, inspect(), inspect(), drift, { 20: 70, 30: 4, 40: 200 });
  assert.equal(seen.visible, true);
  assert.deepEqual(seen.variant, { bundleId: "bundle", category: "draw-order", direction: "wrong-order",
    emitters: [{ index: 0, name: "Fireball" }, { index: 1, name: "Smoke" }], relation: swap.relation,
    windows: { bundle: { image: [20, 30], telemetry: [] } }, policy: { primary: "frames", "bundle-only": null }, correction: { kind: "reorder" } });
  const faint = plantedVariant(swap, inspect(), inspect(), drift, { 20: PERCEPTIBLE - 1, 30: 4, 40: 200 });
  assert.deepEqual([faint.visible, faint.variant.windows.bundle.image], [false, []]);
});
