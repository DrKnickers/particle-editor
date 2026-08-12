// [gate] Unified test gate — runs every automated test layer, exits nonzero on
// any failure.
//
//   node scripts/run-all-tests.mjs [flags]
//
// Lanes, cheap/fast first (later lanes depend on earlier build lanes):
//   lint              tsc --noEmit                        (web/apps/editor)
//   vitest            vitest run — the web unit/component suite
//   web-build         tsc -b && vite build -> dist/ (proof: dist/index.html mtime
//                     must ADVANCE — vite exit 0 alone doesn't prove output)
//   scripts           node --test script libs — MUST follow web-build: the
//                     no-test-seam-in-prod guard silently self-skips without dist/
//   playwright-web    mock-browser Playwright lane (own Vite server)
//   site              landing + guide static-site Playwright (own node server;
//                     global-setup renders placeholder media — warns if FFmpeg absent)
//   cpp-unit          all standalone tests/test_*.cpp except needs-exe ones
//   msbuild-debug     x64 Debug ParticleEditor.sln (test:native launches this exe)
//   cpp-unit-exe      needs-exe native tests against the FRESH Debug exe (explicit
//                     path — bare test_resource_strings would prefer stale Release)
//   playwright-native all registered specs vs the real app over CDP (pnpm test:native)
//   msbuild-release   x64 Release (drive-smoke hard-requires the Release exe)
//   render-goldens    deterministic --capture scenes vs checked-in goldens
//                     (ffmpeg SSIM ≥ 0.9995 — see scripts/render-goldens.mjs)
//   drive-smoke       tasks/drive-smoke.ps1 — real-pixel non-black --drive smoke
//                     + the oracle-step scenarios (assert-state / nonblack /
//                     production-wire bridge-selftest)
//
// Missing prereqs are FAILURES with actionable messages by default; silent skips
// are the enemy of a gate. `--allow-missing <lane>` downgrades that lane's missing
// prereq to a visible SKIP. A lane failure SKIPs lanes that depend on it (visible,
// counted as blocked, overall run still fails).
//
// Skipped TESTS *inside* an exit-0 lane are held to the declared SKIP_BUDGET
// below — same principle one level down, and the same --allow-missing hatch.
//
// Deliberately NOT in the default gate: a11y:drift (mutates goldens; test:native
// already runs a11y specs). test:site now runs as the `site` lane (#599) — its
// FFmpeg-dependent placeholder render degrades to a warning outside CI, so the guide
// suite still runs without FFmpeg.
//
// Flags:
//   --lane <a,b,…>        run only these lanes (deps NOT auto-added)
//   --allow-missing <a,b> downgrade missing-prereq FAIL to SKIP for these lanes
//   --skip-build          skip web-build/msbuild lanes + native unit rebuilds
//                         (UNSAFE: asserts current artifacts are fresh)
//   --list                print lanes and exit
//   --help, -h            print usage and exit
//
// An unknown flag is an ERROR (usage + exit 1), never a silent fall-through to
// the full unscoped gate — which, run by accident, kicks off the multi-minute
// MSBuild/game-install lanes.

import { spawnSync } from "node:child_process";
import { existsSync, statSync, writeFileSync, readFileSync, unlinkSync, readdirSync, rmSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const editorDir = join(repoRoot, "web", "apps", "editor");
const sln = join(repoRoot, "ParticleEditor.sln");
const debugExe = join(repoRoot, "x64", "Debug", "ParticleEditor.exe");
const releaseExe = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
const distIndex = join(editorDir, "dist", "index.html");
const smokeFixture = join(editorDir, "tests", "fixtures", "a11y-base-state.alo");
const lockPath = join(repoRoot, ".gate.lock");

// ---------------------------------------------------------------------------
// Argument parsing. Kept as a pure function (no process.* reads, no side
// effects) so a unit test can assert the grammar — in particular that an
// unrecognized flag or a bare --help lands in `unknown`/`help` rather than
// being silently ignored, which is what let `--help` fall through to the full
// unscoped gate.
const splitList = (s) => s.split(",").map((x) => x.trim()).filter(Boolean);

export function parseArgs(argv) {
  const lane = [];
  const allowMissing = [];
  const unknown = [];
  let skipBuild = false, list = false, help = false;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    // --lane / --allow-missing each consume the next token (a value or comma
    // list) and are repeatable; consuming it here also keeps that value out of
    // `unknown`.
    if (a === "--lane") { if (argv[i + 1] !== undefined) lane.push(...splitList(argv[++i])); continue; }
    if (a === "--allow-missing") { if (argv[i + 1] !== undefined) allowMissing.push(...splitList(argv[++i])); continue; }
    if (a === "--skip-build") { skipBuild = true; continue; }
    if (a === "--list") { list = true; continue; }
    if (a === "--help" || a === "-h") { help = true; continue; }
    unknown.push(a);
  }
  return { lane, allowMissing, skipBuild, list, help, unknown };
}

export function usage() {
  return [
    "Unified test gate — runs the automated test lanes; exits nonzero on any failure.",
    "",
    "Usage: node scripts/run-all-tests.mjs [flags]",
    "",
    "Flags:",
    "  --lane <a,b,…>         run only these lanes (repeatable; deps NOT auto-added)",
    "  --allow-missing <a,b>  downgrade a lane's missing-prereq FAIL — or an",
    "                         over-budget internal-skip FAIL — to a visible SKIP",
    "  --skip-build           skip build lanes + native rebuilds (UNSAFE: assumes fresh artifacts)",
    "  --list                 print the lane names and exit",
    "  --help, -h             print this help and exit",
    "",
    "With no flags, runs the full gate. Use --list to see every lane name.",
  ].join("\n");
}

