import { test } from "node:test";
import assert from "node:assert/strict";
import { detectIsolatedDips } from "./partial-scan.mjs";

test("detectIsolatedDips flags a frame below both neighbors", () => {
  assert.deepEqual(detectIsolatedDips([10, 10, 2, 10, 10], 3.0), [
    { index: 2, yavg: 2, prev: 10, next: 10 },
  ]);
});

test("detectIsolatedDips returns no dips for a flat sequence", () => {
  assert.deepEqual(detectIsolatedDips([10, 10, 10], 3.0), []);
});

test("detectIsolatedDips never flags the first or last frame", () => {
  assert.deepEqual(detectIsolatedDips([2, 10, 10], 3.0), []);
  assert.deepEqual(detectIsolatedDips([10, 10, 2], 3.0), []);
});
