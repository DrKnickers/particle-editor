import { test } from "node:test";
import assert from "node:assert/strict";
import { evaluateSidecar } from "./cursor-on-target.mjs";

test("evaluateSidecar flags a press frame whose target did not resolve", () => {
  const result = evaluateSidecar([
    { frame: 7, cursor: { x: 0, y: 0, vis: true, press: true }, resolved: [{ ref: "atlas-tile:2", x: 10, y: 10, ok: false }] },
  ]);
  assert.deepEqual(result.pressMisses, [{ frame: 7, ref: "atlas-tile:2" }]);
  assert.equal(result.stats.pressFrames, 1);
});

test("evaluateSidecar treats a NON-press transit miss as benign (hidden cursor)", () => {
  const result = evaluateSidecar([
    { frame: 3, cursor: { x: 0, y: 0, vis: false, press: false }, resolved: [{ ref: "curve-key:blue:0", x: 0, y: 0, ok: false }] },
  ]);
  assert.deepEqual(result.pressMisses, []);
  assert.equal(result.stats.hiddenTransit, 1);
});

test("evaluateSidecar does NOT distance-check a pressed cursor (no false positive mid-travel)", () => {
  // cursor far from the resolved (origin) target while approaching a grab — fine.
  const result = evaluateSidecar([
    { frame: 4, cursor: { x: 0, y: 0, vis: true, press: true }, resolved: [{ ref: "channel-row:blue", x: 200, y: 0, ok: true }] },
  ]);
  assert.deepEqual(result.pressMisses, []);
});

test("evaluateSidecar accepts a clean clip and tallies refs", () => {
  const result = evaluateSidecar([
    { frame: 1, cursor: { x: 0, y: 0, vis: true, press: false }, resolved: [{ ref: "curve-key:red:0", x: 2, y: 0, ok: true }] },
    { frame: 2, cursor: { x: 2, y: 0, vis: true, press: true }, resolved: [{ ref: "curve-key:red:0", x: 2, y: 0, ok: true }] },
  ]);
  assert.deepEqual(result.pressMisses, []);
  assert.equal(result.stats.frames, 2);
  assert.deepEqual(result.stats.refs, ["curve-key:red:0"]);
});
