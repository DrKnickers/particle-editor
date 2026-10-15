// [vfx-build] The flipbook track helpers and the pair check, on inspect JSON:
// no exe. fixtures/new-emitter.json is `--tool inspect`'s view of the
// editor's new emitter (a `--tool build` of one entry with no edits).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";
import { FPS, INDEX, LINEAR, ROTATION, SCALE, STEP, at, flipbookOps, frameAges, handOffSamples, indexKeys, linearKeys, sample, spanSamples } from "./tracks.mjs";
import { checkPairs, main } from "./pair-check.mjs";

const here = fileURLToPath(new URL(".", import.meta.url));
const NEW = JSON.parse(readFileSync(join(here, "fixtures", "new-emitter.json"), "utf8"));

// The early smoke and fire: 64 cells, one a game frame; the late smoke: 64
// cells from 1.1 s to 4.0 s. Spans grow with the sim.
const EARLY_LIFE = Math.fround(64 / FPS), LATE_LIFE = 4;
const earlySeconds = Array.from({ length: 64 }, (_, k) => k / FPS);
const lateSeconds = Array.from({ length: 64 }, (_, k) => Math.round((1.1 + (k * 2.9) / 63) * FPS) / FPS);
const span = (s) => 1 + 0.8 * s;
const earlySamples = earlySeconds.map((s) => [s, span(s)]);
const lateSamples = handOffSamples(earlySamples, EARLY_LIFE, lateSeconds.map((s) => [s, span(s)]));

// What `--tool build` makes of an entry's ops, near enough for these rules:
// the fields and tracks the ops name, on the new emitter.
function emitter(name, ops, extra = {}) {
  const e = structuredClone(NEW);
  e.name = name;
  e.parentLinkStrength = 1;
  for (const op of ops) {
    if (op.op === "setProperty") e[op.field] = Math.fround(op.value);
    if (op.op === "setInterpolation") e.tracks[op.track].interpolation = { linear: LINEAR, step: STEP }[op.interpolation];
    if (op.op === "replaceTrack") e.tracks[op.track].keys = op.keys.map(([t, v]) => [Math.fround(t), Math.fround(v)]);
  }
  return Object.assign(e, extra);
}
const early = () => emitter("T1_SMOKE_EARLY", flipbookOps({ cellSeconds: earlySeconds, samples: earlySamples, lifetime: EARLY_LIFE, size: 30, cells: 64 }));
const fire = () => emitter("T1_FIRE", flipbookOps({ cellSeconds: earlySeconds, samples: earlySamples, lifetime: EARLY_LIFE, size: 30, cells: 64 }));
const late = () => emitter("T1_SMOKE_LATE", flipbookOps({ cellSeconds: [0, ...lateSeconds.slice(1)], samples: lateSamples, lifetime: LATE_LIFE, size: 30, cells: 64 }));
const doc = (...emitters) => ({ schema: 1, name: "test", emitters });
const rules = (d) => checkPairs(d).triplets.flatMap((t) => t.violations.map((v) => v.rule));

test("the new-emitter fixture is a root with the defaults the rules start from", () => {
  assert.equal(NEW.parent, null);
  assert.equal(NEW.textureSize, 64);
  assert.equal(NEW.tracks[INDEX].interpolation, STEP);
  assert.equal(NEW.parentLinkStrength, 0);
});

test("sample reads a track as the engine does: at a key's time, a step still shows the earlier value", () => {
  const step = { interpolation: STEP, keys: [[0, 0], [50, 1], [100, 2]] };
  assert.deepEqual([0, 25, 50, 50.001, 99, 100].map((r) => sample(step, r)), [0, 0, 0, 1, 1, 1]);
  const linear = { interpolation: LINEAR, keys: [[0, 0], [50, 10], [100, 0]] };
  assert.deepEqual([0, 25, 50, 75, 100].map((r) => sample(linear, r)), [0, 5, 10, 5, 0]);
  assert.equal(sample(linear, 120), 0, "past the last key, the last value");
  assert.deepEqual(frameAges(3 / FPS), [0, 1 / FPS, 2 / FPS]);
});

test("indexKeys shows cell k from the game frame at its time, and ends on the last cell", () => {
  const keys = indexKeys(earlySeconds, EARLY_LIFE);
  const track = { interpolation: STEP, keys };
  const shown = frameAges(EARLY_LIFE).map((a) => Math.floor(at(track, a, EARLY_LIFE)));
  // 64/60 s as a float32 is a hair over 64 frames, so the particle lives into a 65th, still on cell 63.
  assert.deepEqual(shown.slice(0, 64), earlySeconds.map((_, k) => k), "one cell a frame, none skipped or doubled");
  assert.ok(shown.slice(64).every((c) => c === 63), "and the last cell holds to the end");
  assert.deepEqual(keys.at(-1), [100, 63], "the last key holds cell 63, never 64");
  // The late sheet: cell 0 held until its first change, each cell from its own frame.
  const lateKeys = indexKeys([0, ...lateSeconds.slice(1)], LATE_LIFE);
  const lateTrack = { interpolation: STEP, keys: lateKeys };
  for (let k = 1; k < 64; k++) {
    const f = Math.round(lateSeconds[k] * FPS);
    assert.equal(Math.floor(at(lateTrack, (f - 1) / FPS, LATE_LIFE)), k - 1, `cell ${k - 1} up to the frame before ${f}`);
    assert.equal(Math.floor(at(lateTrack, f / FPS, LATE_LIFE)), k, `cell ${k} from frame ${f}`);
  }
  assert.throws(() => indexKeys([0.1, 0.2], 1), /age 0/);
  assert.throws(() => indexKeys([0, 0.5, 0.5], 1), /not after/);
  assert.throws(() => indexKeys([0, 2], 1), /never shows/);
});

