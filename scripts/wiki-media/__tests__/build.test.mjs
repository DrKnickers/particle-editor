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
        output: "wiki-source/media/manual.jpg",
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
  // Clears present but a LATER event re-sets/re-shows the reference object → still fails.
  const clearedThenReadded = { tracks: [
    { at: 0, kind: "engine/set/reference-object", params: { name: "" } },
    { at: 30, kind: "engine/set/reference-object-visible", params: { visible: false } },
    { at: 5000, kind: "engine/set/reference-object", params: { name: "AT_ST_Walker" } },
    { at: 5030, kind: "engine/set/reference-object-visible", params: { visible: true } },
  ] };
  assert.equal(validateTimelineNoReferenceObject(clearedThenReadded, "x").length, 2, "re-adding after clearing must fail");
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
