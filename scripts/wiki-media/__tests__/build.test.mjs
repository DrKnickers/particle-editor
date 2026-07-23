import { test } from "node:test";
import assert from "node:assert/strict";
import {
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  buildGatePlan,
  runBatch,
  validateManifest,
} from "../build.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = join(__dirname, "..", "..", "..");
const fixtureDir = join(__dirname, "fixtures");
const mockTool = join(fixtureDir, "mock-exe.mjs");
const tmpRoot = join(__dirname, ".tmp");

function tempDir(t) {
  mkdirSync(tmpRoot, { recursive: true });
  const dir = mkdtempSync(join(tmpRoot, "case-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  return dir;
}

function writeJson(file, value) {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, JSON.stringify(value, null, 2));
}

// Every synthesized timeline carries the reference-object clears the runner
// now REQUIRES (tutorial media must never show the default AT_ST_Walker).
const REF_CLEAR_TRACKS = [
  { at: 0, kind: "engine/set/reference-object", params: { name: "" } },
  { at: 30, kind: "engine/set/reference-object-visible", params: { visible: false } },
];

function writeTimeline(dir, name, fields = {}) {
  const file = join(dir, name + ".timeline.json");
  const { tracks = [], ...rest } = fields;
  writeJson(file, {
    fps: 60,
    width: 1280,
    height: 960,
    durationMs: 1000,
    out: join(dir, "frames", name),
    tracks: [...REF_CLEAR_TRACKS, ...tracks],
    ...rest,
  });
  return file;
}

function clip(overrides = {}) {
  return {
    id: "clip-pass",
    kind: "clip",
    timeline: "unused.timeline.json",
    output: "wiki/clip-pass.mp4",
    poster: "wiki/clip-pass.jpg",
    framing: "full-app",
    loop: "none",
    encode: { posterFrame: 0 },
    acceptance: ["accept clip"],
    ...overrides,
  };
}

function configFor(dir) {
  return {
    exe: join(dir, "missing-ParticleEditor.exe"),
    focMods: join(dir, "Mods"),
    extraction: join(dir, "DATA"),
    outDir: join(dir, "out"),
  };
}

function mockCommand(mode) {
  return { bin: process.execPath, prefixArgs: [mockTool, mode] };
}

function mockTools(logPath) {
  return {
    render: mockCommand("render"),
    cursor: mockCommand("cursor"),
    partial: mockCommand("partial"),
    seam: mockCommand("seam"),
    encode: mockCommand("encode"),
    ffmpeg: mockCommand("ffmpeg"),
    env: { WIKI_MEDIA_MOCK_LOG: logPath },
  };
}

async function runFixtureBatch(t, manifest, options = {}) {
  const dir = options.dir ?? tempDir(t);
  const manifestPath = join(dir, "manifest.json");
  const reportPath = join(dir, "report.json");
  const logPath = join(dir, "mock.log");
  writeJson(manifestPath, manifest);
  const result = await runBatch({
    repoRoot,
    manifestPath,
    reportPath,
    config: options.config ?? configFor(dir),
    args: {
      skipStage: true,
      ...(options.args ?? {}),
    },
    tools: options.tools ?? mockTools(logPath),
    preflight: false,
  });
  return {
    ...result,
    dir,
    logPath,
    reportPath,
    report: JSON.parse(readFileSync(reportPath, "utf8")),
  };
}

test("manifest validation rejects removed v1 postprocess and missing/unknown v2 framing or loop", () => {
  assert.match(
    validateManifest({ items: [clip({ postprocess: { kind: "zoom-pan" } })] }).join("\n"),
    /postprocess/,
  );
  assert.match(
    validateManifest({ items: [clip({ framing: undefined })] }).join("\n"),
    /missing required framing/,
  );
  assert.match(
    validateManifest({ items: [clip({ loop: undefined })] }).join("\n"),
    /missing required loop/,
  );
  assert.match(
    validateManifest({ items: [clip({ loop: "replay" })] }).join("\n"),
    /unknown loop/,
  );
  assert.deepEqual(
    validateManifest({ items: [clip({ encode: {} })] }),
    [],
    "missing encode.posterFrame is deferred to the render/encode pipeline",
  );
});

test("gate selection follows loop mode and target-bearing cursor tracks", () => {
  const item = clip({
    id: "crossfade-target",
    loop: "crossfade",
    encode: { start: 12, crossfade: 0.75, fps: 60, crop: "200:100:4:5", posterFrame: 30 },
  });
  const targetTimeline = {
    tracks: [{ kind: "cursor", points: [{ at: 1, target: "curve-key:red:0" }] }],
  };
  const plan = buildGatePlan({
    item,
    timeline: targetTimeline,
    frameDir: "frames",
    sidecarPath: "frames/cursor-sidecar.json",
    repoRoot,
  });

  assert.equal(plan.statuses["cursor-on-target"], "pending");
  assert.equal(plan.statuses["partial-scan"], "pending");
  assert.equal(plan.statuses["seam-churn"], "pending");
  assert.equal(plan.statuses["lull-luma"], "skipped");
  assert.ok(plan.commands.find((command) => command.name === "cursor-on-target"));
  assert.ok(plan.commands.find((command) => command.name === "partial-scan"));
  const seam = plan.commands.find((command) => command.name === "seam-churn");
  assert.ok(seam.command.includes("--start 12"), seam.command);
  assert.ok(seam.command.includes("--crossfade-frames 45"), seam.command);
  assert.ok(seam.command.includes("--roi 200:100:4:5"), seam.command);

  const literalCursor = buildGatePlan({
    item: clip({ id: "literal", loop: "pingpong" }),
    timeline: { tracks: [{ kind: "cursor", points: [{ at: 1, x: 10, y: 20 }] }] },
    frameDir: "frames",
    sidecarPath: "frames/cursor-sidecar.json",
    repoRoot,
  });
  assert.equal(literalCursor.statuses["cursor-on-target"], "skipped");
  assert.equal(literalCursor.statuses["partial-scan"], "pending");
  assert.equal(literalCursor.statuses["seam-churn"], "skipped");
});

test("mutates:true copies each stageInput to the scratch root under its BASENAME (matches the flat path the timeline opens)", async (t) => {
  const dir = tempDir(t);
  const config = configFor(dir);
  const base = join(config.focMods, "ParticleTutorial");
  // Input lives under a _stages/ subdir (like the real t1-green stage), but the
  // mutating clip's timeline opens the flat _scratch/<id>/<basename> path — so the
  // runner must flatten to basename, NOT preserve the _stages/ prefix.
  const stagedInput = join(base, "_stages", "t1-green.alo");
  const scratchFlat = join(base, "_scratch", "mutating", "t1-green.alo");
  const scratchNested = join(base, "_scratch", "mutating", "_stages", "t1-green.alo");
  mkdirSync(dirname(stagedInput), { recursive: true });
  mkdirSync(join(base, "_scratch", "mutating"), { recursive: true });
  writeFileSync(stagedInput, "fresh stage input");
  writeFileSync(scratchFlat, "stale scratch input");

  const timeline = writeTimeline(dir, "mutating");
  const result = await runFixtureBatch(t, {
    version: 2,
    items: [clip({
      id: "mutating",
      timeline,
      mutates: true,
      stageInputs: ["_stages/t1-green.alo"],
    })],
  }, { dir, config });

  assert.equal(result.exitCode, 0);
  // The flat path (what the timeline actually opens) is refreshed...
  assert.equal(readFileSync(scratchFlat, "utf8"), "fresh stage input");
  // ...and the old nested location is NOT created.
  assert.equal(existsSync(scratchNested), false, "must not preserve the _stages/ prefix under scratch");
});

test("report schema shape is stable for PASS, FAIL, and manual PENDING entries", async (t) => {
  const dir = tempDir(t);
  const passTimeline = writeTimeline(dir, "pass");
  const failTimeline = writeTimeline(dir, "fail", { mockExitCode: 9 });

  const result = await runFixtureBatch(t, {
    version: 2,
    items: [
      clip({ id: "pass", timeline: passTimeline }),
      clip({ id: "fail", timeline: failTimeline }),
      {
        id: "manual",
        kind: "image",
        manual: true,
        output: "manual.jpg",
        acceptance: ["manual acceptance"],
      },
    ],
  }, { dir });

  assert.equal(result.exitCode, 1);
  assert.deepEqual(result.report.map((entry) => entry.status), ["PASS", "FAIL", "PENDING"]);
  for (const entry of result.report) {
    assert.deepEqual(Object.keys(entry), [
      "id",
      "status",
      "commands",
      "exit",
      "gates",
      "outputs",
      "acceptance",
      "rerender",
    ]);
    assert.deepEqual(Object.keys(entry.commands), ["render", "verify", "encode"]);
    assert.deepEqual(Object.keys(entry.gates), [
      "cursor-on-target",
      "partial-scan",
      "seam-churn",
      "lull-luma",
      "readability",
      "no-reference-object",
    ]);
  }
  assert.equal(result.report[0].gates["partial-scan"], "pass");
  assert.deepEqual(result.report[0].exit, { render: 0 });
  assert.deepEqual(result.report[1].exit, { render: 9 });
  assert.deepEqual(result.report[2].exit, {});
  assert.deepEqual(result.report[2].outputs, []);
});

test("a clip missing encode.posterFrame fails during the item encode step without aborting the batch", async (t) => {
  const dir = tempDir(t);
  const missingPosterTimeline = writeTimeline(dir, "missing-poster");
  const afterTimeline = writeTimeline(dir, "after");

  const result = await runFixtureBatch(t, {
    version: 2,
    items: [
      clip({ id: "missing-poster", timeline: missingPosterTimeline, encode: {} }),
      clip({ id: "after", timeline: afterTimeline }),
    ],
  }, { dir });

  assert.equal(result.exitCode, 1);
  assert.deepEqual(result.report.map((entry) => [entry.id, entry.status]), [
    ["missing-poster", "FAIL"],
    ["after", "PASS"],
  ]);
  assert.equal(result.report[0].exit.encode, 1);
});

test("encode.zoom threads through to the encode command with the post-trim frame size", async (t) => {
  const dir = tempDir(t);
  const zoomTimeline = writeTimeline(dir, "zoomed");
  const segments = [{ t0: 2, t1: 6, rect: [0, 96, 632, 475], easeMs: 400 }];

  const result = await runFixtureBatch(t, {
    version: 2,
    items: [
      clip({
        id: "zoomed",
        timeline: zoomTimeline,
        encode: { start: 30, posterFrame: 120, crop: "1264:950:0:0", zoom: segments },
      }),
    ],
  }, { dir });

  assert.equal(result.exitCode, 0);
  const encodeCommand = result.report[0].commands.encode;
  assert.ok(encodeCommand.includes("--zoom"), "encode command must carry --zoom");
  // frame size comes from the chrome-trim crop, segments pass through verbatim
  const m = encodeCommand.match(/--zoom "?(\{.*?\})"?(?: |$)/);
  assert.ok(m, "zoom JSON present: " + encodeCommand);
  const zoomArg = JSON.parse(m[1].replace(/\\"/g, '"'));
  assert.equal(zoomArg.w, 1264);
  assert.equal(zoomArg.h, 950);
  assert.deepEqual(zoomArg.segments, segments);
});

test("--only filtering limits the processed ids", async (t) => {
  const dir = tempDir(t);
  const keepTimeline = writeTimeline(dir, "keep");
  const dropTimeline = writeTimeline(dir, "drop");

  const result = await runFixtureBatch(t, {
    version: 2,
    items: [
      clip({ id: "keep", timeline: keepTimeline }),
      clip({ id: "drop", timeline: dropTimeline }),
    ],
  }, { dir, args: { only: new Set(["keep"]) } });

  assert.equal(result.exitCode, 0);
  assert.deepEqual(result.report.map((entry) => entry.id), ["keep"]);
  assert.equal((readFileSync(result.logPath, "utf8").match(/mode=render/g) ?? []).length, 1);
});

test("--dry-run uses config.example fallback, skips child invocations, and still writes a report", async (t) => {
  const dir = tempDir(t);
  const reportPath = join(dir, "dry-report.json");
  const logPath = join(dir, "mock.log");
  const result = await runBatch({
    repoRoot,
    manifestPath: join(fixtureDir, "manifest.json"),
    reportPath,
    configLocalPath: join(dir, "missing-config.local.json"),
    configExamplePath: join(repoRoot, "scripts", "wiki-media", "config.example.json"),
    args: { dryRun: true },
    tools: mockTools(logPath),
    preflight: false,
  });

  assert.equal(result.exitCode, 0);
  assert.equal(existsSync(logPath), false, "dry-run must not invoke the mock subprocesses");
  const report = JSON.parse(readFileSync(reportPath, "utf8"));
  assert.deepEqual(report.map((entry) => [entry.id, entry.status]), [["fixture-pass", "PENDING"]]);
});

test("a clip timeline without the reference-object clears fails the no-reference-object gate", async () => {
  const { validateTimelineNoReferenceObject } = await import("../build.mjs");
  const bare = { tracks: [{ at: 0, kind: "mods/set-layers", params: { paths: [] } }] };
  const errors = validateTimelineNoReferenceObject(bare, "x");
  assert.equal(errors.length, 2, "missing clear AND hide must both be reported");
  const good = { tracks: [
    { at: 0, kind: "engine/set/reference-object", params: { name: "" } },
    { at: 30, kind: "engine/set/reference-object-visible", params: { visible: false } },
  ] };
  assert.deepEqual(validateTimelineNoReferenceObject(good, "x"), []);
  const wrongName = { tracks: [
    { at: 0, kind: "engine/set/reference-object", params: { name: "AT_ST_Walker" } },
    { at: 30, kind: "engine/set/reference-object-visible", params: { visible: false } },
  ] };
  // Setting a real name fails on BOTH counts: no clear present AND a non-empty name set.
  assert.equal(validateTimelineNoReferenceObject(wrongName, "x").length, 2, "setting a real reference object must not count as a clear");
  assert.deepEqual(
    validateTimelineNoReferenceObject(wrongName, "x", true),
    [],
    "an explicit manifest opt-in permits media whose subject is the reference-object feature",
  );
  // Clears present but a LATER event re-sets/re-shows the reference object → still fails.
  const clearedThenReadded = { tracks: [
    { at: 0, kind: "engine/set/reference-object", params: { name: "" } },
    { at: 30, kind: "engine/set/reference-object-visible", params: { visible: false } },
    { at: 5000, kind: "engine/set/reference-object", params: { name: "AT_ST_Walker" } },
    { at: 5030, kind: "engine/set/reference-object-visible", params: { visible: true } },
  ] };
  assert.equal(validateTimelineNoReferenceObject(clearedThenReadded, "x").length, 2, "re-adding after clearing must fail");
});

test("the returning-user scene-context clip explicitly permits its reference object", async () => {
  const { validateTimelineNoReferenceObject } = await import("../build.mjs");
  const manifest = JSON.parse(readFileSync(join(repoRoot, "tasks", "wiki-media", "manifest.json"), "utf8"));
  const item = manifest.items.find(({ id }) => id === "ref-returning-scene-context");
  assert.ok(item, "ref-returning-scene-context must remain in the media manifest");
  assert.equal(item.allowReferenceObject, true, "the manifest must opt this feature clip into reference-object rendering");

  const timeline = JSON.parse(readFileSync(join(repoRoot, item.timeline), "utf8"));
  assert.ok(
    validateTimelineNoReferenceObject(timeline, item.id).length > 0,
    "the reused timeline must exercise a real reference object",
  );
  assert.deepEqual(validateTimelineNoReferenceObject(timeline, item.id, item.allowReferenceObject), []);
});

test("a failing verify gate records gate=fail, does not abort the batch, and yields a non-zero exit", async (t) => {
  const dir = tempDir(t);
  const logPath = join(dir, "mock.log");
  // Inject the partial-scan verifier as a mock that exits non-zero (batch-wide).
  const failTools = { ...mockTools(logPath), env: { WIKI_MEDIA_MOCK_LOG: logPath, WIKI_MEDIA_MOCK_FAIL: "partial" } };
  const first = writeTimeline(dir, "first");
  const second = writeTimeline(dir, "second");
  const result = await runFixtureBatch(t, {
    version: 2,
    items: [
      clip({ id: "first", timeline: first }),
      clip({ id: "second", timeline: second }),
    ],
  }, { dir, tools: failTools });

  const byId = Object.fromEntries(result.report.map((e) => [e.id, e]));
  // Gate failure is recorded (not a silent pass) and marks the item FAIL.
  assert.equal(byId["first"].status, "FAIL");
  assert.equal(byId["first"].gates["partial-scan"], "fail");
  // The SECOND item still has a report entry -> the batch did not abort on the first FAIL.
  assert.ok(byId["second"], "batch must process every item despite a per-item gate failure");
  assert.equal(byId["second"].gates["partial-scan"], "fail");
  // Any FAIL -> non-zero batch exit.
  assert.notEqual(result.exitCode, 0);
});

// Planned-backlog exemption (2026-07-10): a status:"planned" item with no timeline
// is a production TODO — it must not wedge manifest validation, but the carve-out
// must stay narrow: an exact-match status only, and only while the timeline is absent.
test("validateManifest exempts planned items without a timeline — and nothing else", async () => {
  const { validateManifest } = await import("../build.mjs");
  const base = { kind: "clip", publish: true, output: "x.mp4", poster: "x-poster.jpg", framing: "full-app", loop: "none" };
  // exempt: planned + no timeline
  assert.deepEqual(validateManifest({ items: [{ id: "a", status: "planned" }] }), []);
  // NOT exempt: typo'd status still fails loudly on the missing timeline
  const typo = validateManifest({ items: [{ id: "b", status: "Planned" }] });
  assert.ok(typo.some((e) => e.includes("missing required timeline")), typo.join("; "));
  // NOT exempt: planned WITH a timeline gets the full field requirements
  const withTl = validateManifest({ items: [{ id: "c", status: "planned", timeline: "t.json", kind: "clip" }] });
  assert.ok(withTl.some((e) => e.includes("missing required")), withTl.join("; "));
  // a fully-specified rendered item still validates clean
  assert.deepEqual(validateManifest({ items: [{ id: "d", status: "rendered", timeline: "t.json", ...base }] }), []);
});

test("the returning-user curve clip keeps its live particle payoff visible", () => {
  const manifest = JSON.parse(readFileSync(join(repoRoot, "tasks", "wiki-media", "manifest.json"), "utf8"));
  const item = manifest.items.find(({ id }) => id === "ref-curve-visibility");
  assert.ok(item, "ref-curve-visibility must remain in the media manifest");

  const timeline = JSON.parse(readFileSync(join(repoRoot, item.timeline), "utf8"));
  assert.ok(
    timeline.tracks.some(({ kind }) => kind === "spawner/start"),
    "the clip must create a live preview instance",
  );
  assert.ok(
    timeline.tracks.some(({ kind }) => kind === "spawner/trigger"),
    "the clip must trigger the particle system",
  );
  assert.ok(
    timeline.tracks.some(({ kind, params }) => kind === "engine/set/ground" && params?.enabled === false),
    "the clip must disable the persisted ground plane for a neutral viewport",
  );
  assert.ok(
    timeline.tracks.some(({ kind, params }) => kind === "engine/set/grid-visible" && params?.visible === false),
    "the clip must disable the persisted viewport grid",
  );
  assert.ok(
    timeline.tracks.some(
      ({ kind, params }) => kind === "engine/set/skydome-environment"
        && params?.primaryName === ""
        && params?.secondaryName === "",
    ),
    "the clip must clear any persisted skydome environment",
  );
  assert.equal(item.framing, "full-app");
  assert.equal(item.encode?.zoom, undefined, "a curve-only zoom would crop the viewport payoff away");
  assert.ok(
    item.acceptance.some((line) => /particle system.*visible/i.test(line)),
    "acceptance must require the particle system to remain visible",
  );

});

test("the returning-user curve clip authors its checkbox return instead of reversing the video", () => {
  const manifest = JSON.parse(readFileSync(join(repoRoot, "tasks", "wiki-media", "manifest.json"), "utf8"));
  const item = manifest.items.find(({ id }) => id === "ref-curve-visibility");
  assert.ok(item, "ref-curve-visibility must remain in the media manifest");

  const timeline = JSON.parse(readFileSync(join(repoRoot, item.timeline), "utf8"));
  const cursor = timeline.tracks.find(({ cursor }) => Array.isArray(cursor))?.cursor ?? [];
  const clicks = cursor
    .filter(({ press, activate }) => press === true && activate === true)
    .map(({ t, target, vis }) => ({ t, ref: target?.ref, vis }));
  const keptStartMs = (item.encode?.start / timeline.fps) * 1000;
  const setupClicks = clicks.filter(({ t }) => t < keptStartMs);
  const keptClicks = clicks.filter(({ t }) => t >= keptStartMs);

  assert.equal(item.loop, "none", "the encode must preserve the authored direction of travel");
  assert.deepEqual(
    setupClicks.map(({ ref }) => ref),
    [
      "testid:curve-channel-checkbox-green",
      "testid:curve-channel-checkbox-blue",
    ],
    "the trimmed warm-up must hide Green and Blue so the clip opens Red-only",
  );
  assert.ok(
    setupClicks.every(({ vis }) => vis === false),
    "warm-up setup clicks must keep the cursor hidden",
  );
  assert.deepEqual(
    keptClicks.map(({ ref }) => ref),
    [
      "testid:curve-channel-checkbox-green",
      "testid:curve-channel-checkbox-blue",
      "testid:curve-channel-checkbox-alpha",
      "testid:curve-channel-checkbox-alpha",
      "testid:curve-channel-checkbox-blue",
      "testid:curve-channel-checkbox-green",
    ],
    "the visible clip must add Green, Blue, and Alpha, then remove them in reverse order",
  );
  assert.equal(
    timeline.tracks.some(({ kind }) => kind === "ui/reveal-curve-channel"),
    false,
    "the short visibility demonstration should not jump or scroll the channel list",
  );

  const visible = new Set(["red", "green", "blue"]);
  const toggle = (ref) => {
    const channel = ref?.match(/^testid:curve-channel-checkbox-(.+)$/)?.[1];
    assert.ok(channel, `unexpected activation target in curve checkbox cycle: ${ref}`);
    if (visible.has(channel)) visible.delete(channel);
    else visible.add(channel);
  };
  for (const { ref } of setupClicks) toggle(ref);
  assert.deepEqual([...visible], ["red"], "the encoded opening state must show only Red");
  for (const { ref } of keptClicks.slice(0, 3)) toggle(ref);
  assert.deepEqual(
    [...visible].sort(),
    ["alpha", "blue", "green", "red"],
    "the demonstration must build from Red to all four color curves",
  );
  for (const { ref } of keptClicks.slice(3)) toggle(ref);
  assert.deepEqual([...visible], ["red"], "the final visible set must return to the opening Red-only state");

  const first = cursor[0];
  const last = cursor.at(-1);
  assert.deepEqual(last?.target, first?.target, "the cursor must return to its opening park");
  assert.equal(last?.vis, false, "the cursor must be hidden before the loop seam");
  assert.ok(
    timeline.durationMs - last.t >= 2000,
    "the restored UI must hold for at least two seconds before the loop seam",
  );
  assert.ok(
    item.encode?.start >= timeline.fps * 8,
    "the encode must trim at least the particle system's eight-second warm-up",
  );
  const freeze = timeline.tracks.find(
    ({ kind, params }) => kind === "emitters/set-properties" && params?.patch?.freezeTime === 8,
  );
  const startSpawner = timeline.tracks.find(({ kind }) => kind === "spawner/start");
  assert.ok(freeze, "the UI-only loop must freeze the smoke at its settled eight-second state");
  assert.ok(
    freeze.at < startSpawner.at,
    "the freeze time must be configured before the preview instance starts",
  );
});
