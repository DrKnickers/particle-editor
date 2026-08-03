import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync, existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { join, dirname } from "node:path";
import { lintTimeline, collectTestids, KNOWN_UI_KINDS } from "./timeline-lint.mjs";

// Some fixtures live under tasks/ and .claude/, which the public manifest does
// NOT publish. This same test file DOES get published and is run by the public
// mirror's CI, where those paths are absent -- an unconditional readFileSync
// there throws ENOENT and the advertised sync gate can never go green. Skip the
// fixture-bound cases when the fixture is not present rather than weakening
// them: on the private repo they run exactly as before.
const needsFixture = (p) =>
  existsSync(p) ? false : `fixture not published to the public mirror: ${p}`;

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

const el = (ref) => ({ kind: "element", ref });
const pt = (x, y) => ({ kind: "point", x, y });

function base(tracks, extra = {}) {
  return { fps: 30, width: 1280, height: 960, durationMs: 6000, out: "clips/x", tracks, ...extra };
}

const rules = (r) => [...r.errors, ...r.warnings].map((f) => f.rule);
const errRules = (r) => r.errors.map((f) => f.rule);

test("press on a curve key is an error (the #517 wedge trap)", () => {
  const r = lintTimeline(base([
    { cursor: [{ t: 100, target: el("curve-key:red:0"), vis: true, press: true }] },
  ]));
  assert.ok(errRules(r).includes("curve-key-press"));
});

test("hovering a curve key of the focused channel is clean", () => {
  const r = lintTimeline(base([
    { cursor: [{ t: 100, target: el("curve-key:red:0"), vis: true, press: false }] },
  ]));
  assert.deepEqual(r.errors, []);
});

test("press on a spinner testid is an error (hold-repeat races)", () => {
  const r = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:spinner-tail-length"), vis: true, press: true }] },
  ]));
  assert.ok(errRules(r).includes("spinner-press"));
});

test("pressing a channel element of an UNfocused channel is an error; hover only warns", () => {
  const press = lintTimeline(base([
    { cursor: [{ t: 100, target: el("channel-row:blue"), vis: true, press: true, activate: true },
               { t: 200, target: el("channel-row:blue"), vis: true, press: false, activate: true }] },
  ]));
  assert.ok(errRules(press).includes("focus-order"));
  const hover = lintTimeline(base([
    { cursor: [{ t: 100, target: el("curve-key:blue:0"), vis: true, press: false }] },
  ]));
  assert.deepEqual(hover.errors, []);
  assert.ok(hover.warnings.some((w) => w.rule === "focus-order"));
});

test("ui/focus-channel before the target clears the focus-order check", () => {
  const r = lintTimeline(base([
    { at: 50, kind: "ui/focus-channel", params: { channel: "blue" } },
    { cursor: [{ t: 100, target: el("curve-key:blue:0"), vis: true, press: false }] },
  ]));
  assert.deepEqual(rules(r).filter((x) => x === "focus-order"), []);
});

test("atlas tile press without an open dock errors; short settle + missing select warn", () => {
  const closed = lintTimeline(base([
    { cursor: [{ t: 500, target: el("atlas-tile:2"), vis: true, press: true, activate: true },
               { t: 650, target: el("atlas-tile:2"), vis: true, press: false, activate: true }] },
  ]));
  assert.ok(errRules(closed).includes("atlas-order"));

  const rushed = lintTimeline(base([
    { at: 400, kind: "ui/show-panel", params: { panel: "atlas" } },
    { cursor: [{ t: 600, target: el("atlas-tile:2"), vis: true, press: true, activate: true },
               { t: 750, target: el("atlas-tile:2"), vis: true, press: false, activate: true }] },
  ]));
  assert.deepEqual(errRules(rushed).filter((x) => x === "atlas-order"), []);
  assert.ok(rushed.warnings.some((w) => w.rule === "atlas-order"));
  assert.ok(rushed.warnings.some((w) => w.rule === "atlas-select"));

  const clean = lintTimeline(base([
    { at: 100, kind: "ui/select-key", params: { track: "index", time: 0 } },
    { at: 200, kind: "ui/show-panel", params: { panel: "atlas" } },
    { cursor: [{ t: 1400, target: el("atlas-tile:2"), vis: true, press: true, activate: true },
               { t: 1550, target: el("atlas-tile:2"), vis: true, press: false, activate: true }] },
  ]));
  assert.deepEqual(rules(clean).filter((x) => x.startsWith("atlas")), []);
});

