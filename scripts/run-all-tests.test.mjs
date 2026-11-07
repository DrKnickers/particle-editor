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
import { mkdirSync, mkdtempSync, openSync, closeSync, readFileSync, rmSync, writeFileSync, renameSync } from "node:fs";

import { parseArgs, usage, shellSpawnSpec, spawnExitCode, parseSkippedCount, prereq, runReplayProofLane, runObserveLane, runToolSmokeLane, runVfxLoopLane, runHoudiniLane, selectLanes } from "./run-all-tests.mjs";
import { TIERS, blockedDependency, runReportedLane, changedPaths, lanesForChanges, main } from "./run-all-tests.mjs";
import { scriptTestFiles } from "./run-script-tests.mjs";

const scriptPath = join(dirname(fileURLToPath(import.meta.url)), "run-all-tests.mjs");

test("--changed parses an optional equals ref and rejects an empty ref", () => {
  assert.equal(parseArgs(["--changed"]).changed, "origin/master");
  assert.equal(parseArgs(["--changed=topic"]).changed, "topic");
  assert.deepEqual(parseArgs(["--changed="]).unknown, ["--changed="]);
  assert.deepEqual(parseArgs(["--changed", "topic"]).unknown, ["topic"]);
  assert.ok(usage().includes("--changed[=<ref>]"));
  assert.match(usage(), /origin\/master/);
});

const ordered = (...names) => selectLanes([...new Set(names.flat())]).map((l) => l.name);
const scriptLanes = ["web-build", "scripts"];
const fullGate = selectLanes([]).map((l) => l.name);
for (const [paths, expected] of [
  [[], []],
  [["README.md", "CHANGELOG.md", "AGENTS.md", "docs/nested/a.md", "tasks/plan.md", ".claude/agents/a.md"], []],
  [["web/apps/editor/src/App.tsx"], ordered("lint", "vitest", scriptLanes, "playwright-web", TIERS.native)],
  [["web/apps/editor/src/config.ts"], ordered("lint", "vitest", scriptLanes, "playwright-web", TIERS.native)],
  [["web/packages/bridge-schema/src/index.ts"], ordered("lint", "vitest", scriptLanes, "playwright-web", TIERS.native)],
  [["web/apps/editor/src/lib/__tests__/fixture.ts", "web/apps/editor/src/App.test.tsx"], ordered("lint", "vitest", scriptLanes)],
  [["web/apps/editor/tests/render-loop.spec.ts"], ordered("lint", TIERS.native)],
  [["web/apps/editor/tests/a11y-goldens/dialog.golden.yaml"], ordered("lint", TIERS.native)],
  [["web/apps/editor/tests-web/app.spec.ts"], ["lint", "playwright-web"]],
  [["web/apps/editor/tests-site/guide.spec.ts", "site/index.html"], ordered("lint", "site", scriptLanes)],
  [["tests/goldens/render/scene.png"], TIERS.render],
  [["src/rendering/Engine.cpp"], ordered(TIERS.edit, TIERS.native, TIERS.render, TIERS.media)],
  [["src/host/HostWindow.cpp"], ordered(TIERS.edit, TIERS.native, TIERS.render, TIERS.media)],
  [["tests/test_policy.cpp", "tests/native-tests.json"], ["cpp-unit"]],
  [["scripts/run-all-tests.mjs"], scriptLanes],
  [["scripts/other.test.mjs"], scriptLanes],
  [["scripts/render-goldens.mjs"], ordered(scriptLanes, TIERS.render)],
  [["scripts/run-native-unit-tests.mjs"], ordered(scriptLanes, "cpp-unit", "msbuild-debug", "cpp-unit-exe")],
  [["scripts/tool-smoke.mjs", "scripts/mcp-adapter/smoke.mjs"], ordered(scriptLanes, "msbuild-release", "tool-smoke")],
  [["scripts/replay-proof.mjs"], ordered(scriptLanes, "msbuild-debug", "msbuild-release", "replay-proof")],
  [["scripts/observe-lane.mjs"], ordered(scriptLanes, "msbuild-debug", "msbuild-release", "observe")],
  [["scripts/vfx-loop/lane.mjs"], ordered(scriptLanes, "msbuild-release", "vfx-loop")],
  [["scripts/build-guide.mjs"], ordered(scriptLanes, "site")],
  [["scripts/embed-web-dist.mjs"], ordered(scriptLanes, TIERS.native)],
  [["./web\\apps\\editor\\tests-web\\spaces é.spec.ts"], ["lint", "playwright-web"]],
]) test(`lanesForChanges: ${paths.join(", ") || "no changes"}`, () => {
  assert.deepEqual(lanesForChanges(paths).lanes, expected);
});