const argv = process.argv.slice(2);
const ARGS = parseArgs(argv);
const ONLY = ARGS.lane;
const ALLOW_MISSING = new Set(ARGS.allowMissing);
const SKIP_BUILD = ARGS.skipBuild;
const LIST = ARGS.list;

// A gate run must never reuse a Playwright webServer someone else started. Both
// Playwright configs default `reuseExistingServer` to !CI, and this gate
// deliberately never sets CI — so on a box running two worktrees at once (the
// normal workflow here) the second run silently attached to the FIRST
// checkout's Vite/site server and tested the wrong tree. Breaking App.tsx in
// this checkout could leave the lane green (2026-07 audit).
//
// Set for the whole process rather than per-lane: only those two configs read
// it, and a lane that spawns no server is unaffected. Devs running
// `pnpm test:web` by hand keep server reuse.
process.env.PE_GATE_NO_REUSE = "1";

function log(msg) {
  console.log(`[gate] ${msg}`);
}

// ---------------------------------------------------------------------------
// Spawn helpers. .bat/.CMD shims (pnpm) can't be launched with shell:false, so
// they go through cmd.exe /d /s /c with verbatim args — the only reliable form
// for a repo path containing spaces ("Particle Editor").
function runCmdLine(commandLine, cwd) {
  const r = spawnSync("cmd.exe", ["/d", "/s", "/c", `"${commandLine}"`], {
    cwd,
    stdio: "inherit",
    shell: false,
    windowsVerbatimArguments: true,
  });
  return r.error ? 1 : (r.status ?? 1);
}
function runExe(cmd, args, cwd = repoRoot) {
  const r = spawnSync(cmd, args, { cwd, stdio: "inherit", shell: false });
  return r.error ? 1 : (r.status ?? 1);
}

// Like runCmdLine, but TEES: the child's output still reaches the console while
// also being returned for inspection. Used by lanes whose runners can report
// skipped TESTS inside an exit-0 run — the gate used to be blind to those (a
// lane could pass while silently executing nothing), which the 2026-07 audit
// demonstrated with a stub reporting "0 passed, 1 skipped".
function runCmdLineTee(commandLine, cwd) {
  const r = spawnSync("cmd.exe", ["/d", "/s", "/c", `"${commandLine}"`], {
    cwd,
    encoding: "utf8",
    shell: false,
    windowsVerbatimArguments: true,
  });
  const out = `${r.stdout || ""}${r.stderr || ""}`;
  if (out) process.stdout.write(out);
  return { code: r.error ? 1 : (r.status ?? 1), out };
}

// The 8-byte PNG signature every real frame starts with.
export const PNG_SIGNATURE = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

// Oracle for the record-smoke lane. Returns a failure string, or null when the
// run genuinely produced renderable frames.
//
// Extracted and exported so scripts/gate-integrity.test.mjs can exercise it
// directly: we read this lane as PROOF that headless recording works, and it
// could not previously fail. It captured the spawn result and then used
// `status` only inside failure message STRINGS — never as a condition — while
// discarding `error`, `signal` and stderr entirely (stdio was "ignore"). A
// recorder that wrote frames and then crashed, or hit the timeout, passed.
// Its whole oracle was "≥1 file named frame_N.png" plus "largest ≥ 20 KB",
// so equally-large garbage under the right filename passed too
// (2026-07 audit).
export function recordSmokeVerdict({ error, signal, status, stderr, frames, maxBytes, biggest, header }) {
  const tail = (s) => String(s || "").split(/\r?\n/).filter(Boolean).slice(-4).join(" | ");
  if (error)  return `headless --record could not run: ${error}`;
  if (signal) return `headless --record was killed (${signal}; the 90s timeout is the usual cause) — stderr: ${tail(stderr)}`;
  if (status !== 0) return `headless --record exited ${status} — stderr: ${tail(stderr)}`;
  if (!frames || frames.length === 0) return "headless --record produced 0 frames";
  // A real 1280x960 UI render PNG is tens-to-hundreds of KB; a blank/black
  // frame compresses to a few KB. 20KB cleanly separates them.
  if (maxBytes < 20000) return `headless --record frames look blank (max ${maxBytes} bytes across ${frames.length})`;
  // Size alone cannot tell a PNG from equally-large garbage wearing the right
  // filename. Check the signature.
  if (!header || !Buffer.from(header).subarray(0, 8).equals(PNG_SIGNATURE)) {
    return `headless --record wrote ${biggest ?? "a frame"} (${maxBytes} bytes) but it is not a PNG`;
  }
  return null;
}

