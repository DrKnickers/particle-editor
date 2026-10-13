// Unit + integration tests for the unified gate's argument handling. Importing
// the module does NOT run a gate (the CLI is guarded behind an
// import.meta.url === argv[1] check), so parseArgs()/usage() can be exercised in
// isolation. The regression these lock down: `--help` (and any unknown flag)
// used to be silently ignored, so with no --lane the script fell through to the
// full unscoped gate — kicking off the multi-minute MSBuild/game lanes when the
// user only wanted usage.
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { mkdirSync, mkdtempSync, openSync, closeSync, readFileSync, rmSync } from "node:fs";

import { parseArgs, usage, shellSpawnSpec, spawnExitCode, parseSkippedCount, prereq, runReplayProofLane, runObserveLane, runToolSmokeLane, runVfxLoopLane } from "./run-all-tests.mjs";

const scriptPath = join(dirname(fileURLToPath(import.meta.url)), "run-all-tests.mjs");

test("parseArgs: no args → everything empty/false, nothing unknown", () => {
  assert.deepEqual(parseArgs([]), {
    lane: [], allowMissing: [], skipBuild: false, list: false, help: false, unknown: [],
  });
});

test("parseArgs: --help and -h both set help", () => {
  assert.equal(parseArgs(["--help"]).help, true);
  assert.equal(parseArgs(["-h"]).help, true);
});

test("parseArgs: --list and --skip-build set their flags", () => {
  assert.equal(parseArgs(["--list"]).list, true);
  assert.equal(parseArgs(["--skip-build"]).skipBuild, true);
});

test("parseArgs: --lane takes a comma list and is repeatable", () => {
  assert.deepEqual(parseArgs(["--lane", "lint,vitest"]).lane, ["lint", "vitest"]);
  assert.deepEqual(parseArgs(["--lane", "a", "--lane", "b"]).lane, ["a", "b"]);
});

test("parseArgs: --allow-missing collects its values", () => {
  assert.deepEqual(parseArgs(["--allow-missing", "site,drive-smoke"]).allowMissing,
    ["site", "drive-smoke"]);
});

test("parseArgs: an unrecognized flag lands in `unknown`, not silently dropped", () => {
  assert.deepEqual(parseArgs(["--nope"]).unknown, ["--nope"]);
  assert.deepEqual(parseArgs(["--help"]).unknown, []);
});

test("parseArgs: a --lane value is consumed, so a following flag is still parsed", () => {
  // Proves the value isn't mistaken for a flag AND a real flag after it is seen.
  const r = parseArgs(["--lane", "lint", "--bogus"]);
  assert.deepEqual(r.lane, ["lint"]);
  assert.deepEqual(r.unknown, ["--bogus"]);
});

test("parseArgs: a trailing --lane with no value is tolerated (no crash, no lane)", () => {
  const r = parseArgs(["--lane"]);
  assert.deepEqual(r.lane, []);
  assert.deepEqual(r.unknown, []);
});

test("usage: names every flag and the invocation", () => {
  const u = usage();
  for (const needle of ["Usage:", "--lane", "--allow-missing", "--skip-build", "--list", "--help"]) {
    assert.ok(u.includes(needle), `usage() should mention ${needle}`);
  }
});

// Integration: run the real script. These are instant — none of them reaches a
// lane (help/unknown short-circuit; --list just prints names) — so they prove the
// main() wiring + CLI guard, not just the pure parser.
function runScript(...args) {
  // File-backed output also works in Windows sandboxes that deny pipe handles.
  const scratch = join(dirname(scriptPath), "..", "tests", ".tmp");
  mkdirSync(scratch, { recursive: true });
  const directory = mkdtempSync(join(scratch, "gate-cli-"));
  const out = join(directory, "stdout"), err = join(directory, "stderr");
  const stdout = openSync(out, "w"), stderr = openSync(err, "w");
  let result;
  try { result = spawnSync(process.execPath, [scriptPath, ...args], { stdio: ["ignore", stdout, stderr], timeout: 10000, windowsHide: true }); }
  finally { closeSync(stdout); closeSync(stderr); }
  try { return { ...result, stdout: readFileSync(out, "utf8"), stderr: readFileSync(err, "utf8") }; }
  finally { rmSync(directory, { recursive: true, force: true }); }
}

