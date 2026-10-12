import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { checkFindings, LIMITS } from "./schema-check.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const E = { index: 0, name: "Fireball" };
const base = { severity: "medium", category: "duration", direction: "too-short", claim: "c", suspectedEmitters: [E],
  evidence: { citations: [{ kind: "image", frame: 3 }] }, confidence: 0.5 };
const doc = (...findings) => JSON.stringify({ findings });

// One schema-valid finding per corpus class, and both control shapes.
const VALID = {
  scalar: { ...base, suggestedChange: { kind: "scalar", emitter: E, field: "lifetime", value: 3 } },
  enum: { ...base, suggestedChange: { kind: "enum", emitter: E, field: "blendMode", value: 1 } },
  texture: { ...base, suggestedChange: { kind: "texture", emitter: E, field: "colorTexture", name: "a.tga" } },
  track: { ...base, suggestedChange: { kind: "track", emitter: E, track: 5, keys: [[0, 0], [100, 1]] } },
  reorder: { ...base, relation: { front: E, back: { index: 1, name: "B" } }, suggestedChange: { kind: "reorder", front: E, back: { index: 1, name: "B" } } },
  seed: { ...base, bundleId: "bundle-b", evidence: { citations: [{ kind: "telemetry", bundleId: "bundle-b", frame: 4 }, { kind: "inspect", emitter: E, field: "lifetime" }] },
    suggestedChange: { kind: "scalar", emitter: E, field: "randomLifetimePerc", value: 0.05 } },
};

test("one valid fixture per class and an empty answer are ok", () => {
  for (const [name, f] of Object.entries(VALID)) assert.equal(checkFindings(doc(f)).verdict, "ok", name);
  assert.equal(checkFindings(doc()).verdict, "ok");
  assert.equal(checkFindings(Buffer.from(doc())).verdict, "ok");
});

for (const [name, text] of [
  ["not JSON", "{findings: []}"],
  ["a byte-order mark", `﻿${doc()}`],
  ["an array document", "[]"],
  ["an unknown top-level key", JSON.stringify({ findings: [], notes: "x" })],
  ["an unknown finding key", doc({ ...VALID.scalar, extra: 1 })],
  ["a missing confidence", doc((({ confidence, ...f }) => f)(VALID.scalar))],
  ["a confidence above 1", doc({ ...VALID.scalar, confidence: 1.5 })],
  ["an unknown severity", doc({ ...VALID.scalar, severity: "critical" })],
  ["an emitter without a name", doc({ ...VALID.scalar, suspectedEmitters: [{ index: 0 }] })],
  ["a negative frame", doc({ ...VALID.scalar, evidence: { citations: [{ kind: "image", frame: -1 }] } })],
  ["an unknown citation kind", doc({ ...VALID.scalar, evidence: { citations: [{ kind: "video", frame: 1 }] } })],
  ["an unknown change kind", doc({ ...VALID.scalar, suggestedChange: { kind: "delta", emitter: E, field: "lifetime", value: 1 } })],
  ["a string scalar value", doc({ ...VALID.scalar, suggestedChange: { ...VALID.scalar.suggestedChange, value: "3" } })],
  ["a non-numeric track key", doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: [[0, "a"], [100, 1]] } })],
  ["a one-key track", doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: [[0, 0]] } })],
  ["a three-element key", doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: [[0, 0, 0], [100, 1]] } })],
  ["an overflowing number", doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: [[0, 0], [100, 1e400]] } }).replace("null", "1e400")],
  ["a relation without back", doc({ ...VALID.reorder, relation: { front: E } })],
]) test(`schema-error: ${name}`, () => {
  assert.equal(checkFindings(text).verdict, "schema-error");
});

test("equal key times pass the structure; the semantic validator rejects them", () => {
  assert.equal(checkFindings(doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: [[0, 0], [50, 1], [50, 2], [100, 1]] } })).verdict, "ok");
});

test("resource limits are decided before the shape and are never repairable", () => {
  assert.equal(checkFindings("x".repeat(LIMITS.fileBytes + 1)).verdict, "resource-overflow");
  assert.equal(checkFindings(doc(...Array.from({ length: LIMITS.findings + 1 }, () => ({ broken: true })))).verdict, "resource-overflow");
  assert.equal(checkFindings(doc({ ...VALID.scalar, claim: "x".repeat(LIMITS.stringLength + 1) })).verdict, "resource-overflow");
  assert.equal(checkFindings(doc({ ...VALID.scalar, claim: "x".repeat(LIMITS.stringLength) })).verdict, "ok");
  const keys = (n) => Array.from({ length: n }, (_, i) => [i, 0]);
  const five = Array.from({ length: 5 }, () => ({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: keys(LIMITS.trackKeysTotal / 4) } }));
  assert.equal(checkFindings(doc(...five)).verdict, "resource-overflow");
  const cites = Array.from({ length: 2 }, () => ({ ...VALID.scalar, evidence: { citations: Array.from({ length: LIMITS.citationsTotal / 2 + 1 }, () => ({ kind: "image", frame: 1 })) } }));
  assert.equal(checkFindings(doc(...cites)).verdict, "resource-overflow");
});

test("a track over 65,536 keys is a shape error; exactly 65,536 is ok", () => {
  const keys = (n) => Array.from({ length: n }, (_, i) => [i, 0]);
  assert.equal(checkFindings(doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: keys(65536) } })).verdict, "ok");
  assert.equal(checkFindings(doc({ ...VALID.track, suggestedChange: { ...VALID.track.suggestedChange, keys: keys(65537) } })).verdict, "schema-error");
});

test("the agent-facing schema document names the same limits", () => {
  const schema = JSON.parse(readFileSync(join(here, "findings.schema.json"), "utf8"));
  assert.equal(schema.properties.findings.maxItems, LIMITS.findings);
  assert.equal(schema.$defs.change.oneOf[2].properties.keys.maxItems, LIMITS.trackKeysPerFinding);
  for (const n of ["256", "4,096", "262,144", "65,536", "8 MiB"]) assert.ok(schema.description.includes(n), n);
});
