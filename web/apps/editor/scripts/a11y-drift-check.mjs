// a11y-goldens drift check (Phase 1). Regenerates the composition golden
// lane on the current tree and reports drift. Reuses run-native-tests.mjs for
// capture. (The legacy `[hwnd]` lane was removed with the legacy UI.)
//
//   node scripts/a11y-drift-check.mjs [--no-build]
//   exit 0 = no stable drift  ·  2 = drift (files on stdout)  ·  1 = error
//
// Leaves regenerated goldens in the working tree (the caller branches/commits
// them). On drift it re-captures once and reports only *stable* drift, so a
// nondeterministic normalizer gap doesn't masquerade as real drift.

import { spawnSync } from "node:child_process";
import { statSync, readdirSync, existsSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { parseChangedGoldens, stableDrift, verdict } from "./lib/drift-report.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const editorDir = resolve(__dirname, "..");
const repoRoot = resolve(__dirname, "../../../..");
const exe = join(repoRoot, "x64", "Debug", "ParticleEditor.exe");
const srcDir = join(repoRoot, "src");
const sln = join(repoRoot, "ParticleEditor.sln");
const goldensDir = "web/apps/editor/tests/a11y-goldens/";
const NO_BUILD = process.argv.includes("--no-build");

function die(msg) {
  console.error(`[a11y-drift] ERROR: ${msg}`);
  process.exit(1);
}

// Run a command, inheriting stdio; return its exit code (null -> 1).
function run(cmd, args, opts = {}) {
  const r = spawnSync(cmd, args, { stdio: "inherit", shell: false, ...opts });
  if (r.error) return { code: 1, error: r.error };
  return { code: r.status ?? 1 };
}

// Synchronous sleep (this top-level orchestrator has no async context).
function sleepSync(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms);
}

// Kill any stale --test-host instance (scoped like run-native-tests' killAny;
// the user's normal editor — launched without --test-host — is spared) so a
// retry launches into a clean slate.
function killStaleTestHost() {
  const cmd =
    "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
    "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
  spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", cmd], { stdio: "ignore", shell: false });
}

// Resolve MSBuild via vswhere (generic across machines/VS versions).
function findMsbuild() {
  const vswhere = join(
    process.env["ProgramFiles(x86)"] || "C:/Program Files (x86)",
    "Microsoft Visual Studio", "Installer", "vswhere.exe",
  );
  const r = spawnSync(vswhere, ["-latest", "-find", "MSBuild\\**\\Bin\\MSBuild.exe"], {
    encoding: "utf8", shell: false,
  });
  const path = (r.stdout || "").split(/\r?\n/).find((l) => l.trim().endsWith("MSBuild.exe"));
  return path ? path.trim() : null;
}

// Newest mtime of an actual SOURCE file under src/ (recursive) — the
// Debug-host staleness gate. Must skip MSBuild's intermediate output (src/x64/,
// incl. the .tlog/lastbuildstate written AFTER the exe is linked) and count only
// real source extensions — otherwise the build's own artifacts always look
// newer than the exe and the gate would rebuild on every run.
const BUILD_DIRS = new Set(["x64", "win32", "debug", "release", "obj", "ipch", ".vs"]);
const SOURCE_EXT = /\.(c|cc|cpp|cxx|h|hpp|hxx|inl|rc|hlsl|fx|vcxproj|filters|props|targets)$/i;
function newestSrcMtimeMs(dir) {
  let newest = 0;
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    if (entry.isDirectory()) {
      if (BUILD_DIRS.has(entry.name.toLowerCase())) continue;
      const m = newestSrcMtimeMs(join(dir, entry.name));
      if (m > newest) newest = m;
    } else if (SOURCE_EXT.test(entry.name)) {
      const m = statSync(join(dir, entry.name)).mtimeMs;
      if (m > newest) newest = m;
    }
  }
  return newest;
}