// Largest skipped-test count a runner reports. Returns 0 when it makes no such
// claim.
//
// TWO word orders, because the two runners disagree and only one was handled.
// Playwright says "4 skipped" / "3 did not run"; `node --test` says
// "ℹ skipped 1" — noun FIRST. Only the Playwright form was matched, so the
// `scripts` lane (node --test) always parsed 0 and its FFmpeg-dependent skips
// stayed invisible even after they were supposedly surfaced (2026-07 audit —
// the visibility half of that finding never actually worked).
// Both patterns are newline-anchored ([ \t], not \s) because every runner puts
// its count and its noun on ONE line, while \s crosses lines and Playwright's
// tail happens to read "  1 skipped\n  196 passed (114.5s)" — which a \s form
// reads as "skipped 196". The gate caught exactly that.
export function parseSkippedCount(out) {
  let n = 0;
  for (const m of out.matchAll(/(\d+)[ \t]+(?:skipped|did not run)\b/gi)) {
    n = Math.max(n, Number(m[1]));
  }
  for (const m of out.matchAll(/\bskipped[ \t]+(\d+)\b/gi)) {
    n = Math.max(n, Number(m[1]));
  }
  return n;
}
// Declared budget of skipped TESTS a lane may report while still exiting 0.
//
// #687 made internal skips visible; visibility alone is not a gate. "NOTE:
// playwright-native: 1" reads identically whether the 1 is the known skip or a
// freshly-broken test that quietly stopped running — the number carried no
// claim about WHICH test, so any new skip hid behind the old one. Declaring the
// budget turns that NOTE into an assertion, and forces the reason to be written
// down (which is how the entry below got found).
//
// A lane with no entry budgets ZERO. That is the point: the default for a new
// lane is "runs everything it claims to".
// Currently EMPTY, and that is the goal state: every lane runs everything it
// claims to. The one entry this started with — playwright-native's single skip —
// turned out not to be a capability skip at all but a spec whose precondition
// could never hold, and it was rewritten rather than budgeted (see
// composition-hosting.spec.ts). Add an entry only for a skip that is genuinely
// impossible on some machine, and write the reason: the reason is the part that
// gets checked.
export const SKIP_BUDGET = {
  // Eight scripts-lane tests self-skip on the public mirror: five inspect the
  // PRIVATE clip-source data (tasks/wiki-media manifest + timelines) and three
  // pre-existing ones lint unpublished timeline fixtures. The entry exists ONLY
  // when that data is absent — never on the private repo — so the private gate
  // keeps a zero budget (no permanent "tighten" nag) and the public tree passes
  // at exactly its budgeted skips; a ninth quiet skip still fails both sides.
  ...(existsSync(join(repoRoot, "tasks", "wiki-media", "manifest.json"))
    ? {}
    : { scripts: { max: 8, why: "maintainer-only clip-source data and timeline fixtures (tasks/wiki-media, .claude/skills) are absent on the public mirror" } }),
};

// Verdict for a lane that exited 0 while reporting `innerSkipped` skipped tests.
// Over budget FAILS (the false-green the audit describes for the FFmpeg-dependent
// script tests); under budget passes but says the budget is now loose, because a
// budget nobody tightens is how the number drifts upward. Exceeding it is
// overridable with the same --allow-missing <lane> hatch #706 established, for
// the machine that genuinely lacks the capability.
export function skipBudgetVerdict(lane, innerSkipped, allowMissing = ALLOW_MISSING, budget = SKIP_BUDGET) {
  const allowed = budget[lane]?.max ?? 0;
  if (innerSkipped > allowed) {
    const over = `${innerSkipped} skipped test(s), budget ${allowed}`;
    return allowMissing.has(lane)
      ? { ok: true, note: `${over} — allowed via --allow-missing ${lane}` }
      : { ok: false, note: `${over} — un-run coverage (fix, re-budget, or --allow-missing ${lane})` };
  }
  if (innerSkipped < allowed) {
    return { ok: true, note: `only ${innerSkipped} of ${allowed} budgeted skip(s) — tighten SKIP_BUDGET.${lane}` };
  }
  return { ok: true, note: null };
}

// Apply the budget to a lane that already exited 0.
function withSkipBudget(lane, innerSkipped) {
  const v = skipBudgetVerdict(lane, innerSkipped);
  if (!v.ok) return fail(v.note);
  if (innerSkipped === 0 && !v.note) return pass;
  return { ...pass, innerSkipped, ...(v.note ? { note: v.note } : {}) };
}

function psCapture(command) {
  const r = spawnSync(
    "powershell.exe",
    ["-NoProfile", "-NonInteractive", "-Command", command],
    { encoding: "utf8", shell: false },
  );
  return { code: r.status ?? 1, out: (r.stdout || "").trim() };
}

// Resolve MSBuild via vswhere (same locator as a11y-drift-check.mjs — do not
// invent a third one).
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

// Newest mtime among the inputs a native build consumes. Used to prove a build
// lane actually produced current output: an incremental no-op build legitimately
// leaves the exe untouched, but the exe must still be NEWER than every source it
// is built from. A stubbed/no-op'd MSBuild that exits 0 without linking leaves a
// stale exe older than the sources, which is exactly the false green the 2026-07
// audit demonstrated (it exited 0 with an unchanged hash and timestamp).
function newestSourceMtime() {
  let newest = 0;
  const walk = (dir) => {
    let entries;
    try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      const full = join(dir, e.name);
      if (e.isDirectory()) {
        if (e.name === "packages" || e.name === "third_party") continue;
        walk(full);
      } else if (/\.(cpp|h|hpp|rc|vcxproj|fx)$/i.test(e.name)) {
        try { newest = Math.max(newest, statSync(full).mtimeMs); } catch { /* raced */ }
      }
    }
  };
  walk(join(repoRoot, "src"));
  for (const f of [sln]) {
    try { newest = Math.max(newest, statSync(f).mtimeMs); } catch { /* absent */ }
  }
  return newest;
}

