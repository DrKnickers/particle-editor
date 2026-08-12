// [gate] Aggregate runner for the standalone native unit tests (tests/test_*.cpp).
//
//   node scripts/run-native-unit-tests.mjs [flags]
//
// Enumerates tests/test_*.cpp (cpp -> bat direction, so dump_*/spike_*/make_* diag
// tools never enter the lane), builds each via its tests/build_<name>.bat, runs the
// exe, and prints a PASS/FAIL/SKIP summary. Exits nonzero on any FAIL. A needs-exe
// test whose ParticleEditor.exe is absent is a FAIL by default (a gate must not go
// green around missing coverage); pass --allow-missing-exe to downgrade it to a
// VISIBLE SKIP when iterating without an app build. A binary that exits 0 after
// printing `SKIP:` for a case whose capability probe failed is likewise a FAIL by
// default (--allow-missing-capabilities to accept). Hard-errors up front if any
// test_*.cpp has no matching builder (orphan guard — a test that exists but can't
// be built is a silent coverage hole).
//
// Builds are NOT incremental: the bats compile production src/*.cpp and header-only
// deps, so any cheap freshness check risks running stale exes green. --skip-build
// exists as an EXPLICITLY UNSAFE flag for iterating on test logic only.
//
// Flags:
//   --filter <substr>      only tests whose name contains <substr>
//   --skip-build           run existing exes without rebuilding (unsafe; missing exe = FAIL)
//   --only-needs-exe       only the tests that validate a built ParticleEditor.exe
//   --exclude-needs-exe    everything else (the gate splits lanes this way)
//   --exe <path>           ParticleEditor.exe path passed to needs-exe tests
//                          (default x64\Debug\ParticleEditor.exe — bare
//                          test_resource_strings would prefer a stale Release exe)
//   --allow-missing-exe    downgrade a missing app exe from FAIL to a visible SKIP
//   --allow-missing-capabilities
//                          accept a binary that self-SKIPPED cases (see below)
//   --timeout <secs>       per-test run timeout (default 120; hung test = FAIL)
//   --list                 print the resolved test list and exit