function ensureDebugHost(msbuild) {
  let needsBuild = true;
  try {
    // The exe EMBEDS web/apps/editor/dist, so it is stale when EITHER native source
    // OR the dist bundle is newer than it — not native source alone. buildWebDist()
    // runs just before this and (re)builds dist, so a web-only edit makes dist newer
    // than the exe and forces a rebuild; without the dist term the goldens would run
    // against the OLD React bundle baked into an unchanged exe.
    const distM = statSync(join(editorDir, "dist", "index.html")).mtimeMs;
    needsBuild = statSync(exe).mtimeMs < Math.max(newestSrcMtimeMs(srcDir), distM);
  } catch {
    needsBuild = true; // exe or dist missing
  }
  if (!needsBuild) {
    console.log("[a11y-drift] Debug host up to date.");
    return;
  }
  console.log("[a11y-drift] Building Debug host ...");
  // Restore once (no-op if already restored), then build Debug x64.
  run(msbuild, [sln, "/t:Restore", "/p:RestorePackagesConfig=true", "/v:minimal"], { cwd: repoRoot });
  const b = run(msbuild, [sln, "/p:Configuration=Debug", "/p:Platform=x64", "/m", "/v:minimal"], { cwd: repoRoot });
  if (b.code !== 0) die("Debug host build failed.");
}

// Build the web dist/ bundle (`tsc -b && vite build`), shell-free.
//
// The native a11y host serves the React UI from app.local -> web/apps/editor/dist;
// with no dist/ it loads ERR_NAME_NOT_RESOLVED and the goldens would capture a
// broken UI. This build used to be owned by run-native-tests' `--rebuild` path,
// removed alongside the hosting-mode dist gate in d9db84e (legacy hosting mode
// gone -> one mode -> nothing to gate) — with nothing left to build dist/. The
// orchestrator now owns it: the weekly drift task runs on a FRESH worktree whose
// gitignored dist/ is absent, so this build is load-bearing, not optional.
//
// Built UNCONDITIONALLY (not staleness-gated on web src) so the drift capture
// always reflects current source — this also closes the trap where a
// stale-but-present dist silently served the OLD UI and the run passed green
// with zero golden diff. `--no-build` (NO_BUILD) skips it for fast local re-runs.
//
// pnpm is a Windows .CMD shim that shell:false cannot launch, so spawn the node
// bins directly (mirroring the node-direct spawn pattern at run-native-tests.mjs
// :176-178). Paths mirror the deleted rebuildDist — proven under this repo's
// pnpm symlinked store.
function buildWebDist() {
  const tsc = join(editorDir, "node_modules", "typescript", "bin", "tsc");
  const vite = join(editorDir, "node_modules", "vite", "bin", "vite.js");
  const indexHtml = join(editorDir, "dist", "index.html");

  // Report a skipped `pnpm install` clearly: run() spawns `node <bin>`, so a
  // MISSING bin surfaces as node's MODULE_NOT_FOUND (exit 1, no spawn error) —
  // indistinguishable from a compile failure. Check the bins up front instead.
  for (const [pkg, bin] of [["typescript", tsc], ["vite", vite]]) {
    if (!existsSync(bin)) {
      die(`web dist/ build: ${pkg} bin missing (${bin}) — run \`pnpm install\` in web/.`);
    }
  }
  const step = (label, jsBin, args) => {
    if (run(process.execPath, [jsBin, ...args], { cwd: editorDir }).code !== 0) {
      die(`web dist/ build failed (${label}).`);
    }
  };

  console.log("[a11y-drift] Building web dist/ (tsc -b && vite build) ...");
  // Snapshot dist/index.html's mtime before the build so we can prove vite
  // actually (re)produced it. vite runs with emptyOutDir:true (it WIPES dist/
  // first), so exit 0 alone doesn't prove output landed — a no-op or partial
  // build could leave dist/ empty/stale and the goldens would capture the wrong
  // UI green (the stale-dist trap this build exists to close). Comparing two file
  // mtimes (not wall-clock) sidesteps statSync-float vs Date.now-int skew.
  let beforeMs = -1;
  try { beforeMs = statSync(indexHtml).mtimeMs; } catch { /* no prior dist */ }
  step("tsc -b", tsc, ["-b"]);
  step("vite build", vite, ["build"]);
  let afterMs = -1;
  try { afterMs = statSync(indexHtml).mtimeMs; } catch { /* still missing */ }
  if (afterMs <= beforeMs) {
    die("vite build exited 0 but dist/index.html was not (re)produced (missing or unchanged).");
  }
}