// Newest mtime under the editor's WEB source. The C++ build now EMBEDS
// web/apps/editor/dist into the exe, so if web source is newer than dist the
// embedded bundle is stale — an msbuild lane run in isolation (--lane, which does
// NOT auto-add web-build) would otherwise rebuild the exe around old UI and pass.
function newestWebSourceMtime() {
  let newest = 0;
  // Every file, ANY extension: an imported image/font under src/ or a static file
  // under public/ is a bundle input just like a .tsx, so an extension filter would
  // miss them (reviewer round-4). But SKIP test-only files — editing a test is not a
  // bundle change and must not block an isolated MSBuild lane (reviewer round-5).
  const isTestFile = (n) => /\.(test|spec)\.[cm]?[jt]sx?$/i.test(n) || n === "test-setup.ts";
  const walkAll = (dir) => {
    let entries;
    try { entries = readdirSync(dir, { withFileTypes: true }); } catch { return; }
    for (const e of entries) {
      if (e.isDirectory()) {
        if (e.name === "node_modules" || e.name === "dist" || e.name === "__tests__" || e.name === "__mocks__") continue;
        walkAll(join(dir, e.name));
      } else if (!isTestFile(e.name)) {
        try { newest = Math.max(newest, statSync(join(dir, e.name)).mtimeMs); } catch { /* raced */ }
      }
    }
  };
  walkAll(join(editorDir, "src"));                       // components + imported static assets
  walkAll(join(editorDir, "public"));                    // files copied verbatim into dist (e.g. the font)
  walkAll(join(repoRoot, "web", "packages"));            // workspace pkgs the bundle imports (e.g. bridge-schema)
  for (const f of ["index.html", "vite.config.ts", "package.json", "app-version.ts",
                   "tsconfig.json", "tsconfig.app.json", "tailwind.config.ts", "postcss.config.js"]) {
    try { newest = Math.max(newest, statSync(join(editorDir, f)).mtimeMs); } catch { /* absent */ }
  }
  // Cross-language input: web/apps/editor/app-version.ts derives PE_VERSION_STR from
  // the C++ header, so a version bump changes the bundle.
  try { newest = Math.max(newest, statSync(join(repoRoot, "src", "version.h")).mtimeMs); } catch { /* absent */ }
  return newest;
}

// Assert a build lane's binary exists and is not stale relative to its sources.
// Returns null when fresh, or a failure note.
export function staleBinaryNote(exePath, label, newestSrcMs = newestSourceMtime()) {
  if (!existsSync(exePath)) return `${label} produced no ${exePath}`;
  const exeM = statSync(exePath).mtimeMs;
  const srcM = newestSrcMs;
  if (srcM > 0 && exeM < srcM) {
    return `${label} exited 0 but ${exePath} is OLDER than the newest source ` +
           `(exe ${new Date(exeM).toISOString()} < src ${new Date(srcM).toISOString()}) — stale binary`;
  }
  return null;
}

let msbuildPath = null;
let restored = false;
// NuGet restore, once per process. Broken out of msbuild() so a lane that builds
// package-dependent code WITHOUT a full msbuild() call can still guarantee the
// packages exist — the cpp-unit lane's test_startup_callback_guard compiles a TU
// that includes WebView2.h from the Microsoft.Web.WebView2 package. Returns 0 on
// success (or if already restored), nonzero if MSBuild is missing or restore fails.
function ensureRestored() {
  if (restored) return 0;
  if (!msbuildPath) msbuildPath = findMsbuild();
  if (!msbuildPath) {
    log("MSBuild not found via vswhere — install VS with the C++ workload.");
    return 1;
  }
  if (runExe(msbuildPath, [sln, "/t:Restore", "/p:RestorePackagesConfig=true", "/v:minimal"]) !== 0) return 1;
  restored = true;
  return 0;
}
function msbuild(config) {
  if (!msbuildPath) msbuildPath = findMsbuild();
  if (!msbuildPath) {
    log("MSBuild not found via vswhere — install VS with the C++ workload.");
    return 1;
  }
  if (ensureRestored() !== 0) return 1;
  return runExe(msbuildPath, [sln, `/p:Configuration=${config}`, "/p:Platform=x64", "/m", "/v:minimal"]);
}

// Build a single vcxproj for one x64 config, reusing the same MSBuild locator as
// msbuild() above. Used to materialize a prerequisite static lib without paying
// for a full-solution build. No /t:Restore: this is only used for package-free
// projects (e.g. expatw_static). Returns the process exit code.
function msbuildProject(projectPath, config) {
  if (!msbuildPath) msbuildPath = findMsbuild();
  if (!msbuildPath) {
    log("MSBuild not found via vswhere — install VS with the C++ workload.");
    return 1;
  }
  return runExe(msbuildPath, [projectPath, `/p:Configuration=${config}`, "/p:Platform=x64", "/m", "/v:minimal"]);
}

// ---------------------------------------------------------------------------
// Lane bodies. Each returns { status: "PASS"|"FAIL"|"SKIP", note? }.
const pass = { status: "PASS" };
const fail = (note) => ({ status: "FAIL", note });
const skip = (note) => ({ status: "SKIP", note });