for (const p of [
  "web/pnpm-lock.yaml", "web/package.json", "package-lock.json", "yarn.lock", "web/pnpm-workspace.yaml",
  "ParticleEditor.sln", "src/ParticleEditor.vcxproj", "src/ParticleEditor.vcxproj.filters", "Directory.Build.props",
  "web/apps/editor/vite.config.ts", "web/apps/editor/tsconfig.e2e.json", "web/.nvmrc",
]) test(`build/config change selects the full default gate: ${p}`, () => {
  assert.deepEqual(lanesForChanges([p]).lanes, fullGate);
});

test("publication paths take priority over docs and scripts, without requiring a private tree", () => {
  for (const parts of [
    ["tools", "public_history", "tool.py"], ["tests", "public_history", "test.py"],
    ["tasks", "public-history-rebuild", "notes.md"], ["tasks", "public-manifest.txt"],
    ["scripts", "public-history.py"], ["scripts", "public-history.test.mjs"], ["scripts", "sync-public.ps1"],
  ]) assert.deepEqual(lanesForChanges([parts.join("/")]).lanes, ["publication"]);
});

test("unmatched paths name the fallback and select the full default gate", () => {
  for (const p of ["unknown/file.bin", "web/apps/editor/src-other/file.ts", "tests/test_other.txt", "docs-extra/file"]) {
    const selected = lanesForChanges([p]);
    assert.deepEqual(selected.lanes, fullGate);
    assert.ok(selected.reasons.lint.some((why) => why.includes(p) && why.includes("unmatched")));
    assert.ok(!selected.lanes.includes("houdini"));
  }
});

test("changed selection unions explicit lanes/tiers and recursively adds dependencies in gate order", () => {
  const selected = lanesForChanges(["README.md"], [...TIERS.site, "observe", "houdini"]);
  assert.deepEqual(selected.lanes, ordered("web-build", TIERS.site, "msbuild-debug", "msbuild-release", "observe", "houdini"));
  assert.ok(selected.reasons["msbuild-release"].includes("required by observe"));
  assert.ok(selected.reasons.observe.includes("explicit --lane/--tier"));
  assert.deepEqual(lanesForChanges([], ["houdini"]).lanes, ["web-build", "msbuild-release", "houdini"]);
  assert.deepEqual(lanesForChanges(["web/apps/editor/src/App.test.tsx", "web/apps/editor/src/App.tsx"]).lanes,
    ordered("lint", "vitest", scriptLanes, "playwright-web", TIERS.native));
  const all = lanesForChanges(["unknown"]);
  for (const lane of selectLanes(all.lanes)) {
    for (const dep of lane.deps || []) assert.ok(all.lanes.includes(dep), `${lane.name} needs ${dep}`);
  }
  assert.deepEqual(lanesForChanges(["scripts/run-all-tests.mjs", "scripts/run-all-tests.mjs"]).reasons.scripts,
    ["scripts/run-all-tests.mjs"]);
});