import { spawnSync } from "node:child_process";
import { readdirSync, existsSync, statSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { parseArgs } from "node:util";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const testsDir = join(repoRoot, "tests");

// Tests that validate a built ParticleEditor.exe rather than standalone logic.
const NEEDS_EXE = new Set(["test_resource_strings"]);

// strict:false mirrors the old hand scanner: unknown flags and positionals are
// ignored, and — load-bearing — gate-integrity.test.mjs IMPORTS this module, so
// this runs at module scope under `node --test`'s argv (test-file positionals);
// strict mode would throw on import.
const { values: args } = parseArgs({
  args: process.argv.slice(2),
  strict: false,
  options: {
    "filter":                     { type: "string" },
    "skip-build":                 { type: "boolean", default: false },
    "only-needs-exe":             { type: "boolean", default: false },
    "exclude-needs-exe":          { type: "boolean", default: false },
    "list":                       { type: "boolean", default: false },
    "allow-missing-exe":          { type: "boolean", default: false },
    "allow-missing-capabilities": { type: "boolean", default: false },
    "timeout":                    { type: "string", default: "120" },
    "exe":                        { type: "string", default: join("x64", "Debug", "ParticleEditor.exe") },
  },
});
const FILTER = args.filter ?? null;
const SKIP_BUILD = args["skip-build"];
const ONLY_NEEDS_EXE = args["only-needs-exe"];
const EXCLUDE_NEEDS_EXE = args["exclude-needs-exe"];
const LIST = args.list;
const ALLOW_MISSING_EXE = args["allow-missing-exe"];
const ALLOW_MISSING_CAPABILITIES = args["allow-missing-capabilities"];
const TIMEOUT_MS = Number(args.timeout) * 1000;
const APP_EXE = resolve(repoRoot, args.exe);

// Verdict for a test binary that exited 0 but printed `SKIP: <case> (<reason>)`
// for one or more of its cases.
//
// Exported so scripts/gate-integrity.test.mjs can exercise it directly: this is
// the decision the 2026-07 audit filed as an-audit-finding. #687 made these skips VISIBLE,
// and stopped there on the reasoning that "the capability genuinely is absent on
// some machines, and failing there would punish a legitimate environment". That
// is true of the machine that never had the capability and false of the machine
// that HAD it and quietly lost it — and the second machine is the dangerous one,
// because `test_clip_save_confinement`'s junction case going quiet is
// indistinguishable, in the exit code, from junction rejection still working.
//
// So: FAIL by default, and let an environment that genuinely cannot run the case
// say so out loud with --allow-missing-capabilities. Same shape as
// --allow-missing-exe above and as the gate's own --allow-missing <lane> (#706):
// a missing capability is a decision someone makes explicitly, not a silence.
export function selfSkipVerdict(skippedCount, allowMissingCapabilities) {
  if (skippedCount === 0) return { ok: true, note: "" };
  const what = `${skippedCount} case(s) SKIPPED`;
  if (allowMissingCapabilities) return { ok: true, note: `${what} (allowed)` };
  return { ok: false, note: `${what} — coverage did NOT run (or pass --allow-missing-capabilities)` };
}

if (ONLY_NEEDS_EXE && EXCLUDE_NEEDS_EXE) {
  console.error("[gate] --only-needs-exe and --exclude-needs-exe are mutually exclusive.");
  process.exit(1);
}

// cmd.exe /d /s /c with verbatim args: the only reliable way to launch a .bat
// whose absolute path contains spaces ("Particle Editor") from Node. cwd is
// ALWAYS repoRoot — several builders (e.g. build_test_clip_runner.bat) assume it.
function runBat(batPath) {
  return spawnSync("cmd.exe", ["/d", "/s", "/c", `""${batPath}""`], {
    cwd: repoRoot,
    stdio: "inherit",
    shell: false,
    windowsVerbatimArguments: true,
    timeout: 300000, // a single unit build should never take 5 min
  });
}

function discover() {
  const names = readdirSync(testsDir)
    .filter((f) => /^test_.*\.cpp$/i.test(f))
    .map((f) => f.replace(/\.cpp$/i, ""))
    .sort();
  // Orphan guard: every assertion test must have a standard builder.
  const orphans = names.filter((n) => !existsSync(join(testsDir, `build_${n}.bat`)));
  if (orphans.length > 0) {
    console.error(
      `[gate] ORPHAN native test(s) with no tests/build_<name>.bat — unbuildable ` +
        `assertions are a silent coverage hole:\n  ${orphans.join("\n  ")}`,
    );
    process.exit(1);
  }
  let list = names;
  if (ONLY_NEEDS_EXE) list = list.filter((n) => NEEDS_EXE.has(n));
  if (EXCLUDE_NEEDS_EXE) list = list.filter((n) => !NEEDS_EXE.has(n));
  if (FILTER) list = list.filter((n) => n.includes(FILTER));
  // Belt-and-braces: the palette test uses a temp-dir override seam, but still
  // run it last so a regression there can't perturb earlier results.
  list = [...list.filter((n) => n !== "test_palette_store"),
          ...list.filter((n) => n === "test_palette_store")];
  return list;
}

function main() {
  const tests = discover();
  if (LIST) {
    for (const n of tests) console.log(n + (NEEDS_EXE.has(n) ? "  (needs-exe)" : ""));
    return 0;
  }
  if (tests.length === 0) {
    console.error("[gate] no native unit tests matched.");
    return 1;
  }

  const results = [];
  // Cases a binary self-skipped because a capability probe failed. Reported at
  // the end so a green run states plainly what it did NOT exercise (2026-07 audit).
  const skippedCases = [];
  for (const name of tests) {
    const started = Date.now();
    const exe = join(testsDir, `${name}.exe`);
    const record = (status, note = "") =>
      results.push({ name, status, secs: (Date.now() - started) / 1000, note });

    if (NEEDS_EXE.has(name) && !existsSync(APP_EXE)) {
      if (ALLOW_MISSING_EXE && !ONLY_NEEDS_EXE) {
        console.log(`[gate] ${name}: SKIP (app exe missing: ${APP_EXE})`);
        record("SKIP", "app exe missing");
      } else {
        console.error(
          `[gate] ${name}: required app exe missing (${APP_EXE}) — build it first` +
            (ONLY_NEEDS_EXE ? "." : " (or pass --allow-missing-exe)."),
        );
        record("FAIL", "app exe missing");
      }
      continue;
    }

    if (!SKIP_BUILD) {
      let beforeMs = -1;
      try { beforeMs = statSync(exe).mtimeMs; } catch { /* no prior exe */ }
      console.log(`[gate] build ${name}`);
      const b = runBat(join(testsDir, `build_${name}.bat`));
      if (b.status !== 0 || b.error) {
        record("FAIL", `build exit ${b.error ? "spawn-error" : b.status}`);
        continue;
      }
      // A bat can exit 0 without compiling (broken script, skipped cl). Exit 0
      // alone doesn't prove a fresh binary — require the exe mtime to ADVANCE,
      // else a stale exe reads green (same trap as the web-build dist proof).
      let afterMs = -1;
      try { afterMs = statSync(exe).mtimeMs; } catch { /* still missing */ }
      if (afterMs <= beforeMs || afterMs < 0) {
        record("FAIL", "build exited 0 but exe was not (re)produced");
        continue;
      }
    }
    if (!existsSync(exe)) {
      // --skip-build on a tree that never built this test.
      record("FAIL", "exe missing (run without --skip-build)");
      continue;
    }

    console.log(`[gate] run   ${name}`);
    // Capture-and-echo rather than "inherit": a test binary can print
    // `SKIP: <case> (<reason>)` for a case whose CAPABILITY probe failed —
    // test_clip_save_confinement does exactly that when `mklink /J` or 8.3
    // short-name lookup is unavailable — and then still exit 0. Reading only the
    // exit code made a self-skipped junction/short-path confinement case
    // indistinguishable from a passing one (2026-07 audit). A self-skipped
    // case now FAILS the lane unless explicitly allowed — see selfSkipVerdict.
    const r = spawnSync(exe, NEEDS_EXE.has(name) ? [APP_EXE] : [], {
      cwd: repoRoot,
      stdio: ["ignore", "pipe", "pipe"],
      encoding: "utf8",
      shell: false,
      timeout: TIMEOUT_MS,
    });
    if (r.stdout) process.stdout.write(r.stdout);
    if (r.stderr) process.stderr.write(r.stderr);
    const skipped = [...String(r.stdout || "").matchAll(/^SKIP:\s*(.+)$/gm)].map((m) => m[1].trim());
    if (skipped.length) skippedCases.push({ test: name, cases: skipped });

    const verdict = selfSkipVerdict(skipped.length, ALLOW_MISSING_CAPABILITIES);
    if (r.error || r.signal) record("FAIL", r.signal ? `timeout/killed (${r.signal})` : String(r.error));
    else if (r.status !== 0) record("FAIL", `exit ${r.status}`);
    else record(verdict.ok ? "PASS" : "FAIL", verdict.note);
  }

  const width = Math.max(...results.map((r) => r.name.length));
  console.log(`\n[gate] native unit summary (${results.length} tests)`);
  for (const r of results) {
    const note = r.note ? `  — ${r.note}` : "";
    console.log(`  ${r.name.padEnd(width)}  ${r.status.padEnd(4)}  ${r.secs.toFixed(1)}s${note}`);
  }
  const fails = results.filter((r) => r.status === "FAIL");
  const skips = results.filter((r) => r.status === "SKIP");
  console.log(
    `[gate] ${results.length - fails.length - skips.length} passed, ${fails.length} failed, ${skips.length} skipped`,
  );
  // Always name the un-run coverage, whether it failed the lane or was allowed.
  if (skippedCases.length) {
    const total = skippedCases.reduce((n, s) => n + s.cases.length, 0);
    const verb = ALLOW_MISSING_CAPABILITIES ? "ALLOWED but NOT exercised" : "NOT exercised (lane FAILED)";
    console.log(`[gate] NOTE: ${total} case(s) self-SKIPPED — coverage ${verb}:`);
    for (const s of skippedCases) {
      for (const c of s.cases) console.log(`         ${s.test}: ${c}`);
    }
  }
  return fails.length > 0 ? 1 : 0;
}

// Only run when invoked as a script — gate-integrity.test.mjs imports
// selfSkipVerdict, and an unguarded process.exit(main()) would launch a full
// native build the moment the module was imported.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exit(main());
}
