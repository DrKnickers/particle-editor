// Task 2.2 test harness: orchestrates the native bridge Playwright run.
//
// 1. Kill any stale ParticleEditor.exe (best-effort).
// 2. Launch x64\Debug\ParticleEditor.exe --new-ui --test-host.
// 3. Poll http://localhost:9222/json/version until CDP is ready
//    (≤ 30 s; WebView2 init plus DPI/COM startup can take 5–10 s).
// 4. Spawn Playwright against tests/bridge-native.spec.ts.
// 5. Tear down the host and exit with Playwright's exit code.
//
// Cleanup runs on success, failure, AND uncaught throws — the binary
// is single-instance so leaving it around blocks the next run.

import { spawn } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { readFileSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const editorDir = resolve(__dirname, "..");
const repoRoot = resolve(__dirname, "../../../..");
const exe = join(repoRoot, "x64", "Debug", "ParticleEditor.exe");
const buildMetaPath = join(editorDir, "dist", "build-meta.json");

// [handoff item 4] dist/ build-mode gate.
//
// The editor has two hosting modes that MUST agree across a runtime
// knob and a build knob:
//   - ALO_HOSTING_MODE  (runtime; set below from --legacy)
//   - VITE_HOSTING_MODE  (build-time; baked into dist/ by `pnpm build`)
// This harness owns the runtime knob but historically trusted that
// whoever built dist/ matched the build knob. When they disagreed the
// editor rendered broken yet every spec still executed, producing a
// meaningless pass/fail number — the silent failure this gate kills.
//
// vite.config.ts's buildMetaPlugin stamps dist/build-meta.json with the
// baked hostingMode; we read it here and refuse to launch on mismatch
// (or missing/unmarked dist/) unless --rebuild was passed.

// Read the baked hosting mode, or null if dist/ is missing / unmarked
// (a pre-gate build, or a Rollup failure that left no marker).
function readDistMode() {
  try {
    return JSON.parse(readFileSync(buildMetaPath, "utf8"));
  } catch {
    return null;
  }
}

// PowerShell remediation command for a given lane — quoted in the
// fail-fast message AND mirrored by the --rebuild spawn below.
function buildCmdFor(mode) {
  const build = "pnpm --filter @particle-editor/editor build";
  return mode === "legacy"
    ? `$env:VITE_HOSTING_MODE="legacy"; ${build}; Remove-Item Env:VITE_HOSTING_MODE`
    : build;
}

// Run the editor's two-step build (`tsc -b && vite build`) shell-free,
// matching the Playwright-CLI invocation pattern below (pnpm is a .CMD
// shim that shell-free spawn refuses; the local node bins don't have
// that problem). Returns the child's exit code.
function runBuildStep(jsBin, args, env) {
  return new Promise((resolveStep) => {
    const p = spawn(process.execPath, [jsBin, ...args], {
      cwd: editorDir,
      stdio: "inherit",
      shell: false,
      env,
    });
    p.on("exit", (code) => resolveStep(code ?? 1));
    p.on("error", () => resolveStep(1));
  });
}

async function rebuildDist(requestedMode) {
  // Mirror the documented manual flow: set VITE_HOSTING_MODE for the
  // legacy lane, delete it for composition (cf. handoff "How to run
  // modes locally"). The build inherits this env.
  const env = { ...process.env };
  if (requestedMode === "legacy") env.VITE_HOSTING_MODE = "legacy";
  else delete env.VITE_HOSTING_MODE;

  const tsc = join(editorDir, "node_modules", "typescript", "bin", "tsc");
  const vite = join(editorDir, "node_modules", "vite", "bin", "vite.js");
  console.log(`[run-native-tests] --rebuild → building ${requestedMode} dist/ ...`);
  const tscCode = await runBuildStep(tsc, ["-b"], env);
  if (tscCode !== 0) return tscCode;
  return runBuildStep(vite, ["build"], env);
}

// Pre-flight: ensure dist/ was built for the requested lane, else
// fail-fast (or rebuild). Runs BEFORE the host launch so a wrong-mode
// run never burns time. Calls process.exit(1) on an unrecoverable
// mismatch.
async function ensureDistMode(requestedMode, allowRebuild) {
  let meta = readDistMode();
  if (meta && meta.hostingMode === requestedMode) {
    console.log(
      `[run-native-tests] dist/ build mode OK: ${requestedMode} ` +
        `(commit ${meta.commit ?? "?"}, built ${meta.builtAt ?? "?"})`,
    );
    return;
  }

  const found = meta
    ? `${meta.hostingMode} (commit ${meta.commit ?? "?"}, built ${meta.builtAt ?? "?"})`
    : "<no dist/build-meta.json — dist/ is missing or was built before this gate>";

  if (allowRebuild) {
    console.log(
      `[run-native-tests] dist/ mode mismatch (requested ${requestedMode}, ` +
        `found ${found}) — rebuilding (--rebuild).`,
    );
    const code = await rebuildDist(requestedMode);
    if (code !== 0) {
      console.error(`[run-native-tests] rebuild failed (exit ${code}).`);
      process.exit(1);
    }
    // Don't trust exit 0 — re-read the marker and confirm it flipped
    // (a build can silently no-op; cf. lessons).
    meta = readDistMode();
    if (!meta || meta.hostingMode !== requestedMode) {
      console.error(
        `[run-native-tests] rebuild did not produce a ${requestedMode} dist/ ` +
          `(marker now: ${meta ? meta.hostingMode : "<missing>"}). Aborting.`,
      );
      process.exit(1);
    }
    console.log(`[run-native-tests] rebuild OK: dist/ is now ${requestedMode}.`);
    return;
  }

  console.error(
    `\n[run-native-tests] dist/ build-mode mismatch — refusing to run.\n` +
      `  Requested lane : ${requestedMode}\n` +
      `  dist/ was built: ${found}\n\n` +
      `  Running the suite now would test the wrong hosting mode (broken\n` +
      `  viewport, meaningless pass/fail). Rebuild dist/ for this lane:\n\n` +
      `    ${buildCmdFor(requestedMode)}\n\n` +
      `  ...or re-run this command with --rebuild to do it automatically.\n`,
  );
  process.exit(1);
}

async function probeCdp() {
  try {
    const res = await fetch("http://localhost:9222/json/version");
    return res.ok;
  } catch {
    return false;
  }
}

function killAny() {
  return new Promise((resolve) => {
    // Scope the cleanup to ONLY the test-host instances this harness spawns
    // (ParticleEditor.exe --new-ui --test-host). A blanket
    // `taskkill /F /IM ParticleEditor.exe` matches by image name, so it would
    // also kill a legacy editor build the user is daily-driving in parallel —
    // same exe name, different binary. Filter on the command line instead:
    // the legacy build is never launched with --test-host, so it survives.
    // Fails safe — if CommandLine is unreadable the -like is false and the
    // process is left alone (worst case: a stale test-host, never the user's
    // editor). PowerShell process management mirrors tests/helpers/uia.ts.
    const cmd =
      "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
      "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
    const p = spawn("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", cmd], {
      stdio: "ignore",
      shell: false,
    });
    p.on("exit", () => resolve());
    p.on("error", () => resolve()); // powershell missing → nothing to clean
  });
}

