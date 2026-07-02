// [gate] Aggregate runner for the standalone native unit tests (tests/test_*.cpp).
//
//   node scripts/run-native-unit-tests.mjs [flags]
//
// Enumerates tests/test_*.cpp (cpp -> bat direction, so dump_*/spike_*/make_* diag
// tools never enter the lane), builds each via its tests/build_<name>.bat, runs the
// exe, and prints a PASS/FAIL/SKIP summary. Exits nonzero on any FAIL. A needs-exe
// test whose ParticleEditor.exe is absent is a FAIL by default (a gate must not go
// green around missing coverage); pass --allow-missing-exe to downgrade it to a
// VISIBLE SKIP when iterating without an app build. Hard-errors up front if any
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
//   --timeout <secs>       per-test run timeout (default 120; hung test = FAIL)
//   --list                 print the resolved test list and exit

import { spawnSync } from "node:child_process";
import { readdirSync, existsSync, statSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const testsDir = join(repoRoot, "tests");

// Tests that validate a built ParticleEditor.exe rather than standalone logic.
const NEEDS_EXE = new Set(["test_resource_strings"]);

const argv = process.argv.slice(2);
function argValue(name, fallback) {
  const i = argv.indexOf(name);
  return i >= 0 && argv[i + 1] !== undefined ? argv[i + 1] : fallback;
}
const FILTER = argValue("--filter", null);
const SKIP_BUILD = argv.includes("--skip-build");
const ONLY_NEEDS_EXE = argv.includes("--only-needs-exe");
const EXCLUDE_NEEDS_EXE = argv.includes("--exclude-needs-exe");
const LIST = argv.includes("--list");
const ALLOW_MISSING_EXE = argv.includes("--allow-missing-exe");
const TIMEOUT_MS = Number(argValue("--timeout", "120")) * 1000;
const APP_EXE = resolve(repoRoot, argValue("--exe", join("x64", "Debug", "ParticleEditor.exe")));

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
    const r = spawnSync(exe, NEEDS_EXE.has(name) ? [APP_EXE] : [], {
      cwd: repoRoot,
      stdio: "inherit",
      shell: false,
      timeout: TIMEOUT_MS,
    });
    if (r.error || r.signal) record("FAIL", r.signal ? `timeout/killed (${r.signal})` : String(r.error));
    else if (r.status !== 0) record("FAIL", `exit ${r.status}`);
    else record("PASS");
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
  return fails.length > 0 ? 1 : 0;
}

process.exit(main());