test("--help exits 0 and prints usage without running a lane", () => {
  const r = runScript("--help");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /Usage:/);
  assert.doesNotMatch(r.stdout + r.stderr, /=== \w/); // no "=== <lane> ===" banner
});

test("an unknown flag exits 1 and prints usage (never runs the full gate)", () => {
  const r = runScript("--nope");
  assert.equal(r.status, 1);
  assert.match(r.stdout + r.stderr, /unknown argument/);
  assert.match(r.stdout, /Usage:/);
  assert.doesNotMatch(r.stdout + r.stderr, /=== \w/);
});

test("--list exits 0 and prints lane names", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /^lint$/m);
});

// ---- spawn portability: cmd.exe only where it exists ----

test("shellSpawnSpec: win32 goes through cmd.exe with verbatim args", () => {
  const s = shellSpawnSpec("pnpm run lint", "win32");
  assert.equal(s.cmd, "cmd.exe");
  assert.deepEqual(s.args, ["/d", "/s", "/c", '"pnpm run lint"']);
  assert.equal(s.opts.windowsVerbatimArguments, true);
  assert.equal(s.opts.shell, false);
});

test("shellSpawnSpec: linux/darwin use the POSIX shell, never cmd.exe", () => {
  for (const platform of ["linux", "darwin"]) {
    const s = shellSpawnSpec("pnpm run lint", platform);
    assert.equal(s.cmd, "pnpm run lint");
    assert.deepEqual(s.args, []);
    assert.equal(s.opts.shell, true);
  }
});

test("spawnExitCode: a child that never started reports why, then fails", () => {
  const said = [];
  const code = spawnExitCode({ error: new Error("spawn cmd.exe ENOENT"), status: null }, "pnpm run lint", (m) => said.push(m));
  assert.equal(code, 1);
  assert.equal(said.length, 1);
  assert.match(said[0], /could not start pnpm run lint: spawn cmd\.exe ENOENT/);
});

test("spawnExitCode: a normal exit passes its status through silently", () => {
  const said = [];
  assert.equal(spawnExitCode({ status: 0 }, "x", (m) => said.push(m)), 0);
  assert.equal(spawnExitCode({ status: 3 }, "x", (m) => said.push(m)), 3);
  assert.equal(spawnExitCode({ status: null }, "x", (m) => said.push(m)), 1);
  assert.deepEqual(said, []);
});

test("parseSkippedCount reads the render-goldens summary line", () => {
  assert.equal(parseSkippedCount("[gate] render-goldens: 2/3 scenes ok, 1 skipped"), 1);
  assert.equal(parseSkippedCount("[gate] render-goldens: 3/3 scenes ok, 0 skipped"), 0);
});

test("parseSkippedCount ignores a number glued to a word, such as a test title", () => {
  assert.equal(parseSkippedCount("✔ checker rejects E11 skipped (10.5ms)\nℹ skipped 0"), 0);
  assert.equal(parseSkippedCount("✔ checker rejects E9 skipped\n  11 skipped"), 11);
});

test("tool-smoke follows msbuild-release in the registered gate", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /msbuild-release\r?\ntool-smoke\r?\n/);
});

function toolSmokeLane(overrides = {}, allowMissing = false) {
  return runToolSmokeLane({
    exists: () => true, stale: () => null,
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? ["tool-smoke"] : [])),
    runSmoke: () => 0, ...overrides,
  });
}

for (const missing of ["Release", "tool-smoke.mjs", "mcp-adapter"]) {
  test(`tool-smoke missing ${missing} fails by default and --allow-missing visibly skips`, () => {
    const overrides = { exists: (path) => !path.includes(missing), runSmoke: () => assert.fail("must not run") };
    assert.equal(toolSmokeLane(overrides).status, "FAIL");
    const allowed = toolSmokeLane(overrides, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, /missing/);
  });
}