test("G/B/A track write after add-root without a set-track-lock errors; with the unlock it is clean", () => {
  const missing = lintTimeline(base([
    { at: 100, kind: "emitters/add-root", params: {} },
    { at: 300, kind: "emitters/set-track-key", params: { id: 1, track: "green", oldTime: 0, newTime: 0, newValue: 1 } },
  ]));
  assert.ok(errRules(missing).includes("track-lock"));

  const unlocked = lintTimeline(base([
    { at: 100, kind: "emitters/add-root", params: {} },
    { at: 200, kind: "emitters/set-track-lock", params: { id: 1, channel: "green", lockTo: null } },
    { at: 300, kind: "emitters/set-track-key", params: { id: 1, track: "green", oldTime: 0, newTime: 0, newValue: 1 } },
  ]));
  assert.deepEqual(errRules(unlocked), []);
});

test("activate release off the armed target warns (release containment cancels the click)", () => {
  const r = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:appearance-has-tail"), vis: true, press: true, activate: true },
               { t: 250, target: pt(400, 400), vis: true, press: false }] },
  ]));
  assert.ok(r.warnings.some((w) => w.rule === "activate-pair"));
});

test("an activate press near a scripted spawner/trigger warns about double-fire", () => {
  const r = lintTimeline(base([
    { at: 500, kind: "spawner/trigger", params: {} },
    { cursor: [{ t: 400, target: pt(170, 128), vis: true, press: true, activate: true },
               { t: 550, target: pt(170, 128), vis: true, press: false, activate: true }] },
  ]));
  assert.ok(r.warnings.some((w) => w.rule === "double-fire"));
});

test("file/save without saveRoot or params.path errors", () => {
  const r = lintTimeline(base([{ at: 100, kind: "file/save", params: {} }]));
  assert.equal(errRules(r).filter((x) => x === "save-root").length, 2);
});

test("mixed literal/target cursor keys, out-of-order keys, and dual cursor tracks error", () => {
  const mixed = lintTimeline(base([
    { cursor: [{ t: 0, x: 1, y: 2, vis: false, press: false },
               { t: 100, target: el("channel-row:red"), vis: true, press: false }] },
  ]));
  assert.ok(errRules(mixed).includes("cursor-mixed"));

  const order = lintTimeline(base([
    { cursor: [{ t: 200, target: pt(1, 1), vis: false, press: false },
               { t: 100, target: pt(1, 1), vis: false, press: false }] },
  ]));
  assert.ok(errRules(order).includes("cursor-order"));

  const dual = lintTimeline(base([{ cursor: [] }, { cursor: [] }]));
  assert.ok(errRules(dual).includes("cursor-track"));
});

test("spawner lifetime ending before the clip warns, measured from the trigger", () => {
  const dim = lintTimeline(base([
    { at: 100, kind: "spawner/start", params: { maxLifetimeSec: 2 } },
    { at: 200, kind: "spawner/trigger", params: {} },
  ]));
  assert.ok(dim.warnings.some((w) => w.rule === "lifetime"));

  // A late trigger whose instance outlives the clip end is clean (tutorial-3 shape).
  const late = lintTimeline(base([
    { at: 4000, kind: "spawner/start", params: { maxLifetimeSec: 3 } },
    { at: 4200, kind: "spawner/trigger", params: {} },
  ]));
  assert.deepEqual(rules(late).filter((x) => x === "lifetime"), []);
});

test("fps must divide 60", () => {
  const r = lintTimeline(base([], { fps: 24 }));
  assert.ok(errRules(r).includes("fps"));
});

