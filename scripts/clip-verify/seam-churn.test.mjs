import test from "node:test";
import assert from "node:assert/strict";
import { parseRoi, churnRatio, churnVerdict } from "./seam-churn.mjs";

test("parseRoi parses W:H:X:Y", () => {
  assert.deepEqual(parseRoi("1264:300:8:0"), { w: 1264, h: 300, x: 8, y: 0 });
});
test("parseRoi rejects malformed", () => {
  assert.throws(() => parseRoi("1:2:3"));
});
test("churnRatio normalizes a mean-abs-diff to 0..1", () => {
  // mean abs luma diff 25.5 over 255 → 0.1
  assert.ok(Math.abs(churnRatio(25.5) - 0.1) < 1e-9);
});
test("churnVerdict fails above the threshold", () => {
  assert.equal(churnVerdict(0.12, 0.10).ok, false);
  assert.equal(churnVerdict(0.08, 0.10).ok, true);
});