test("tool-smoke refuses a stale Release exe without running it", () => {
  const r = toolSmokeLane({ stale: () => "tool-smoke: exe older than its sources", runSmoke: () => assert.fail("must not run") });
  assert.equal(r.status, "FAIL");
  assert.match(r.note, /older/);
});

test("tool-smoke only passes after running the smoke; a failure cannot skip", () => {
  let runs = 0;
  assert.equal(toolSmokeLane({ runSmoke: () => { ++runs; return 0; } }).status, "PASS");
  assert.equal(runs, 1);
  assert.equal(toolSmokeLane({ runSmoke: () => 1 }, true).status, "FAIL");
});

test("replay-proof follows drive-smoke in the registered gate", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /drive-smoke\r?\nreplay-proof\r?\n/);
});

function replayLane(overrides = {}, allowMissing = false) {
  return runReplayProofLane({
    desktop: () => ({ ok: true }), exists: () => true, gameRoots: () => ["installed-game"],
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? ["replay-proof"] : [])),
    runProof: () => ({ code: 0 }), ...overrides,
  });
}

test("replay-proof has visible render-desktop SKIP and never runs without it", () => {
  const r = replayLane({ desktop: () => ({ ok: false, reason: "locked" }), runProof: () => assert.fail("must not run") });
  assert.equal(r.status, "SKIP");
  assert.match(r.note, /render desktop.*locked/);
});

for (const missing of ["Release", "Debug", "dist", "replay-proof.mjs"]) {
  test(`replay-proof missing ${missing} fails by default and --allow-missing visibly skips`, () => {
    const overrides = { exists: (path) => !path.includes(missing), runProof: () => assert.fail("must not run") };
    assert.equal(replayLane(overrides).status, "FAIL");
    const allowed = replayLane(overrides, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, /missing/);
  });
}

test("replay-proof reports missing host game data via prereq, never PASS", () => {
  const overrides = { gameRoots: () => [], runProof: () => assert.fail("must not run") };
  assert.match(replayLane(overrides).note, /missing game data/);
  assert.equal(replayLane(overrides).status, "FAIL");
  const allowed = replayLane(overrides, true);
  assert.equal(allowed.status, "SKIP");
  assert.match(allowed.note, /GameDataPath.*sibling.*cwd/);
});

test("replay-proof only passes after actually running the proof; failures cannot skip", () => {
  let runs = 0;
  assert.equal(replayLane({ runProof: (roots) => { ++runs; assert.deepEqual(roots, ["installed-game"]); return { code: 0 }; } }).status, "PASS");
  assert.equal(runs, 1);
  assert.equal(replayLane({ runProof: () => ({ code: 4 }) }, true).status, "FAIL");
});

test("observe immediately follows replay-proof in the registered gate", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /replay-proof\r?\nobserve\r?\n/);
});

function observeLane(overrides = {}, allowMissing = false) {
  return runObserveLane({
    desktop: () => ({ ok: true }), exists: () => true, gameRoots: () => ["installed-game"],
    probe: () => ({ status: 0 }),
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? ["observe"] : [])),
    runObserve: () => ({ code: 0 }), ...overrides,
  });
}

test("observe visibly skips without a render desktop and never runs", () => {
  const r = observeLane({ desktop: () => ({ ok: false, reason: "locked" }),
    probe: () => assert.fail("must not probe"), runObserve: () => assert.fail("must not run") });
  assert.equal(r.status, "SKIP");
  assert.match(r.note, /render desktop.*locked/);
});

for (const missing of ["Release", "Debug", "dist", "observe.mjs", "observe-lane.mjs"]) {
  test(`observe missing ${missing} fails by default and --allow-missing visibly skips`, () => {
    const overrides = { exists: (path) => !path.includes(missing), runObserve: () => assert.fail("must not run") };
    assert.equal(observeLane(overrides).status, "FAIL");
    const allowed = observeLane(overrides, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, /missing/);
  });
}