// Press steps from the UPCOMING key while position eases from the PREVIOUS one,
// so a false->true edge fires the pointerdown at the departure target.
test("a press engaging while departing a curve-key/spinner hover is an error (interval semantics)", () => {
  const curve = lintTimeline(base([
    { cursor: [{ t: 100, target: el("curve-key:red:0"), vis: true, press: false },
               { t: 500, target: pt(400, 700), vis: true, press: true },
               { t: 650, target: pt(400, 700), vis: true, press: false }] },
  ]));
  assert.ok(errRules(curve).includes("curve-key-press"));

  const spinner = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:spinner-tail-length"), vis: true, press: false },
               { t: 500, target: pt(400, 700), vis: true, press: true },
               { t: 650, target: pt(400, 700), vis: true, press: false }] },
  ]));
  assert.ok(errRules(spinner).includes("spinner-press"));
});

test("a press engaging mid-travel between different targets warns; a same-target approach is clean", () => {
  const midTravel = lintTimeline(base([
    { cursor: [{ t: 100, target: pt(50, 50), vis: true, press: false },
               { t: 500, target: pt(400, 700), vis: true, press: true },
               { t: 650, target: pt(400, 700), vis: true, press: false }] },
  ]));
  assert.ok(midTravel.warnings.some((w) => w.rule === "press-transition"));

  const approached = lintTimeline(base([
    { cursor: [{ t: 100, target: pt(400, 700), vis: true, press: false },
               { t: 500, target: pt(400, 700), vis: true, press: true },
               { t: 650, target: pt(400, 700), vis: true, press: false }] },
  ]));
  assert.deepEqual(rules(approached).filter((x) => x === "press-transition"), []);
});

test("a focus event at the SAME timestamp as the cursor key is too late (tick precedes at-events)", () => {
  const r = lintTimeline(base([
    { at: 100, kind: "ui/focus-channel", params: { channel: "blue" } },
    { cursor: [{ t: 100, target: el("curve-key:blue:0"), vis: true, press: false }] },
  ]));
  assert.ok(r.warnings.some((w) => w.rule === "focus-order"));
});

test("lifetime births come from triggers (manual mode) and only the LAST birth's expiry matters", () => {
  // manual start with NO trigger spawns nothing — no warning
  const noTrigger = lintTimeline(base([
    { at: 100, kind: "spawner/start", params: { mode: "manual", maxLifetimeSec: 0.5 } },
  ]));
  assert.deepEqual(rules(noTrigger).filter((x) => x === "lifetime"), []);

  // repeated triggers: earlier instances dying mid-clip is fine when the last one reaches the end
  const repeated = lintTimeline(base([
    { at: 100, kind: "spawner/start", params: { mode: "manual", maxLifetimeSec: 2 } },
    { at: 200, kind: "spawner/trigger", params: {} },
    { at: 2500, kind: "spawner/trigger", params: {} },
    { at: 4500, kind: "spawner/trigger", params: {} }, // 4500 + 2000 >= 6000
  ]));
  assert.deepEqual(rules(repeated).filter((x) => x === "lifetime"), []);
});

test("KNOWN_UI_KINDS stays in sync with the C++ record allowlist (greps ClipTimeline.h)", () => {
  const header = readFileSync(join(repoRoot, "src", "host", "ClipTimeline.h"), "utf8");
  const fromHeader = new Set([...header.matchAll(/kind == "(ui\/[^"]+)"/g)].map((m) => m[1]));
  assert.deepEqual([...fromHeader].sort(), [...KNOWN_UI_KINDS].sort());
});

test("an unknown ui/* kind is an error (fails the --record parse with exit 2)", () => {
  const r = lintTimeline(base([{ at: 100, kind: "ui/atlas-alpha", params: { on: true } }]));
  assert.ok(errRules(r).includes("ui-kind"));
});

test("unknown testid refs warn; exact ids and template-literal prefixes both legitimize", () => {
  const known = { exact: new Set(["toolbar"]), prefixes: ["tab-trigger-"] };
  const bogus = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:no-such-thing"), vis: true, press: false }] },
  ]), { knownTestids: known });
  assert.ok(bogus.warnings.some((w) => w.rule === "testid-unknown"));

  const good = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:toolbar"), vis: true, press: false },
               { t: 300, target: el("testid:tab-trigger-physics"), vis: true, press: false }] },
  ]), { knownTestids: known });
  assert.deepEqual(rules(good).filter((x) => x === "testid-unknown"), []);
});

