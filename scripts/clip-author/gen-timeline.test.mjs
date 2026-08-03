import { test } from "node:test";
import assert from "node:assert/strict";
import { generateTimeline, parseGenArgs } from "./gen-timeline.mjs";
import { lintTimeline } from "../clip-verify/timeline-lint.mjs";

const BASE = { fps: 60, width: 1280, height: 960, durationMs: 20000, out: "clips/gen-test", park: { x: 700, y: 500 } };

const atEvents = (tl) => tl.tracks.filter((t) => typeof t.at === "number");
const cursorKeys = (tl) => tl.tracks.find((t) => Array.isArray(t.cursor))?.cursor ?? [];

test("type beat expands to a focus-click triple plus prefix patches at the char cadence", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 3000, do: "type", ref: "testid:emitter-name-input", text: "Core", charMs: 90 },
  ] });
  const patches = atEvents(tl).filter((e) => e.kind === "emitters/set-properties");
  assert.deepEqual(patches.map((p) => p.params.patch.name), ["C", "Co", "Cor", "Core"]);
  assert.deepEqual(patches.map((p) => p.at), [3700, 3790, 3880, 3970]);
  const clicks = cursorKeys(tl).filter((k) => k.activate === true);
  assert.equal(clicks.length, 2); // down + release, same target
  assert.ok(clicks.every((k) => k.target.ref === "testid:emitter-name-input"));
  assert.equal(clicks[0].press, true);
  assert.equal(clicks[1].press, false);
});

test("step beat emits absolute set-properties patches while the cursor hovers (never presses) the spinner", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 7000, do: "step", field: "tailSize", values: [55, 62, 70], stepMs: 400,
      hover: { ref: "testid:spinner-tail-length" } },
  ] });
  const patches = atEvents(tl).filter((e) => e.kind === "emitters/set-properties");
  assert.deepEqual(patches.map((p) => [p.at, p.params.patch.tailSize]), [[7000, 55], [7400, 62], [7800, 70]]);
  const spinnerKeys = cursorKeys(tl).filter((k) => k.target?.ref === "testid:spinner-tail-length");
  assert.ok(spinnerKeys.length >= 2);
  assert.ok(spinnerKeys.every((k) => k.press === false && k.activate !== true));
});

test("step-key beat emits set-track-key walks; select-key hover targets the curve-key ref without pressing", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 1000, do: "focus-channel", channel: "scale", click: true },
    { at: 2500, do: "select-key", track: "scale", time: 0, hover: true },
    { at: 3600, do: "step-key", track: "scale", keyTime: 0, values: [14, 8, 3], stepMs: 300,
      hover: { point: { x: 1185, y: 773 } } },
  ] });
  const setKeys = atEvents(tl).filter((e) => e.kind === "emitters/set-track-key");
  assert.deepEqual(setKeys.map((e) => e.params.newValue), [14, 8, 3]);
  assert.ok(setKeys.every((e) => e.params.oldTime === 0 && e.params.newTime === 0));
  const keyHovers = cursorKeys(tl).filter((k) => k.target?.ref === "curve-key:scale:0");
  assert.ok(keyHovers.length >= 2);
  assert.ok(keyHovers.every((k) => k.press === false && k.activate !== true));
});

test("cursor track gets hidden park bookends and stays time-ordered", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 5000, do: "click", ref: "testid:tab-trigger-appearance" },
  ] });
  const keys = cursorKeys(tl);
  assert.equal(keys[0].t, 0);
  assert.equal(keys[0].vis, false);
  assert.equal(keys[keys.length - 1].vis, false);
  for (let i = 1; i < keys.length; i++) assert.ok(keys[i].t >= keys[i - 1].t);
});

test("a beats-only spec with no cursor beats emits NO cursor track (cursor-free clips are legal)", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 100, do: "event", kind: "spawner/trigger", params: {} },
  ] });
  assert.equal(tl.tracks.find((t) => Array.isArray(t.cursor)), undefined);
});