test("changed CLI prints reasons, unions tiers/lanes, exits on docs, and rejects stale builds or Git failure", () => {
  const output = [];
  const original = console.log;
  console.log = (s) => output.push(s);
  try {
    assert.equal(main(parseArgs(["--changed"]), () => ["README.md"]), 0);
    assert.match(output.join("\n"), /nothing needs testing/);
    output.length = 0;
    assert.equal(main(parseArgs(["--changed", "--tier", "site", "--lane", "tool-smoke", "--list"]), () => []), 0);
    assert.deepEqual(output.filter((line) => !line.startsWith("[gate]")),
      ["web-build", "playwright-web", "site", "msbuild-release", "tool-smoke"]);
    output.length = 0;
    assert.equal(main(parseArgs(["--changed=topic", "--tier", "site", "--lane", "tool-smoke", "--list"]),
      (base) => { assert.equal(base, "topic"); return ["unknown/file"]; }), 0);
    assert.match(output.join("\n"), /unmatched path.*unknown\/file/);
    assert.match(output.join("\n"), /required by tool-smoke/);
    output.length = 0;
    assert.equal(main(parseArgs(["--changed", "--skip-build"]), () => assert.fail("must not query git")), 1);
    assert.match(output.join("\n"), /fresh builds/);
    output.length = 0;
    assert.equal(main(parseArgs(["--changed"]), () => { throw new Error("missing base ref"); }), 1);
    assert.match(output.join("\n"), /missing base ref/);
  } finally { console.log = original; }
});

test("changedPaths fails closed on any Git failure or missing merge-base", () => {
  for (let failAt = 0; failAt < 5; failAt++) {
    let calls = 0;
    assert.throws(() => changedPaths("missing-ref", ".", () => calls++ === failAt
      ? { status: 128, stderr: "bad revision" } : { status: 0, stdout: "base\0" }), /--changed: git .*failed.*missing-ref.*bad revision/);
  }
  assert.throws(() => changedPaths("base", ".", () => ({ error: new Error("ENOENT") })), /ENOENT/);
  assert.throws(() => changedPaths("base", ".", () => ({ status: 0, stdout: "" })), /no merge-base/);
});

test("changedPaths preserves NUL-delimited names and queries each change source without a shell", () => {
  const calls = [];
  const output = ["abc123\n", "committed\0space é\nname\0", "staged\0committed\0", "unstaged\0", "untracked\0"];
  assert.deepEqual(changedPaths("topic", "scratch", (cmd, args, opts) => {
    assert.equal(cmd, "git");
    assert.equal(opts.cwd, "scratch");
    assert.equal(opts.shell, false);
    calls.push(args);
    return { status: 0, stdout: output[calls.length - 1] };
  }), ["committed", "space é\nname", "staged", "unstaged", "untracked"]);
  assert.deepEqual(calls, [
    ["merge-base", "--", "HEAD", "topic"],
    ["diff", "--name-only", "--no-renames", "-z", "abc123", "HEAD", "--"],
    ["diff", "--name-only", "--no-renames", "-z", "--cached", "HEAD", "--"],
    ["diff", "--name-only", "--no-renames", "-z", "--"],
    ["ls-files", "--others", "--exclude-standard", "-z"],
  ]);
});