test("collectTestids finds literal, prop-passed, and template-prefix testids in the real web source", () => {
  const known = collectTestids(join(repoRoot, "web", "apps", "editor", "src"));
  // literal data-testid, prop testId=, and a template prefix respectively
  assert.ok(known.exact.has("toolbar"));
  assert.ok(known.exact.has("emitter-name-input"));
  assert.ok(known.exact.has("ce-spinner-value-wrapper"));
  assert.ok(known.prefixes.some((p) => "emitter-row:0".startsWith(p)));
  // test-only ids must NOT legitimize timeline refs
  assert.ok(!known.exact.has("atlas-alpha-toggle"));
  // multiline conditional template (data-testid={ testId ? `${testId}-option-${v}` : undefined })
  // legitimizes derived option ids via its static infix
  const r = lintTimeline(base([
    { cursor: [{ t: 100, target: el("testid:appearance-blend-mode-trigger-option-11"), vis: true, press: false }] },
  ]), { knownTestids: known });
  assert.deepEqual(r.warnings.filter((w) => w.rule === "testid-unknown"), []);
});

test("a same-timestamp ui/select-key does not satisfy the atlas select-before-click check", () => {
  const r = lintTimeline(base([
    { at: 200, kind: "ui/show-panel", params: { panel: "atlas" } },
    { at: 1400, kind: "ui/select-key", params: { track: "index", time: 0 } },
    { cursor: [{ t: 1400, target: el("atlas-tile:2"), vis: true, press: true, activate: true },
               { t: 1550, target: el("atlas-tile:2"), vis: true, press: false, activate: true }] },
  ]));
  assert.ok(r.warnings.some((w) => w.rule === "atlas-select"));
});

const PILOT_TIMELINE = join(repoRoot, "tasks", "wiki-media", "tutorials", "03-laser-shot", "projectile-core.timeline.json");
const SKILL_TEMPLATE = join(repoRoot, ".claude", "skills", "clip-author", "template.timeline.json");

test("the tutorial-3 pilot's testid refs all resolve against the real web source", { skip: needsFixture(PILOT_TIMELINE) }, () => {
  const t = JSON.parse(readFileSync(
    join(repoRoot, "tasks", "wiki-media", "tutorials", "03-laser-shot", "projectile-core.timeline.json"), "utf8"));
  const known = collectTestids(join(repoRoot, "web", "apps", "editor", "src"));
  const r = lintTimeline(t, { knownTestids: known });
  assert.deepEqual(r.errors, []);
  assert.deepEqual(r.warnings.filter((w) => w.rule === "testid-unknown"), []);
});

// Regression anchors: the committed interaction-honest pilot and the skill's
// starter template must lint with ZERO errors (warnings allowed but asserted).
test("the tutorial-3 projectile-core pilot lints clean", { skip: needsFixture(PILOT_TIMELINE) }, () => {
  const t = JSON.parse(readFileSync(
    join(repoRoot, "tasks", "wiki-media", "tutorials", "03-laser-shot", "projectile-core.timeline.json"), "utf8"));
  const r = lintTimeline(t);
  assert.deepEqual(r.errors, []);
});

test("the clip-author skill template lints clean", { skip: needsFixture(SKILL_TEMPLATE) }, () => {
  const t = JSON.parse(readFileSync(
    join(repoRoot, ".claude", "skills", "clip-author", "template.timeline.json"), "utf8"));
  const r = lintTimeline(t);
  assert.deepEqual(r.errors, []);
});