// Render-capability preflight for the D3D9 desktop lanes. When the workstation
// is locked or on the secure/logon desktop — e.g. the nightly gate firing at
// 04:37 on a locked box — DWM stops compositing and a D3D9 present yields a
// BLACK frame, so render-goldens / drive-smoke / playwright-native fail as a
// FALSE rendering regression (#745). Detect the clear locked case and SKIP those
// lanes VISIBLY instead of running them into a black-frame failure. Deliberately
// conservative: LogonUI.exe running means the secure desktop is up (locked or a
// credential prompt) — high confidence, near-zero false positives. It does NOT
// catch every non-render state (e.g. a display asleep while unlocked), so the
// lanes keep their own black-frame asserts as the backstop; this only converts
// the common, unambiguous locked-session failure into a visible skip.
let _renderDesktop = null;
function renderDesktopUsable() {
  if (_renderDesktop) return _renderDesktop;
  if (process.platform !== "win32") return (_renderDesktop = { ok: true });
  // Scope to THIS gate's session: a LogonUI in a DIFFERENT session (a locked RDP
  // or Fast-User-Switching session) must not make an otherwise-usable gate
  // session skip its render lanes. The spawned PowerShell inherits the node
  // process's session, so its own SessionId is the gate's session.
  const r = psCapture(
    "$s = (Get-Process -Id $PID).SessionId; " +
    "if (Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $s }) { 'LOCKED' } else { 'OK' }",
  );
  // Probe error → don't block: assume usable and let the lane's own checks rule.
  _renderDesktop = r.out === "LOCKED"
    ? { ok: false, reason: "workstation locked / secure desktop active (LogonUI running)" }
    : { ok: true };
  return _renderDesktop;
}

// Missing prereq -> FAIL unless the user explicitly allowed it for this lane.
function prereq(lane, ok, what, hint) {
  if (ok) return null;
  const msg = `${what} — ${hint}`;
  if (ALLOW_MISSING.has(lane)) return skip(msg);
  log(`${lane}: missing prereq: ${msg} (or pass --allow-missing ${lane})`);
  return fail(`missing prereq: ${what}`);
}

