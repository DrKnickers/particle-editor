import { test } from "node:test";
import assert from "node:assert/strict";
import { acceptTrack, atlasCells, isPerfectSquare, MAX_TRACK_KEYS, projectTrack, stepTrackEquivalent, trackErrors } from "./track.mjs";

const BASE = [[0, 0], [25, 1], [50, 2], [75, 3], [100, 3]];
const ok = (keys, base = BASE, cells = 4) => assert.equal(acceptTrack(base, cells, keys).ok, true, JSON.stringify(keys));
const no = (keys, re, base = BASE, cells = 4) => {
  const r = acceptTrack(base, cells, keys);
  assert.equal(r.ok, false, JSON.stringify(keys).slice(0, 80));
  if (re) assert.match(r.reason, re);
};

test("atlas cells follow the renderer's grid", () => {
  assert.equal(atlasCells(16), 16);
  assert.equal(atlasCells(6), 4);
  assert.equal(atlasCells(0), 1);
  assert.equal(isPerfectSquare(16), true);
  assert.equal(isPerfectSquare(6), false);
});

test("integer steps plus a quarter cell, and an extra key that changes no floor, play the same", () => {
  ok(BASE.map(([t, v]) => [t, v + 0.25]));
  ok([[0, 0], [10, 0.5], [25, 1], [50, 2], [75, 3], [100, 3]]);
  ok([[0, 0], [25, 1], [50, 2], [75, 3.99], [100, 3]]);
});

test("reversed, shifted or cell-changing proposals fail", () => {
  no([[0, 3], [25, 2], [50, 1], [75, 0], [100, 0]], /plays differently/);
  no(BASE.map(([t, v]) => [t, Math.min(v + 1, 3)]), /plays differently/);
  no([[0, 0], [25, 1], [40, 3], [50, 2], [75, 3], [100, 3]], /plays differently/);
  no([[0, 0], [25, 1], [50, 2], [75, 3], [99.5, 1], [100, 3]], /plays differently/);
});

test("a key exactly at a base key time follows the strictly-before cursor at both ends", () => {
  // The base changes value just above 25; a proposal that changes just above 30 differs on (25, 30].
  no([[0, 0], [30, 1], [50, 2], [75, 3], [100, 3]], /plays differently/);
  // The same change points, written with a duplicate-free extra key at 25: same playback.
  ok([[0, 0], [25, 1], [37.5, 1], [50, 2], [75, 3], [100, 3]]);
});

test("the terminal value is part of playback: [[0,0],[100,1]] differs from [[0,0],[100,2]]", () => {
  assert.equal(stepTrackEquivalent([[0, 0], [100, 1]], [[0, 0], [100, 2]]), false);
  assert.equal(stepTrackEquivalent([[0, 0], [100, 1]], [[0, 0], [100, 1.75]]), true);
});

test("structurally invalid proposals fail before equivalence", () => {
  no([[0, 0], [50, 1]], /last key/);
  no([[0, 0], [-10, 1], [100, 3]], /interior/);
  no([[5, 0], [25, 1], [100, 3]], /first key/);
  no([[0, 0]], /two keys/);
  no([[0, 0], [25, 1], [25, 2], [100, 3]], /strictly after/);
  no([[0, 0], [25, 1], [50, 2], [100, -1e30]], /atlas cells/);
  no([[0, 0], [25, 1], [50, 2], [100, 4]], /atlas cells/);
  assert.match(trackErrors([[0, 0], [100, NaN]], 4)[0], /finite/);
});

test("on a six-sprite atlas only four cells exist", () => {
  const base = [[0, 0], [100, 3]];
  no([[0, 0], [100, 5]], /atlas cells/, base, atlasCells(6));
  ok([[0, 0], [100, 3.9]], base, atlasCells(6));
});

test("the key-count limit admits 65,536 and refuses 65,537", () => {
  const keys = (n) => Array.from({ length: n }, (_, i) => [i === n - 1 ? 100 : (i * 100) / (n - 1), i === n - 1 ? 3 : Math.min(3, Math.floor((i * 4) / (n - 1)))]);
  assert.deepEqual(trackErrors(keys(MAX_TRACK_KEYS), 4), []);
  assert.match(trackErrors(keys(MAX_TRACK_KEYS + 1), 4)[0], /limit/);
});

test("times project through the save: 30 and 30.000001907348633 collide and fail after save", () => {
  const projected = projectTrack([[0, 0], [30, 1], [30.000001907348633, 1], [100, 3]]);
  assert.equal(projected[1][0], 30.000001907348633);
  assert.equal(projected[2][0], 30.000001907348633);
  no([[0, 0], [30, 1], [30.000001907348633, 1], [100, 3]], /after save/, [[0, 0], [30, 1], [100, 3]]);
});

test("the comparison uses the projection, not the raw keys", () => {
  // 0.99999999 stores as float32 1.0, which floors to cell 1, not 0.
  no([[0, 0], [25, 0.99999999], [50, 2], [75, 3], [100, 3]], undefined, [[0, 0], [25, 0], [50, 2], [75, 3], [100, 3]]);
  ok([[0, 0], [25, 0.99999999], [50, 2], [75, 3], [100, 3]]);
});