test("changedPaths collects branch, staged, unstaged, untracked and rename paths from the merge-base", () => {
  const scratch = join(dirname(scriptPath), "..", "tests", ".tmp");
  mkdirSync(scratch, { recursive: true });
  const cwd = mkdtempSync(join(scratch, "gate-changes-"));
  const run = (cmd, args, opts) => {
    const out = join(cwd, "stdout"), err = join(cwd, "stderr");
    const stdout = openSync(out, "w"), stderr = openSync(err, "w");
    let r;
    try { r = spawnSync(cmd, args, { ...opts, stdio: ["ignore", stdout, stderr], windowsHide: true }); }
    finally { closeSync(stdout); closeSync(stderr); }
    try { return { ...r, stdout: readFileSync(out, "utf8"), stderr: readFileSync(err, "utf8") }; }
    finally { rmSync(out); rmSync(err); }
  };
  const git = (...args) => {
    const r = run("git", args, { cwd, encoding: "utf8" });
    assert.equal(r.status, 0, r.stderr || r.error?.message);
    return r.stdout.trim();
  };
  const write = (p, text = p) => writeFileSync(join(cwd, p), text, "utf8");
  const untracked = process.platform === "win32" ? "untracked é name" : "untracked é\nname";
  try {
    git("init");
    git("config", "user.name", "Gate Test");
    git("config", "user.email", "gate@example.invalid");
    for (const p of ["staged", "unstaged", "renamed", "deleted"]) write(p);
    write(".gitignore", "ignored\nstdout\nstderr\n");
    git("add", "."); git("commit", "-m", "base");
    const base = git("rev-parse", "HEAD");
    git("branch", "comparison");
    write("branch"); git("add", "."); git("commit", "-m", "branch change");
    const head = git("rev-parse", "HEAD");
    git("checkout", "comparison");
    write("base-only"); git("add", "."); git("commit", "-m", "comparison change");
    git("checkout", "--detach", head);
    write("staged", "new"); git("add", "staged");
    write("unstaged", "new");
    renameSync(join(cwd, "renamed"), join(cwd, "renamed new")); git("add", "-A");
    // Leave a separate unstaged edit after staging the rename.
    write("unstaged", "newer");
    rmSync(join(cwd, "deleted"));
    write(untracked); write("ignored");
    const expected = ["branch", "staged", "unstaged", "renamed", "renamed new", "deleted", untracked].sort();
    assert.deepEqual(changedPaths("comparison", cwd, run).sort(), expected);
    assert.deepEqual(changedPaths(base, cwd, run).sort(), expected);
    assert.throws(() => changedPaths("missing-ref", cwd, run), /--changed: git merge-base failed/);
  } finally { rmSync(cwd, { recursive: true, force: true }); }
});

test("parseArgs: no args → everything empty/false, nothing unknown", () => {
  assert.deepEqual(parseArgs([]), {
    lane: [], tier: [], allowMissing: [], skipBuild: false, list: false, help: false, changed: null, unknown: [],
  });
});

test("parseArgs: --help and -h both set help", () => {
  assert.equal(parseArgs(["--help"]).help, true);
  assert.equal(parseArgs(["-h"]).help, true);
});

test("blockedDependency: --skip-build leaves skipped build lanes non-blocking", () => {
  const lane = { deps: ["msbuild-release"] };
  const skipped = { name: "msbuild-release", status: "SKIP", note: "--skip-build" };
  assert.equal(blockedDependency(lane, [skipped], true), undefined);
  assert.equal(blockedDependency(lane, [skipped], false), skipped);
  const failed = { name: "msbuild-release", status: "FAIL" };
  assert.equal(blockedDependency(lane, [failed], true), failed);
});