async function main() {
  // T12] `--update` flag: forward to the Playwright run as
  // UPDATE_A11Y_GOLDENS=1 so the a11y matcher writes goldens instead
  // of comparing. Set here (rather than expecting the caller to
  // prefix the env var) so `pnpm a11y:update` works on Windows
  // without cross-env. The flag affects only the toMatchJSONGolden
  // matcher — other native specs ignore the env var.
  if (process.argv.includes("--update")) {
    process.env.UPDATE_A11Y_GOLDENS = "1";
    console.log("[run-native-tests] --update flag → UPDATE_A11Y_GOLDENS=1");
  }

  // [handoff item 16 follow-up] Forward unknown CLI args through to
  // Playwright so scoped runs like `pnpm a11y:update --grep "dialog-about"`
  // actually filter the suite. Previously these args were silently
  // dropped (the Playwright spawn below had a hard-coded arg list),
  // which made every "scoped" refresh regenerate ALL goldens —
  // the exact footgun handoff item 16 R7 warned about. Recognised
  // flags (--update, --legacy) are consumed above; anything else
  // gets forwarded as-is.
  const RECOGNISED_FLAGS = new Set(["--update", "--legacy", "--rebuild"]);
  const forwardedArgs = process.argv.slice(2).filter((a) => !RECOGNISED_FLAGS.has(a));

  // `--legacy` flag: run the host + Playwright tests in
  // architecture A (legacy AlphaCompositor popup + HWND-hosted
  // WebView2) instead of the new default (architecture C / composition).
  // Caller is responsible for having a matching dist/ baked with
  // `VITE_HOSTING_MODE=legacy`; the boot-time consistency log in
  // App.tsx + the host's [host] hosting mode line surface mismatches
  // immediately. Used by `pnpm test:native:legacy` for the legacy
  // regression lane (132/0/56 baseline pre-; same lane still
  // exists, just opt-in now).
  const requestedMode = process.argv.includes("--legacy") ? "legacy" : "composition";
  if (requestedMode === "legacy") {
    process.env.ALO_HOSTING_MODE = "legacy";
    console.log("[run-native-tests] --legacy flag → ALO_HOSTING_MODE=legacy");
  }

  // [handoff item 4] Verify dist/ was built for this lane before
  // launching the host. Fail-fast on mismatch unless --rebuild.
  await ensureDistMode(requestedMode, process.argv.includes("--rebuild"));

  await killAny();
  // Give Windows a moment to release file locks.
  await sleep(300);

  console.log(`[run-native-tests] Launching ${exe} --new-ui --test-host ...`);
  // Phase 3 Stage 4f hardening — DON'T inherit stdio. The
  // previous `stdio: "inherit"` caused a real footgun: ParticleEditor.exe
  // is a SUBSYSTEM:Windows app, but node attaches an inherited console
  // for its piped stdio. The host writes [ArchC]/[host]/[COMP-*]
  // diagnostics to stderr every frame; if the user clicks in that
  // inherited console window, Windows enters QuickEdit (Mark) mode
  // which BLOCKS the stderr buffer. The next per-frame fprintf hangs
  // and freezes the entire host thread — Playwright then times out,
  // ALL in-flight specs cascade-fail. Surfaced during Stage 4f smoke.
  //
  // Fix: discard child stdio. All host diagnostics are duplicated to
  // %LOCALAPPDATA%\AloParticleEditor\host.log via the Log() macro, so
  // test diagnostics don't lose anything.
  //
  // T9.3] windowsHide:true removed. Win32 UIA does not expose
  // WebView2's accessibility tree when the host window is hidden
  // (SW_HIDE) — UIA can't traverse into the Chrome_WidgetWin_1 →
  // BrowserRootView → React DOM subtree. The window must be visible
  // (SW_SHOW) for the a11y specs (T9) to capture meaningful trees.
  // The Stage 4f QuickEdit risk only applies to `stdio:"inherit"`;
  // with stdio:"ignore" there is no inherited console window for the
  // user to click into, so windowsHide is not needed for safety.
  //
  // If host stderr is genuinely needed for debugging, use
  // ["ignore", "pipe", "pipe"] + pipe child.stderr to a log file
  // (NOT process.stderr, which has the same QuickEdit risk if an
  // inherited console is present).
  const child = spawn(exe, ["--new-ui", "--test-host"], {
    cwd: repoRoot,
    stdio: ["ignore", "ignore", "ignore"],
    detached: false,
  });

  let childExited = false;
  // Mid-run host-death guard (see lessons.md). If the host dies WHILE
  // Playwright is running, every remaining spec fails with
  // `connect ECONNREFUSED ::1:9222` (the CDP endpoint died with the host).
  // That ~60-failure cascade is indistinguishable from a real regression
  // unless we detect it and shout: a single mid-run death once turned a real
  // 160/5 into 39-failed and looked like a catastrophe. `pwRunning` gates the
  // detection so the EXPECTED end-of-run teardown kill (after Playwright has
  // already exited) and the CDP-timeout kill (before it starts) don't trip it.
  let pwRunning = false;
  let pwChild = null;
  let hostDiedMidRun = false;
  child.on("exit", (code, signal) => {
    childExited = true;
    console.log(`[run-native-tests] host process exited (code=${code}, signal=${signal})`);
    if (pwRunning) {
      hostDiedMidRun = true;
      console.error(
        "\n[run-native-tests] *** FATAL: host process died MID-RUN " +
          `(code=${code}, signal=${signal}). ***\n` +
          "  Remaining specs are INVALID — their CDP endpoint died with the\n" +
          "  host. This is NOT a test failure / regression. Re-run the suite;\n" +
          "  if it recurs, check for a stale `--test-host` process or a locked\n" +
          "  WebView2 user-data folder (lessons.md /).\n",
      );
      // Stop Playwright NOW so the run halts at the death instead of burning
      // through every remaining spec against the dead CDP endpoint.
      try {
        pwChild?.kill();
      } catch {
        /* already gone */
      }
    }
  });

  let cdpUp = false;
  const deadline = Date.now() + 30_000;
  while (Date.now() < deadline) {
    if (childExited) {
      throw new Error("Host process exited before CDP came up");
    }
    if (await probeCdp()) {
      cdpUp = true;
      break;
    }
    await sleep(500);
  }
  if (!cdpUp) {
    console.error("[run-native-tests] CDP did not come up at http://localhost:9222 within 30s");
    try {
      child.kill();
    } catch {
      /* ignore */
    }
    await killAny();
    process.exit(1);
  }
  console.log("[run-native-tests] CDP ready, running Playwright spec ...");

  // On Windows the playwright .bin entry is a .CMD shim; node's spawn
  // refuses to launch .CMD without shell:true, so use the JS cli entry
  // directly via node to keep the spawn cross-platform and shell-free.
  const playwrightCli = join(editorDir, "node_modules", "@playwright", "test",
    "cli.js");
  const pwExit = await new Promise((resolve) => {
    pwRunning = true;
    const pw = spawn(process.execPath, [
      playwrightCli, "test",
      "tests/bridge-native.spec.ts",
      "tests/background-picker.spec.ts",
      "tests/app-shell.spec.ts",
      "tests/toolbar.spec.ts",
      "tests/menu-bar.spec.ts",
      "tests/primitives.spec.ts",
      "tests/dialogs.spec.ts",
      "tests/tools.spec.ts",
      "tests/file-ops.spec.ts",
      "tests/spawner-import-mod.spec.ts",
      "tests/host-state-plumbing.spec.ts",
      "tests/render-loop.spec.ts",
      "tests/viewport-camera.spec.ts",
      "tests/viewport-resize.spec.ts",
      "tests/emitter-tree.spec.ts",
      "tests/emitter-mutations.spec.ts",
      "tests/emitter-multi-mutations.spec.ts",
      "tests/undo-navigation.spec.ts",
      "tests/emitter-import.spec.ts",
      "tests/emitter-drag.spec.ts",
      "tests/emitter-keyboard.spec.ts",
      "tests/track-editor.spec.ts",
      "tests/property-tabs.spec.ts",
      "tests/mods-contract.spec.ts",
      "tests/leave-particles.spec.ts",
      "tests/autosave-recovery.spec.ts",
      "tests/splitters.spec.ts",
      "tests/d3d9ex.spec.ts",
      "tests/alpha-compositor-snapshot.spec.ts",
      // Phase 2 — DOM-event → viewport/input bridge wiring
      // under architecture-C (canvas-in-DOM viewport). Skips with a
      // clear annotation when ALO_HOSTING_MODE == "legacy",
      // so runs WITHOUT the env var are a no-op. Included in the
      // harness so the moment canvas-jpeg is enabled (Phase 4 default
      // flip) the bridge surface is gated automatically.
      "tests/canvas-architecture.spec.ts",
      // Phase 3 Stage 3g — composition-hosting A/B parity
      // gate. Tests skip with a clear annotation when
      // ALO_HOSTING_MODE == "legacy" (composition mode inactive), so running the
      // harness WITHOUT the env var (HWND-mode baseline) is a no-op
      // for this file. Running WITH the env-var pair gates the
      // composition path's bridge layer.
      "tests/composition-hosting.spec.ts",
      // Phase 3 Stage 4f — DXGI transport / resize-stress /
      // perf gates. All three specs skip when ALO_WEBVIEW2_HOSTING
      // != "composition". Composition mode requires BOTH env vars
      // (canvas-jpeg + composition) plus a dist/ built with VITE_*
      // counterparts to be a meaningful gate. (Note 4f #2 dxgi-vs-
      // jpeg SSIM was deferred from this list — Playwright's DOM-only
      // screenshots can't see DXGI engine pixels under composition;
      // manual visual smoke is the irreducible gate. See sub-plan §6.)
      "tests/dxgi-transport.spec.ts",
      "tests/dxgi-resize-stress.spec.ts",
      "tests/dxgi-perf.spec.ts",
      // Phase 3 Stage 5 T7 — scene-rect transform gate. Skips
      // when ALO_HOSTING_MODE == "legacy" (composition mode inactive) (LayoutBroker's
      // new wiring is composition-mode-only per R9 mitigation c).
      // Asserts [COMP-engine-transform] log lines fire on
      // layout/scene-rect dispatch with the expected absolute clip.
      "tests/dxgi-scene-rect.spec.ts",
      // Phase 3 a11y T9.1 — HWND Win32 UIA snapshot specs.
      // Each parametrizes over its surface-driver array (T5–T8) and
      // golden-compares the normalized UIA tree. Auto-skip under
      // ALO_HOSTING_MODE != legacy (default) (T10 covers that lane).
      // Goldens are generated separately (UPDATE_A11Y_GOLDENS=1 run
      // in T9.3); without goldens these specs fail — run via
      // pnpm test:native only after T9.3 has landed the golden files.
      "tests/a11y-chrome.spec.ts",
      "tests/a11y-dialogs.spec.ts",
      "tests/a11y-keyboard.spec.ts",
      "tests/a11y-curve-spinner.spec.ts",
      // Phase 3 a11y T10 — composition-mode DOM-snapshot specs.
      // Mirror the T9 HWND quartet but capture via
      // page.accessibility.snapshot() (CDP) instead of Win32 UIA.
      // Auto-skip under default HWND mode (T9 covers that lane);
      // active only when ALO_HOSTING_MODE != legacy (default). Reuse the
      // surface-driver arrays from T5-T8 unchanged.
      "tests/a11y-chrome-composition.spec.ts",
      "tests/a11y-dialogs-composition.spec.ts",
      "tests/a11y-keyboard-composition.spec.ts",
      "tests/a11y-curve-spinner-composition.spec.ts",
      // Phase 3 a11y T11 — composition-mode UIA backbone
      // reachability spec. Asserts the composition-hosted tree
      // exposes AloHostMain → Chromium chrome → EmbeddedBrowserFrame
      // → React menubar all the way down via Win32 UIA. Catches the
      // case where Blink's lazy a11y regresses (would leave
      // composition users with no screen-reader access to React).
      // Auto-skips under default HWND mode.
      "tests/a11y-uia-composition-reachable.spec.ts",
      // [handoff item 16 follow-up] Forward unknown args (e.g. --grep
      // "dialog-about") so scoped a11y refresh actually scopes. See
      // RECOGNISED_FLAGS above.
      ...forwardedArgs,
    ], {
      cwd: editorDir,
      stdio: "inherit",
      shell: false,
    });
    pwChild = pw;
    pw.on("exit", (code) => {
      pwRunning = false;
      resolve(code ?? 1);
    });
    pw.on("error", (err) => {
      pwRunning = false;
      console.error("[run-native-tests] failed to spawn playwright:", err);
      resolve(1);
    });
  });

  try {
    child.kill();
  } catch {
    /* ignore */
  }
  await sleep(500);
  await killAny();

  // A mid-run host death (detected in child.on("exit") above) makes the
  // Playwright exit code meaningless — exit 2 to distinguish it from ordinary
  // spec failures (exit 1) and a clean pass (exit 0). See lessons.md.
  if (hostDiedMidRun) {
    console.error(
      "[run-native-tests] run ABORTED: host died mid-run (see FATAL above). " +
        "Exiting 2 — NOT a regression; re-run before trusting this result.",
    );
    process.exit(2);
  }

  process.exit(pwExit);
}

main().catch(async (err) => {
  console.error("[run-native-tests]", err);
  await killAny();
  process.exit(1);
});