const LANES = [
  {
    name: "lint",
    run: () => (runCmdLine("pnpm run lint", editorDir) === 0 ? pass : fail("tsc --noEmit")),
  },
  {
    name: "vitest",
    run: () => (runCmdLine("pnpm run test", editorDir) === 0 ? pass : fail("vitest run")),
  },
  {
    name: "web-build",
    run: () => {
      if (SKIP_BUILD) return skip("--skip-build");
      let before = -1;
      try { before = statSync(distIndex).mtimeMs; } catch { /* no prior dist */ }
      if (runCmdLine("pnpm run build", editorDir) !== 0) return fail("tsc -b && vite build");
      let after = -1;
      try { after = statSync(distIndex).mtimeMs; } catch { /* still missing */ }
      if (after <= before) return fail("build exited 0 but dist/index.html was not (re)produced");
      return pass;
    },
  },
  {
    name: "scripts",
    deps: ["web-build"],
    run: () => {
      // The no-test-seam-in-prod guard self-skips without dist/; require it.
      const p = prereq("scripts", existsSync(distIndex), "web dist/ missing", "run the web-build lane first");
      if (p) return p;
      // ...and require it to be FRESH. `existsSync` alone let the seam guard
      // scan a bundle built before the change under test, so reintroducing a
      // dev-only test seam could pass against yesterday's dist. The lane
      // declares deps on web-build, but `--lane scripts` on its own bypasses
      // that (2026-07 audit).
      const staleDist = staleBinaryNote(distIndex, "scripts", newestSourceMtime());
      if (staleDist) return fail(staleDist);

      // Tee + count: several script tests self-skip when FFmpeg is absent, and
      // the lane reported a bare PASS while their coverage never ran
      // (2026-07 audit). Visibility was the first half; the budget below is the
      // second — this lane declares no skips, so on a box with FFmpeg (where
      // these tests CAN run) a skip is a real problem and fails. A box without
      // it says so with --allow-missing scripts.
      const r = runCmdLineTee("pnpm run test:scripts", editorDir);
      if (r.code !== 0) return fail("node --test");
      return withSkipBudget("scripts", parseSkippedCount(r.out));
    },
  },
  {
    name: "playwright-web",
    run: () => (runCmdLine("pnpm run test:web", editorDir) === 0 ? pass : fail("mock-browser Playwright")),
  },
  {
    name: "site",
    // Landing + guide static-site Playwright. Self-contained: serve.mjs hosts the
    // committed repo-root site/ (no dist/, no exe). global-setup renders placeholder
    // landing media via FFmpeg. In-gate so guide.spec can't silently go stale
    // again (#599).
    run: () => {
      // The landing playback specs `test.skip()` themselves when that
      // placeholder media is absent, and the media only exists if global-setup
      // could run FFmpeg. Outside CI that degraded to a warning, so a box with
      // no FFmpeg reported a GREEN site lane having never exercised playback at
      // all — break video playback and the gate still passes. Treat FFmpeg as
      // the real prereq it is: FAIL by default, `--allow-missing site` for a
      // VISIBLE skip. Same shape as drive-smoke's registry/fixture checks.
      const ff = spawnSync("ffmpeg", ["-version"], { encoding: "utf8", shell: false });
      const p = prereq(
        "site",
        ff.status === 0,
        "ffmpeg not on PATH (the landing playback specs silently skip without it)",
        "install ffmpeg so global-setup can render the placeholder media",
      );
      if (p) return p;
      return runCmdLine("pnpm run test:site", editorDir) === 0
        ? pass
        : fail("landing + guide Playwright");
    },
  },
  {
    name: "cpp-unit",
    run: () => {
      // Four expat-linking tests (test_xml_billion_laughs -> Release, the other
      // three -> Debug) link a prebuilt expatw_static.lib via /LIBPATH but never
      // build it. On a fresh worktree that lib doesn't exist yet — the msbuild
      // lanes that produce it run AFTER this one — so the link fails with
      // LNK1181. Materialize it here (both configs) as a prereq. Building just
      // this one small static-lib project keeps the lane early and fast; a dev
      // machine with a prior full build simply rebuilds it as a no-op. (#483)
      if (!SKIP_BUILD) {
        // test_startup_callback_guard compiles StartupCallbackAdapter.cpp, which
        // includes WebView2.h from the Microsoft.Web.WebView2 NuGet package. That
        // package is restored by the LATER msbuild lanes, so on a fresh worktree
        // (e.g. the nightly gate) the header is absent and the test build fails
        // with C1083 — a false cpp-unit failure. Restore here as a prereq, the
        // same shape as the expat static-lib prereq below. (#745)
        if (ensureRestored() !== 0) {
          return fail("NuGet restore (prereq for the WebView2-dependent unit test)");
        }
        const expatProj = join(repoRoot, "libs", "expat-2.2.0", "expatw_static.vcxproj");
        for (const cfg of ["Debug", "Release"]) {
          if (msbuildProject(expatProj, cfg) !== 0) {
            return fail(`expatw_static ${cfg} build (prereq for expat-linking unit tests)`);
          }
        }
      }
      const args = [join(repoRoot, "scripts", "run-native-unit-tests.mjs"), "--exclude-needs-exe"];
      if (SKIP_BUILD) args.push("--skip-build");
      return runExe(process.execPath, args) === 0 ? pass : fail("native unit lane");
    },
  },
  {
    name: "msbuild-debug",
    // The C++ build now EMBEDS the web bundle (RCDATA via the vcxproj
    // GenerateEmbeddedWebAssets target reading web/apps/editor/dist), so a
    // fresh dist/ is a genuine build input — order after web-build and block
    // if it failed.
    deps: ["web-build"],
    run: () => {
      if (SKIP_BUILD) return skip("--skip-build");
      // A dist older than web source means the build just embedded a STALE UI into
      // the exe. Catch it here so `--lane msbuild-debug` (no auto web-build dep)
      // can't false-green on old web content.
      const staleDist = staleBinaryNote(distIndex, "msbuild-debug (embedded web dist)", newestWebSourceMtime());
      if (staleDist) return fail(staleDist);
      if (msbuild("Debug") !== 0) return fail("x64 Debug build");
      const stale = staleBinaryNote(debugExe, "msbuild-debug");
      return stale ? fail(stale) : pass;
    },
  },
  {
    name: "cpp-unit-exe",
    deps: ["msbuild-debug"],
    run: () => {
      const p = prereq("cpp-unit-exe", existsSync(debugExe), "x64/Debug/ParticleEditor.exe missing", "run the msbuild-debug lane first");
      if (p) return p;
      const args = [
        join(repoRoot, "scripts", "run-native-unit-tests.mjs"),
        "--only-needs-exe", "--exe", debugExe,
      ];
      if (SKIP_BUILD) args.push("--skip-build");
      return runExe(process.execPath, args) === 0 ? pass : fail("needs-exe native tests");
    },
  },
  {
    name: "playwright-native",
    deps: ["web-build", "msbuild-debug"],
    run: () => {
      const rd = renderDesktopUsable();
      if (!rd.ok) return skip(`no interactive render desktop — ${rd.reason}`);
      let p = prereq("playwright-native", existsSync(debugExe), "x64/Debug/ParticleEditor.exe missing", "run the msbuild-debug lane first");
      if (!p) p = prereq("playwright-native", existsSync(distIndex), "web dist/ missing", "run the web-build lane first");
      if (p) return p;
      // Preflight CDP port 9222. A stale --test-host is fine (run-native-tests
      // kills those itself); anything ELSE owning the port is an environment
      // error, distinct from a test failure. A daily-driver editor without
      // --test-host doesn't listen on 9222 and must never be flagged or killed.
      const probe = psCapture(
        "$c = Get-NetTCPConnection -LocalPort 9222 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1; " +
        "if (-not $c) { 'FREE' } else { " +
        "$p = Get-CimInstance Win32_Process -Filter \"ProcessId=$($c.OwningProcess)\"; " +
        "if ($p.Name -eq 'ParticleEditor.exe' -and $p.CommandLine -like '*--test-host*') { 'TESTHOST' } " +
        "else { \"BUSY $($p.Name) pid $($p.ProcessId)\" } }",
      );
      if (probe.out.startsWith("BUSY")) {
        return fail(`CDP port 9222 owned by another process (${probe.out.slice(5)}) — close it and re-run`);
      }
      if (probe.code !== 0 || probe.out === "") {
        // Inconclusive probe (powershell/cmdlet failure): warn and proceed — a
        // genuinely busy port still fails loudly inside the lane itself.
        log(`playwright-native: port preflight inconclusive (probe exit ${probe.code}); proceeding.`);
      }
      const r = runCmdLineTee("pnpm run test:native", editorDir);
      if (r.code !== 0) return fail("native Playwright");
      // Skipped TESTS inside this passing lane are held to SKIP_BUDGET, so a
      // SECOND test going quiet can no longer hide behind the known one.
      return withSkipBudget("playwright-native", parseSkippedCount(r.out));
    },
  },
  {
    name: "msbuild-release",
    // Embeds the web bundle at compile time (see msbuild-debug) — depends on a
    // fresh dist/.
    deps: ["web-build"],
    run: () => {
      if (SKIP_BUILD) return skip("--skip-build");
      const staleDist = staleBinaryNote(distIndex, "msbuild-release (embedded web dist)", newestWebSourceMtime());
      if (staleDist) return fail(staleDist);
      if (msbuild("Release") !== 0) return fail("x64 Release build");
      const stale = staleBinaryNote(releaseExe, "msbuild-release");
      return stale ? fail(stale) : pass;
    },
  },
  {
    name: "render-goldens",
    deps: ["msbuild-release"],
    run: () => {
      const rd = renderDesktopUsable();
      if (!rd.ok) return skip(`no interactive render desktop — ${rd.reason}`);
      const p = prereq("render-goldens", existsSync(releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first");
      if (p) return p;
      return runExe(process.execPath, [join(repoRoot, "scripts", "render-goldens.mjs")]) === 0
        ? pass
        : fail("render goldens (bless intentional changes with scripts/render-goldens.mjs --update)");
    },
  },
  {
    // #510: the ONLY lane that exercises `--record` (render-goldens uses
    // --capture, drive-smoke uses --drive). Records ~15 frames of a
    // self-contained fixture .alo (no game install) and asserts the output got
    // >0 NON-BLANK PNGs — the coverage whose absence let #510 (0-frame records)
    // go unnoticed. Drives the HEADLESS path (PE_RECORD_HEADLESS=1 +
    // --record-minimized) so it guards the offscreen-composition + message-ack
    // mechanism the #510 fix rests on (and runs without popping a window onto
    // the gate desktop). Frame count + a max-byte floor are the pass signals: a
    // blank/black capture (e.g. composition-disabled session) yields tiny PNGs,
    // so the floor catches the loud→silent shift GrabWindowPixels would allow.
    name: "record-smoke",
    deps: ["msbuild-release", "web-build"],
    run: () => {
      const checks = [
        [existsSync(releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first"],
        [existsSync(distIndex), "web dist/ missing (headless record renders the UI)", "run the web-build lane first"],
      ];
      for (const [ok, what, hint] of checks) { const p = prereq("record-smoke", ok, what, hint); if (p) return p; }
      const outDir = join(repoRoot, "tests", ".tmp", "record-smoke-frames");
      const clean = () => { rmSync(outDir, { recursive: true, force: true }); rmSync(outDir + ".tmp", { recursive: true, force: true }); };
      clean();
      // stderr is PIPED, not ignored: it is the only explanation we get when the
      // recorder dies, and this lane used to discard it. stdout stays ignored —
      // a headless record is chatty enough to risk the default maxBuffer.
      const r = spawnSync(releaseExe,
        ["--record", join(repoRoot, "tests", "record-smoke.timeline.json"), "--record-minimized"],
        { cwd: repoRoot, encoding: "utf8", timeout: 90000, stdio: ["ignore", "ignore", "pipe"],
          env: { ...process.env, PE_RECORD_HEADLESS: "1" } });
      let frames = [];
      try { frames = readdirSync(outDir).filter((f) => /^frame_\d+\.png$/.test(f)); }
      catch { /* out dir missing → 0 frames (record aborted before the tmp→out rename) */ }
      let maxBytes = 0;
      let biggest = null;
      for (const f of frames) {
        try {
          const n = statSync(join(outDir, f)).size;
          if (n > maxBytes) { maxBytes = n; biggest = f; }
        } catch { /* raced */ }
      }
      // Read the header BEFORE clean() removes the directory.
      let header = null;
      if (biggest) {
        try { header = readFileSync(join(outDir, biggest)).subarray(0, 8); } catch { /* raced */ }
      }
      clean();

      const verdict = recordSmokeVerdict({
        error: r.error, signal: r.signal, status: r.status, stderr: r.stderr,
        frames, maxBytes, biggest, header,
      });
      return verdict ? fail(verdict) : pass;
    },
  },
  {
    name: "drive-smoke",
    deps: ["msbuild-release", "web-build"],
    run: () => {
      const rd = renderDesktopUsable();
      if (!rd.ok) return skip(`no interactive render desktop — ${rd.reason}`);
      // drive-smoke.ps1 only self-checks the exe; the gate owns the full prereq
      // trio + the fixture its A3 scenario silently skips without.
      const reg = spawnSync("reg.exe", ["query", "HKCU\\Software\\AloParticleEditor", "/v", "GameDataPath"], {
        encoding: "utf8", shell: false,
      });
      const checks = [
        [existsSync(releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first"],
        [existsSync(distIndex), "web dist/ missing", "run the web-build lane first"],
        [reg.status === 0, "HKCU GameDataPath not set", "launch the editor once and pick the game data folder"],
        [existsSync(smokeFixture), `smoke fixture missing (${smokeFixture})`, "restore it from git"],
      ];
      for (const [ok, what, hint] of checks) {
        const p = prereq("drive-smoke", ok, what, hint);
        if (p) return p;
      }
      const code = runExe("powershell.exe", [
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", join(repoRoot, "tasks", "drive-smoke.ps1"),
      ]);
      return code === 0 ? pass : fail(`drive smoke exit ${code}`);
    },
  },
];

// ---------------------------------------------------------------------------
// Single-instance lock: several native tests use fixed temp paths and the CDP
// port; two concurrent gates would corrupt each other. Stale locks (dead PID)
// are cleared automatically.
function acquireLock() {
  try {
    writeFileSync(lockPath, String(process.pid), { flag: "wx" });
    return true;
  } catch {
    let pid = NaN;
    try { pid = Number(readFileSync(lockPath, "utf8")); } catch { /* unreadable */ }
    if (Number.isFinite(pid) && pid > 0) {
      try {
        process.kill(pid, 0); // throws ESRCH if dead
        log(`another gate run is active (pid ${pid}, ${lockPath}) — refusing to run concurrently.`);
        return false;
      } catch (err) {
        // Only ESRCH proves the holder is dead. EPERM (etc.) means alive but
        // unsignalable from this context — that is NOT a stale lock.
        if (err.code !== "ESRCH") {
          log(`gate lock held by pid ${pid} (probe: ${err.code}) — refusing to run concurrently.`);
          return false;
        }
      }
    }
    log("clearing stale gate lock.");
    try { unlinkSync(lockPath); } catch { /* raced */ }
    try { writeFileSync(lockPath, String(process.pid), { flag: "wx" }); return true; } catch { return false; }
  }
}

function main() {
  if (ARGS.help) {
    console.log(usage());
    return 0;
  }
  if (ARGS.unknown.length > 0) {
    // Reject rather than ignore: an unrecognized flag with no --lane would
    // otherwise silently run the entire gate.
    log(`unknown argument(s): ${ARGS.unknown.join(", ")}`);
    console.log(usage());
    return 1;
  }
  const lanes = ONLY.length > 0 ? LANES.filter((l) => ONLY.includes(l.name)) : LANES;
  if (LIST) {
    for (const l of lanes) console.log(l.name);
    return 0;
  }
  const unknown = ONLY.filter((n) => !LANES.some((l) => l.name === n));
  if (unknown.length > 0) {
    log(`unknown lane(s): ${unknown.join(", ")} (use --list)`);
    return 1;
  }
  if (!acquireLock()) return 1;

  const results = [];
  try {
    for (const lane of lanes) {
      const blockedBy = (lane.deps || []).find(
        (d) => results.some((r) => r.name === d && r.status === "FAIL"),
      );
      const started = Date.now();
      let res;
      if (blockedBy) {
        res = skip(`blocked: ${blockedBy} failed`);
        log(`${lane.name}: SKIP (${res.note})`);
      } else {
        log(`=== ${lane.name} ===`);
        res = lane.run();
      }
      results.push({ name: lane.name, ...res, secs: (Date.now() - started) / 1000 });
    }
  } finally {
    try { unlinkSync(lockPath); } catch { /* already gone */ }
  }

  const width = Math.max(...results.map((r) => r.name.length));
  console.log("\n[gate] ================= summary =================");
  for (const r of results) {
    const note = r.note ? `  — ${r.note}` : "";
    console.log(`  ${r.name.padEnd(width)}  ${r.status.padEnd(4)}  ${r.secs.toFixed(1)}s${note}`);
  }
  if (SKIP_BUILD) {
    console.log("[gate] WARNING: --skip-build run — build lanes skipped; results may reflect STALE artifacts.");
  }
  const failed = results.filter((r) => r.status === "FAIL");
  const blocked = results.filter((r) => r.status === "SKIP" && (r.note || "").startsWith("blocked"));
  // "lanes" is explicit on purpose: these counts have ALWAYS been lane counts,
  // but a bare "0 skipped" next to a lane that internally reported "190 passed,
  // 4 skipped" reads as a claim about TESTS. The 2026-07 audit was misled by
  // exactly that, and separately showed a stubbed sub-runner reporting
  // "0 passed, 1 skipped" while the aggregate printed PASS / 0 skipped.
  console.log(
    `[gate] ${results.filter((r) => r.status === "PASS").length} lanes passed, ` +
      `${failed.length} failed, ${blocked.length} blocked, ` +
      `${results.filter((r) => r.status === "SKIP").length - blocked.length} skipped`,
  );
  const internallySkipped = results.filter((r) => r.innerSkipped > 0);
  if (internallySkipped.length > 0) {
    console.log(
      "[gate] NOTE: lanes reporting SKIPPED TESTS inside a passing lane — " +
        internallySkipped.map((r) => `${r.name}: ${r.innerSkipped}`).join(", "),
    );
  }
  return failed.length > 0 ? 1 : 0;
}

// Only run the gate when invoked as a script — importing the module (e.g. from
// run-all-tests.test.mjs to exercise parseArgs/usage) must not start a gate.
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exit(main());
}