test("lanesForChanges: lane data and drivers outside scripts are not treated as prose", () => {
  const scriptsLanes = lanesForChanges(["scripts/x.test.mjs"]).lanes;
  for (const parts of [["tasks", "wiki-media", "a.json"], ["tasks", "clips", "a.md"], [".claude", "skills", "a", "t.json"]]) {
    assert.deepEqual(lanesForChanges([parts.join("/")]).lanes, scriptsLanes);
  }
  const driver = lanesForChanges([["tasks", "drive-smoke.ps1"].join("/")]).lanes;
  assert.ok(driver.includes("drive-smoke"));
  assert.ok(driver.includes("cpp-unit"));
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
  for (const needle of ["Usage:", "--lane", "--tier", "--allow-missing", "--skip-build", "--list", "--help"]) {
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

test("parseArgs: tiers take repeatable comma lists and combine with lanes", () => {
  const r = parseArgs(["--tier", "edit,publication", "--tier", "site", "--lane", "render-goldens"]);
  assert.deepEqual(r.tier, ["edit", "publication", "site"]);
  assert.deepEqual(r.lane, ["render-goldens"]);
  assert.deepEqual(parseArgs(["--tier"]).unknown, ["--tier"]);
});

test("tiers contain the requested lanes and their build prerequisites", () => {
  assert.deepEqual(TIERS, {
    edit: ["lint", "vitest", "web-build", "scripts", "cpp-unit"],
    publication: ["publication"],
    render: ["web-build", "msbuild-release", "render-goldens"],
    site: ["playwright-web", "site"],
    media: ["web-build", "msbuild-debug", "msbuild-release", "record-smoke", "drive-smoke", "replay-proof", "observe", "vfx-loop"],
    native: ["web-build", "msbuild-debug", "cpp-unit-exe", "playwright-native", "msbuild-release", "tool-smoke"],
  });
  for (const [tier, names] of Object.entries(TIERS)) {
    const lanes = selectLanes(names);
    assert.deepEqual(lanes.map((l) => l.name), names, tier);
    for (const lane of lanes) {
      for (const dep of lane.deps || []) assert.ok(names.includes(dep), `${tier}: ${lane.name} needs ${dep}`);
    }
    const r = runScript("--tier", tier, "--list");
    assert.equal(r.status, 0);
    assert.deepEqual(r.stdout.trim().split(/\r?\n/), names);
  }
});

test("--list describes tiers, while combined selection deduplicates in gate order", () => {
  const listing = runScript("--list");
  for (const tier of Object.keys(TIERS)) assert.ok(listing.stdout.includes(`  ${tier}: `));
  const r = runScript("--tier", "publication,site", "--lane", "site", "--list");
  assert.equal(r.status, 0);
  assert.deepEqual(r.stdout.trim().split(/\r?\n/), ["publication", "playwright-web", "site"]);
});

test("unknown tiers and lanes fail before running, including with --list", () => {
  for (const flag of ["--tier", "--lane"]) {
    for (const tail of [[], ["--list"]]) {
      const r = runScript(flag, "no-such-group", ...tail);
      assert.equal(r.status, 1);
      assert.match(r.stdout, /unknown (tier|lane)\(s\): no-such-group/);
      assert.doesNotMatch(r.stdout + r.stderr, /=== \w/);
    }
  }
  assert.equal(runScript("--tier", "toString", "--list").status, 1);
});

test("the publication lane stays in the full gate and runs separately from scripts", () => {
  assert.ok(selectLanes([]).some((l) => l.name === "publication"));
  const files = scriptTestFiles();
  assert.ok(files.length > 50);
  assert.ok(!files.some((p) => p.endsWith("public-history.test.mjs")));
  assert.ok(files.some((p) => p.endsWith("run-all-tests.test.mjs")));
  const pkg = JSON.parse(readFileSync(join(dirname(scriptPath), "..", "web", "apps", "editor", "package.json"), "utf8"));
  assert.equal(pkg.scripts["test:scripts"], "node ../../../scripts/run-script-tests.mjs");
  assert.equal(pkg.scripts["test:public-history"], undefined);
});

test("FAIL and SKIP dependencies block consumers; omitted dependencies do not", () => {
  const release = { name: "msbuild-release", deps: ["web-build"] };
  const render = selectLanes(["render-goldens"])[0];
  for (const status of ["FAIL", "SKIP"]) {
    const result = { name: "msbuild-release", status };
    assert.equal(blockedDependency(render, [result]), result);
  }
  const webSkip = { name: "web-build", status: "SKIP" };
  assert.equal(blockedDependency(release, [webSkip]), webSkip);
  assert.equal(blockedDependency(render, [webSkip, { name: "msbuild-release", status: "SKIP", note: "blocked: web-build skipped" }]).name, "msbuild-release");
  assert.equal(blockedDependency(render, []), undefined);
  assert.equal(blockedDependency(render, [{ name: "msbuild-release", status: "PASS" }]), undefined);
  assert.equal(blockedDependency({ name: "lint" }, [webSkip]), undefined);
});

test("Vitest and Playwright summaries expose skips without counting passed tests", () => {
  assert.equal(parseSkippedCount(" Test Files  1 skipped (1)\n      Tests  12 skipped (12)"), 12);
  assert.equal(parseSkippedCount("      Tests  42 passed | 2 skipped (44)"), 2);
  assert.equal(parseSkippedCount("      Tests  \x1b[32m42 passed\x1b[39m | \x1b[33m2 skipped\x1b[39m (44)"), 2);
  assert.equal(parseSkippedCount("  2 skipped\n  42 passed (2.3s)"), 2);
});

test("publication: Python skips survive node:test's zero-skip wrapper summary", () => {
  assert.equal(parseSkippedCount("# [publication] 3 skipped Python tests\n# skipped 0"), 3);
  const run = (out) => runReportedLane("publication", "runner", "runner failed", () => ({ code: 0, out }));
  assert.equal(run("# [publication] 0 skipped Python tests\n# skipped 0").status, "PASS");
  assert.equal(run("# [publication] 3 skipped Python tests\n# skipped 0").status, "FAIL");
  assert.equal(run("# [publication] 4 skipped Python tests\n# skipped 0").status, "FAIL");
});

for (const lane of ["vitest", "playwright-web", "site"]) {
  test(`${lane}: zero skips passes, unbudgeted skips and runner failures fail`, () => {
    const run = (code, out) => runReportedLane(lane, "runner", "runner failed", () => ({ code, out }));
    assert.equal(run(0, "42 passed").status, "PASS");
    const skipped = run(0, "42 passed | 2 skipped");
    assert.equal(skipped.status, "FAIL");
    assert.match(skipped.note, /2 skipped test\(s\), budget 0/);
    assert.equal(run(1, "42 passed").status, "FAIL");
  });
}

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

test("opt-in lanes are marked in --list and selected only with --lane", () => {
  const r = runScript("--list");
  assert.equal(r.status, 0);
  assert.match(r.stdout, /vfx-loop\r?\nhoudini \(opt-in\)\r?\n/);
  assert.match(r.stdout, /engine-preview-e2e \(opt-in\)/);
  const expectedOptInCount = 2;
  assert.equal(r.stdout.match(/opt-in/g).length, expectedOptInCount);
  const names = (only) => selectLanes(only).map((l) => l.name);
  assert.ok(!names([]).includes("houdini"), "the default gate never runs houdini");
  assert.deepEqual(names([]), names([]).filter((n) => n !== "houdini"));
  assert.ok(names([]).includes("vfx-loop"));
  assert.deepEqual(names(["houdini"]), ["houdini"]);
  assert.deepEqual(names(["scripts", "houdini"]), ["scripts", "houdini"]);
  assert.ok(!names([]).includes("engine-preview-e2e"));
  assert.deepEqual(names(["engine-preview-e2e"]), ["engine-preview-e2e"]);
  assert.deepEqual(selectLanes(["engine-preview-e2e"])[0].deps, ["msbuild-release"]);
  assert.deepEqual(lanesForChanges([], ["engine-preview-e2e"]).lanes,
    ["web-build", "msbuild-release", "engine-preview-e2e"]);
});

function houdiniLane(overrides = {}, allowMissing = false) {
  return runHoudiniLane({
    platform: "win32", exists: () => true,
    checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? ["houdini"] : [])),
    runLane: () => ({ code: 0 }), ...overrides,
  });
}

for (const [missing, overrides, note] of [
  ["Houdini", { exists: (path) => !path.includes("Houdini") }, /Houdini 19.5.435 missing/],
  ["lane.mjs", { exists: (path) => !path.endsWith("lane.mjs") }, /lane.mjs missing/],
  ["the Release build", { exists: (path) => !path.endsWith("ParticleEditor.exe") }, /x64\/Release\/ParticleEditor.exe missing/],
  ["Windows", { platform: "linux" }, /Windows-only/],
]) {
  test(`houdini without ${missing} fails by default and --allow-missing visibly skips`, () => {
    const never = { runLane: () => assert.fail("must not run") };
    assert.equal(houdiniLane({ ...overrides, ...never }).status, "FAIL");
    const allowed = houdiniLane({ ...overrides, ...never }, true);
    assert.equal(allowed.status, "SKIP");
    assert.match(allowed.note, note);
  });
}

test("houdini passes only when its lane script exits 0", () => {
  let runs = 0;
  assert.equal(houdiniLane({ runLane: () => { ++runs; return { code: 0 }; } }).status, "PASS");
  assert.equal(runs, 1);
  assert.equal(houdiniLane({ runLane: () => ({ code: 1 }) }).status, "FAIL");
});
