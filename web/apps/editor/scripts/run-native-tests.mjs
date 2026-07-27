// Native test harness: orchestrates the native bridge Playwright run.
//
// 1. Kill any stale ParticleEditor.exe (best-effort).
// 2. Launch x64\Debug\ParticleEditor.exe --test-host.
// 3. Poll http://localhost:9222/json/version until CDP is ready
//    (≤ 30 s; WebView2 init plus DPI/COM startup can take 5–10 s).
// 4. Spawn Playwright against tests/bridge-native.spec.ts.
// 5. Tear down the host and exit with Playwright's exit code.
//
// Cleanup runs on success, failure, AND uncaught throws — the binary
// is single-instance so leaving it around blocks the next run.

import { spawn, spawnSync } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const editorDir = resolve(__dirname, "..");
const repoRoot = resolve(__dirname, "../../../..");
const exe = join(repoRoot, "x64", "Debug", "ParticleEditor.exe");

// True when this machine has a resolved game data path, i.e. the specs that
// need real textures CAN run here. Mirrors the same registry probe the gate's
// drive-smoke prereq uses.
function gameDataPathPresent() {
  const r = spawnSync("reg.exe",
    ["query", String.raw`HKCU\Software\AloParticleEditor`, "/v", "GameDataPath"],
    { encoding: "utf8", shell: false });
  return r.status === 0;
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
    // (ParticleEditor.exe --test-host). A blanket
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
  // `--update` flag: forward to the Playwright run as
  // UPDATE_A11Y_GOLDENS=1 so the a11y matcher writes goldens instead
  // of comparing. Set here (rather than expecting the caller to
  // prefix the env var) so `pnpm a11y:update` works on Windows
  // without cross-env. The flag affects only the toMatchJSONGolden
  // matcher — other native specs ignore the env var.
  if (process.argv.includes("--update")) {
    process.env.UPDATE_A11Y_GOLDENS = "1";
    console.log("[run-native-tests] --update flag → UPDATE_A11Y_GOLDENS=1");
  }

  // Forward unknown CLI args through to
  // Playwright so scoped runs like `pnpm a11y:update --grep "dialog-about"`
  // actually filter the suite. Previously these args were silently
  // dropped (the Playwright spawn below had a hard-coded arg list),
  // which made every "scoped" refresh regenerate ALL goldens —
  // the exact footgun the design review warned about. The only
  // recognised flag (--update) is consumed above; anything else gets
  // forwarded as-is.
  const RECOGNISED_FLAGS = new Set(["--update"]);
  const forwardedArgs = process.argv.slice(2).filter((a) => !RECOGNISED_FLAGS.has(a));

  await killAny();
  // Give Windows a moment to release file locks.
  await sleep(300);

  console.log(`[run-native-tests] Launching ${exe} --test-host ...`);
  // Stdio hardening — DON'T inherit stdio. The
  // previous `stdio: "inherit"` caused a real footgun: ParticleEditor.exe
  // is a SUBSYSTEM:Windows app, but node attaches an inherited console
  // for its piped stdio. The host writes [ArchC]/[host]/[COMP-*]
  // diagnostics to stderr every frame; if the user clicks in that
  // inherited console window, Windows enters QuickEdit (Mark) mode
  // which BLOCKS the stderr buffer. The next per-frame fprintf hangs
  // and freezes the entire host thread — Playwright then times out,
  // ALL in-flight specs cascade-fail. Surfaced during smoke testing.
  //
  // Fix: discard child stdio. All host diagnostics are duplicated to
  // %LOCALAPPDATA%\AloParticleEditor\host.log via the Log() macro, so
  // test diagnostics don't lose anything.
  //
  // windowsHide:true removed. Win32 UIA does not expose
  // WebView2's accessibility tree when the host window is hidden
  // (SW_HIDE) — UIA can't traverse into the Chrome_WidgetWin_1 →
  // BrowserRootView → React DOM subtree. The window must be visible
  // (SW_SHOW) for the a11y specs to capture meaningful trees.
  // The QuickEdit risk only applies to `stdio:"inherit"`;
  // with stdio:"ignore" there is no inherited console window for the
  // user to click into, so windowsHide is not needed for safety.
  //
  // If host stderr is genuinely needed for debugging, use
  // ["ignore", "pipe", "pipe"] + pipe child.stderr to a log file
  // (NOT process.stderr, which has the same QuickEdit risk if an
  // inherited console is present).
  const child = spawn(exe, ["--test-host"], {
    cwd: repoRoot,
    stdio: ["ignore", "ignore", "ignore"],
    detached: false,
  });

  let childExited = false;
  // Mid-run host-death guard. If the host dies WHILE
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
          "  WebView2 user-data folder.\n",
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
      // PreferencesDialog host round-trips (Edit → Preferences…): MSAA
      // level via engine/query/msaa-levels `current`, plus toggle
      // persistence across reopen. Restores every setting it mutates.
      "tests/preferences.spec.ts",
      // AtlasPickerPanel click-to-assign: index-channel key selection
      // auto-opens the docked picker; cell clicks write frames via
      // emitters/set-track-key (incl. the differing-frames confirm
      // modal). Runtime-skips when the seed texture can't be decoded
      // (textures/get-preview != ok — game data unreachable).
      "tests/atlas-picker.spec.ts",
      "tests/mods-contract.spec.ts",
      "tests/mod-stack.spec.ts",
      "tests/leave-particles.spec.ts",
      "tests/autosave-recovery.spec.ts",
      "tests/splitters.spec.ts",
      "tests/d3d9ex.spec.ts",
      "tests/alpha-compositor-snapshot.spec.ts",
      // DOM-event → viewport/input bridge wiring
      // under this architecture (canvas-in-DOM viewport). Skips with a
      // clear annotation when ALO_HOSTING_MODE == "legacy",
      // so runs WITHOUT the env var are a no-op. Included in the
      // harness so the moment canvas-jpeg becomes the default the
      // bridge surface is gated automatically.
      "tests/canvas-architecture.spec.ts",
      // Composition-hosting A/B parity
      // gate. Tests skip with a clear annotation when
      // ALO_HOSTING_MODE == "legacy" (composition mode inactive), so running the
      // harness WITHOUT the env var (HWND-mode baseline) is a no-op
      // for this file. Running WITH the env-var pair gates the
      // composition path's bridge layer.
      "tests/composition-hosting.spec.ts",
      // DXGI transport / resize-stress /
      // perf gates. All three specs skip when ALO_WEBVIEW2_HOSTING
      // != "composition". Composition mode requires BOTH env vars
      // (canvas-jpeg + composition) plus a dist/ built with VITE_*
      // counterparts to be a meaningful gate. (The dxgi-vs-
      // jpeg SSIM check was deferred from this list — Playwright's DOM-only
      // screenshots can't see DXGI engine pixels under composition;
      // manual visual smoke is the irreducible gate.)
      "tests/dxgi-transport.spec.ts",
      "tests/dxgi-resize-stress.spec.ts",
      "tests/dxgi-perf.spec.ts",
      // Scene-rect transform gate. Skips
      // when ALO_HOSTING_MODE == "legacy" (composition mode inactive) (LayoutBroker's
      // new wiring is composition-mode-only).
      // Asserts [COMP-engine-transform] log lines fire on
      // layout/scene-rect dispatch with the expected absolute clip.
      "tests/dxgi-scene-rect.spec.ts",
      // Composition-mode a11y DOM-snapshot specs.
      // (The HWND/UIA `[hwnd]` quartet + their `.golden.json` goldens
      // were removed along with the legacy `--legacy` lane.)
      // Mirror the HWND quartet but capture via
      // page.accessibility.snapshot() (CDP) instead of Win32 UIA.
      // Auto-skip under default HWND mode (the UIA lane covers that);
      // active only when ALO_HOSTING_MODE != legacy (default). Reuse the
      // surface-driver arrays from the golden lanes unchanged.
      "tests/a11y-chrome-composition.spec.ts",
      "tests/a11y-dialogs-composition.spec.ts",
      "tests/a11y-keyboard-composition.spec.ts",
      "tests/a11y-curve-spinner-composition.spec.ts",
      // Composition-mode UIA backbone
      // reachability spec. Asserts the composition-hosted tree
      // exposes AloHostMain → Chromium chrome → EmbeddedBrowserFrame
      // → React menubar all the way down via Win32 UIA. Catches the
      // case where Blink's lazy a11y regresses (would leave
      // composition users with no screen-reader access to React).
      // Auto-skips under default HWND mode.
      "tests/a11y-uia-composition-reachable.spec.ts",
      // Preview overload guard regression. Bombs the
      // live preview with rate=1e9 and asserts the engine plateaus at
      // the particle budget + latches `overload` on stats/tick instead
      // of OOM-crashing. Runs LAST: it floods the live engine and
      // briefly mutates the boot doc's first emitter; its finally-block
      // restores properties + clears instances, but keeping it after
      // the deterministic a11y goldens removes any timing coupling.
      "tests/preview-overload.spec.ts",
      // Forward unknown args (e.g. --grep
      // "dialog-about") so scoped a11y refresh actually scopes. See
      // RECOGNISED_FLAGS above.
      ...forwardedArgs,
    ], {
      cwd: editorDir,
      stdio: "inherit",
      shell: false,
      // Demand real game textures WHEN THIS MACHINE HAS THEM.
      //
      // Several native specs (atlas assignment, seed-dependent reparent /
      // paste-as-child) call test.skip() when game data can't be reached, and
      // nothing ever set PE_REQUIRE_GAME_TEXTURES — so on a box with the game
      // installed they still silently self-skipped, and breaking atlas
      // assignment or ParticleSystem::reparentEmitter left the lane green
      // (2026-07 audit, an-audit-finding).
      //
      // Gated on the registry game path rather than forced: a machine with no
      // install genuinely cannot run them, and failing there would punish a
      // legitimate environment. Where the data IS present, a skip now means a
      // real problem and fails loudly.
      env: gameDataPathPresent()
        ? { ...process.env, PE_REQUIRE_GAME_TEXTURES: "1" }
        : process.env,
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
  // spec failures (exit 1) and a clean pass (exit 0).
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