test("a representative honest-style spec generates a timeline that LINTS with zero errors", () => {
  const tl = generateTimeline({
    ...BASE,
    durationMs: 30000,
    comment: "gen-test",
    prelude: [
      { at: 0, kind: "mods/set-layers", params: { paths: [] } },
      { at: 26000, kind: "spawner/start", params: { mode: "manual", enabled: true, burstSize: 1, position: [0, 0, 0], maxLifetimeSec: 30 } },
      { at: 26200, kind: "spawner/trigger", params: {} },
    ],
    beats: [
      { at: 3000, do: "type", ref: "testid:emitter-name-input", text: "Projectile", charMs: 90 },
      { at: 6000, do: "click", point: { x: 170, y: 128 } }, // commit-by-blur click elsewhere
      { at: 8000, do: "step", field: "nParticlesPerSecond", values: [2, 3, 4, 5], hover: { ref: "testid:spinner-particles-per-second" } },
      { at: 11000, do: "focus-channel", channel: "index", click: true },
      { at: 13000, do: "select-key", track: "index", time: 0, hover: true },
      { at: 13600, do: "show-panel", panel: "atlas" },
      { at: 15200, do: "click", ref: "atlas-tile:2" },
      { at: 17500, do: "focus-channel", channel: "scale", click: true },
      { at: 19000, do: "select-key", track: "scale", time: 0, hover: true },
      { at: 20200, do: "step-key", track: "scale", keyTime: 0, values: [14, 8, 3], hover: { point: { x: 1185, y: 773 } } },
      { at: 22500, do: "click", ref: "testid:tab-trigger-appearance" },
      { at: 24000, do: "step", field: "tailSize", values: [55, 62, 70], hover: { ref: "testid:spinner-tail-length" } },
    ],
  });
  const { errors, warnings } = lintTimeline(tl);
  assert.deepEqual(errors, []);
  // the walk above is designed to be warning-free too — any new warning is a
  // generator/linter drift worth looking at
  assert.deepEqual(warnings, []);
});

test("drag beat emits a grab-travel-drop press envelope with NO activate (a press is drag XOR activation)", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 5000, do: "drag", from: { ref: "testid:stack-row:2" }, to: { point: { x: 400, y: 300 } }, travelMs: 600 },
  ] });
  const keys = cursorKeys(tl).filter((k) => k.t >= 4000 && k.t <= 6500);
  const pressed = keys.filter((k) => k.press === true);
  assert.ok(pressed.length >= 3); // grab, travel, settle at minimum
  assert.ok(keys.every((k) => k.activate !== true));
  // grab is preceded by a same-target approach (press-transition rule)
  const grab = keys.find((k) => k.press === true);
  const before = keys[keys.indexOf(grab) - 1];
  assert.equal(before.press, false);
  assert.equal(before.target.ref, "testid:stack-row:2");
  // the drop releases at the destination
  const last = keys[keys.length - 1];
  assert.equal(last.press, false);
  assert.deepEqual(last.target, { kind: "point", x: 400, y: 300 });
  // and the whole thing is linter-clean
  assert.deepEqual(lintTimeline(tl).errors, []);
});

test("a spec without a comment auto-derives _comment from the beat table (incl. expect)", () => {
  const tl = generateTimeline({ ...BASE, beats: [
    { at: 3000, do: "type", ref: "testid:emitter-name-input", text: "Core", expect: "name reads Core" },
    { at: 8000, do: "step", field: "tailSize", values: [55, 70], hover: { ref: "testid:spinner-tail-length" } },
  ] });
  assert.ok(tl._comment.startsWith("beats: "));
  assert.ok(tl._comment.includes('3000 type testid:emitter-name-input "Core" => name reads Core'));
  assert.ok(tl._comment.includes("55→70"));
  // an explicit comment wins
  const explicit = generateTimeline({ ...BASE, comment: "hand-written", beats: [
    { at: 100, do: "event", kind: "spawner/trigger", params: {} },
  ] });
  assert.equal(explicit._comment, "hand-written");
});

test("parseGenArgs rejects a dangling/duplicate --out and unknown flags", () => {
  assert.deepEqual(parseGenArgs(["beats.json", "--out", "t.json"]), { input: "beats.json", outPath: "t.json" });
  assert.throws(() => parseGenArgs(["beats.json", "--out"]), /--out requires a path/);
  assert.throws(() => parseGenArgs(["beats.json", "--out", "--force"]), /--out requires a path/);
  assert.throws(() => parseGenArgs(["beats.json", "--out", "a", "--out", "b"]), /given twice/);
  assert.throws(() => parseGenArgs(["beats.json", "--frobnicate"]), /unknown option/);
  assert.throws(() => parseGenArgs(["a.json", "b.json"]), /exactly one/);
  assert.throws(() => parseGenArgs([]), /exactly one/);
});

test("validation fails loud: missing fields, bad fps, unknown beat, colliding cursor envelopes", () => {
  assert.throws(() => generateTimeline({ fps: 60 }), /missing required field/);
  assert.throws(() => generateTimeline({ ...BASE, fps: 24 }), /must divide 60/);
  assert.throws(() => generateTimeline({ ...BASE, beats: [{ at: 100, do: "warp" }] }), /unknown beat/);
  assert.throws(() => generateTimeline({ ...BASE, beats: [
    { at: 5000, do: "click", ref: "testid:a" },
    { at: 5100, do: "click", ref: "testid:b" }, // approach of b starts before a's release
  ] }), /collide/);
});