// Run one capture lane, retrying once on failure. The native host's CDP
// startup occasionally exceeds run-native-tests' 30s window under the load of
// back-to-back launches (the drift path runs four in a row); a clean-slate
// retry recovers that transient (see run-native-tests). A genuine
// failure persists across the retry and then aborts (exit 1) — never a false
// "no drift".
function runLane(extraArgs, label) {
  // Two retry layers for two failure modes:
  //  - --retries=1 (forwarded to Playwright) retries a single flaky surface
  //    in-process (e.g. a uia_inspector capture timeout) with no host relaunch.
  //  - the attempt loop below relaunches the whole lane for whole-host death /
  //    CDP-startup failures (which abort before Playwright's retries apply).
  const args = ["./scripts/run-native-tests.mjs", ...extraArgs, "--retries", "1"];
  for (let attempt = 1; attempt <= 2; attempt++) {
    const r = run(process.execPath, args, { cwd: editorDir });
    if (r.code === 0) return;
    if (attempt === 2) die(`${label} lane failed twice (exit ${r.code}).`);
    console.log(`[a11y-drift] ${label} lane failed (exit ${r.code}); cleaning up + retrying once ...`);
    killStaleTestHost();
    sleepSync(4000);
  }
}

// Regenerate the composition golden lane. The anchored grep selects only the
// golden tests (titles END in the suffix), excluding
// a11y-uia-composition-reachable.spec.ts whose describe contains [composition].
function regenerateGoldens() {
  // No --rebuild: run-native-tests dropped the dist-mode gate (and that flag)
  // in d9db84e; the web dist/ build is now owned by buildWebDist() below, run
  // once up front. Passing --rebuild here would leak to Playwright as an
  // unknown option and abort the lane.
  runLane(["--update", "--grep", "\\[composition\\]$"], "composition");
}

function changedGoldens() {
  const r = spawnSync("git", ["status", "--porcelain", "--", goldensDir], {
    cwd: repoRoot, encoding: "utf8", shell: false,
  });
  if (r.status !== 0) die("git status failed.");
  return parseChangedGoldens(r.stdout || "");
}

function restoreGoldens() {
  // Revert tracked edits AND remove any untracked goldens regeneration created,
  // so the noise path truly leaves a clean tree.
  spawnSync("git", ["checkout", "--", goldensDir], { cwd: repoRoot, stdio: "inherit", shell: false });
  spawnSync("git", ["clean", "-f", "--", goldensDir], { cwd: repoRoot, stdio: "inherit", shell: false });
}

function main() {
  // Preflight build tools (fail fast, before the expensive build).
  if (!NO_BUILD) {
    const msbuild = findMsbuild();
    if (!msbuild) die("MSBuild not found via vswhere.");
    // WEB FIRST: the C++ host build now embeds web/apps/editor/dist into the exe
    // (GenerateEmbeddedWebAssets), so building the host before the bundle fails on
    // a fresh worktree (no dist/index.html) and, on an existing tree, would bake a
    // stale UI into the exe. Build the bundle, then the host that embeds it.
    buildWebDist();
    ensureDebugHost(msbuild);
  }
  // Preflight git only (a real .exe). `node` is process.execPath (always
  // present). Do NOT probe pnpm: it's a Windows .CMD shim that shell:false
  // cannot launch (run-native-tests.mjs:60) — and the orchestrator never
  // invokes pnpm anyway (it spawns `node` directly).
  const g = spawnSync("git", ["--version"], { shell: false, encoding: "utf8" });
  if (g.error) die("git not found on PATH.");

  console.log("[a11y-drift] Pass 1 ...");
  regenerateGoldens();
  const first = changedGoldens();
  if (first.length === 0) {
    console.log("A11Y-DRIFT: none");
    process.exit(0);
  }

  // Drift seen — re-capture once and keep only stable drift.
  console.log("[a11y-drift] Drift detected; re-capturing to filter noise ...");
  regenerateGoldens();
  const second = changedGoldens();
  const v = verdict(stableDrift(first, second));
  if (v.kind === "noise") restoreGoldens();
  console.log(v.message);
  process.exit(v.exitCode);
}

main();