test("linearKeys holds past the last sample and cuts a sample past death at 100", () => {
  assert.deepEqual(linearKeys([[0, 1], [0.5, 2]], 1), [[0, 1], [50, 2], [100, 2]]);
  assert.deepEqual(linearKeys([[0, 1], [0.5, 2], [1.5, 4]], 1), [[0, 1], [50, 2], [100, 3]]);
  assert.throws(() => linearKeys([[0.1, 1]], 1), /from 0 s/);
  assert.deepEqual(spanSamples({ cellSeconds: [0, 0.5], cellSpan: [1, 2] }), [[0, 1], [0.5, 2]]);
  assert.throws(() => spanSamples({ cellSeconds: [0], cellSpan: [] }), /one length/);
  assert.deepEqual(handOffSamples([[0, 1], [0.5, 2]], 1, [[1.5, 3]]), [[0, 1], [0.5, 2], [1, 2], [1.5, 3]]);
  assert.throws(() => handOffSamples([[0, 1]], 1, [[1, 3]]), /after the early/);
});

test("a matched triplet passes the pair check", () => {
  const result = checkPairs(doc(early(), fire(), late()));
  assert.deepEqual(result.triplets.map((t) => [t.triplet, t.members, t.violations]), [["T1", { early: 0, fire: 1, late: 2 }, []]]);
  assert.equal(result.ok, true);
  assert.equal(checkPairs(doc(NEW)).triplets.length, 0, "an effect with no triplets has nothing to check");
});

test("each broken rule is named", () => {
  const cases = [
    ["randoms", () => doc(early(), Object.assign(fire(), { randomScalePerc: 0.2 }))],
    ["randoms", () => doc(Object.assign(early(), { randomRotationDirection: true }), fire())],
    ["randoms", () => { const f = fire(); f.groups[2] = { ...f.groups[2], type: 3, sphereRadius: 5 }; return doc(early(), f); }],
    ["shared", () => doc(early(), Object.assign(fire(), { gravity: -1 }))],
    ["shared", () => { const f = fire(); f.groups[0] = { ...f.groups[0], valZ: 2 }; return doc(early(), f); }],
    ["inherit", () => doc(Object.assign(early(), { parentLinkStrength: 0.5 }), Object.assign(fire(), { parentLinkStrength: 0.5 }))],
    ["tracks", () => { const f = fire(); f.tracks[SCALE].keys[5][1] += 1; return doc(early(), f); }],
    ["tracks", () => { const l = late(); l.tracks[SCALE].keys[3][1] *= 1.1; return doc(early(), fire(), l); }],
    ["rotation", () => { const e = early(), f = fire(); for (const x of [e, f]) x.tracks[ROTATION].keys = [[0, 1], [100, 1]]; return doc(e, f); }],
    ["cells", () => { const f = fire(); f.tracks[INDEX].keys[10][0] += 1; return doc(early(), f); }],
    ["lifetime", () => doc(early(), Object.assign(fire(), { lifetime: 1.2 }))],
    ["index", () => { const f = fire(), e = early(); for (const x of [e, f]) x.tracks[INDEX].keys.at(-1)[1] = 64; return doc(e, f); }],
    ["index", () => { const f = fire(), e = early(); for (const x of [e, f]) x.tracks[INDEX].keys.at(-1)[1] = 62; return doc(e, f); }],
    ["order", () => doc(fire(), early())],
    ["incomplete", () => doc(fire())],
    ["root", () => doc(early(), Object.assign(fire(), { parent: 0 }))],
  ];
  for (const [rule, make] of cases) assert.ok(rules(make()).includes(rule), `${rule}: ${JSON.stringify(rules(make()))}`);
});

test("main reads an inspect file and exits 0, 5, 2 or 3", () => {
  const lines = [];
  const say = (line) => lines.push(line);
  const dir = join(here, "fixtures");
  assert.equal(main(["--inspect", join(dir, "new-emitter.json")], { say }), 3, "an emitter alone is not an inspect document");
  assert.match(lines.at(-1), /^\[pair-check\] .*not an inspect document/);
  assert.equal(main([], { say }), 2);
  assert.equal(main(["a.alo", "--inspect", "b.json"], { say }), 2);
  assert.equal(main(["--inspect", join(dir, "absent.json")], { say }), 3);
});
