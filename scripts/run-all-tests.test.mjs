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

import { parseArgs, usage } from "./run-all-tests.mjs";

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
  return spawnSync(process.execPath, [scriptPath, ...args], { encoding: "utf8" });
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