for (const missing of ["ffmpeg", "ffprobe"]) {
  test(`observe missing ${missing} cannot pass: visible SKIP or FAIL`, () => {
    const calls = [];
    const overrides = { probe: (bin) => { calls.push(bin); return { status: bin === missing ? null : 0, error: bin === missing ? new Error("ENOENT") : null }; },
      runObserve: () => assert.fail("must not run") };
    assert.equal(observeLane(overrides).status, "FAIL");
    const allowed = observeLane(overrides, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, new RegExp(missing));
    assert.ok(calls.includes(missing));
  });
}

test("observe missing host game data gives FAIL or visible allowed SKIP", () => {
  const overrides = { gameRoots: () => [], runObserve: () => assert.fail("must not run") };
  assert.equal(observeLane(overrides).status, "FAIL");
  const allowed = observeLane(overrides, true);
  assert.equal(allowed.status, "SKIP");
  assert.match(allowed.note, /GameDataPath.*sibling.*cwd/);
});

test("observe cannot pass without actually running, and a failed lane cannot skip", () => {
  let runs = 0;
  assert.equal(observeLane({ runObserve: (roots) => { ++runs; assert.deepEqual(roots, ["installed-game"]); return { code: 0 }; } }).status, "PASS");
  assert.equal(runs, 1);
  assert.equal(observeLane({ runObserve: () => ({ code: 1 }) }, true).status, "FAIL");
  assert.equal(observeLane({ runObserve: () => ({}) }).status, "FAIL");
});

test("vfx-loop immediately follows observe in the registered gate", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /observe\r?\nvfx-loop\r?\n/);
});

function vfxLoopLane(overrides = {}, allowMissing = false) {
  return runVfxLoopLane({
    desktop: () => ({ ok: true }), exists: () => true, gameRoots: () => ["installed-game"],
    probe: () => ({ status: 0 }),
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? ["vfx-loop"] : [])),
    runLoopLane: () => ({ code: 0 }), ...overrides,
  });
}

test("vfx-loop visibly skips without a render desktop and never runs", () => {
  const r = vfxLoopLane({ desktop: () => ({ ok: false, reason: "locked" }),
    probe: () => assert.fail("must not probe"), runLoopLane: () => assert.fail("must not run") });
  assert.equal(r.status, "SKIP");
  assert.match(r.note, /render desktop.*locked/);
});

for (const missing of ["Release", "lane.mjs"]) {
  test(`vfx-loop missing ${missing} fails by default and --allow-missing visibly skips`, () => {
    const overrides = { exists: (path) => !path.includes(missing), runLoopLane: () => assert.fail("must not run") };
    assert.equal(vfxLoopLane(overrides).status, "FAIL");
    const allowed = vfxLoopLane(overrides, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, /missing/);
  });
}

for (const [name, overrides] of [
  ["ffmpeg", { probe: (bin) => ({ status: bin === "ffmpeg" ? null : 0, error: bin === "ffmpeg" ? new Error("ENOENT") : null }) }],
  ["game data", { gameRoots: () => [] }],
]) test(`vfx-loop missing ${name} cannot pass: FAIL or visible allowed SKIP`, () => {
  const guarded = { ...overrides, runLoopLane: () => assert.fail("must not run") };
  assert.equal(vfxLoopLane(guarded).status, "FAIL");
  assert.equal(vfxLoopLane(guarded, true).status, "SKIP");
});

test("vfx-loop passes only by running, and a failed run cannot skip", () => {
  let runs = 0;
  assert.equal(vfxLoopLane({ runLoopLane: (roots) => { ++runs; assert.deepEqual(roots, ["installed-game"]); return { code: 0 }; } }).status, "PASS");
  assert.equal(runs, 1);
  assert.equal(vfxLoopLane({ runLoopLane: () => ({ code: 1 }) }, true).status, "FAIL");
});
