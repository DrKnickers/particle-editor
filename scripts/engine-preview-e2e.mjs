// Exercises managed engine setup and a live frame through the Release WebView2 bridge.
// Snapshots user settings and source files; never writes into the game or source map editor.
import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, createReadStream, existsSync, mkdirSync, mkdtempSync, readdirSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { createServer } from "node:net";
import { basename, dirname, join, resolve } from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { fileURLToPath, pathToFileURL } from "node:url";

/** @type {string} */
const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const SETTINGS_KEY = String.raw`HKCU\Software\AloParticleEditor`;
const CDP_PORT = 9222;
const CDP_URL = `http://127.0.0.1:${CDP_PORT}`;
const POLL_INTERVAL_MS = 1_000;
const START_TIMEOUT_MS = 60_000;
const CDP_PROBE_TIMEOUT_MS = 1_000;
const REQUEST_TIMEOUT_MS = 30_000;
const SETUP_TIMEOUT_MS = 900_000;
const LIVE_TIMEOUT_MS = 90_000;
const STOP_TIMEOUT_MS = 30_000;
const HOST_LOG_NAME = "alamo_preview.log";
const SNAPSHOT_INDENT_SPACES = 2;
// The native setup worker emits this after setup work and settings persistence finish.
const SETUP_FINISHED_EVENT = "engineHost/setup-finished";
const LIVE_STATE_CHANGED_EVENT = "engineLive/state-changed";
/** @typedef {{what: string, hint: string}} MissingPrerequisite */
/** @typedef {{ok: boolean, missing: MissingPrerequisite[]}} PrerequisiteResult */
/** @typedef {{platform: string, gameDataPath: string, editorExePath: string, releaseExe: string}} Configuration */
/** @typedef {Record<string, string>} FileHashes */
/** @typedef {import('playwright-core').Page} Page */
/** @typedef {import('../web/packages/bridge-schema/src/index.ts').EngineHostSetupStatusDto} SetupStatus */
/** @typedef {import('../web/packages/bridge-schema/src/index.ts').EngineHostSetupAction} SetupAction */
/** @typedef {import('../web/packages/bridge-schema/src/index.ts').EngineHostSetupFinishedDto} SetupFinished */
/** @typedef {{resolve: (reply: unknown) => void, reject: (error: Error) => void, timer: number}} PendingReply */
/** @typedef {{pending: Map<string, PendingReply>, setupFinished: SetupFinished | null}} BridgeState */

/** @param {string} name @returns {string} */
function readRegistryString(name) {
  /** @type {import('node:child_process').SpawnSyncReturns<string>} */
  const result = spawnSync("reg.exe", ["query", SETTINGS_KEY, "/v", name], { encoding: "utf8", windowsHide: true });
  return result.stdout?.match(/REG_SZ\s+([^\r\n]*)/)?.[1]?.trim() ?? "";
}

/** @returns {Configuration} */
function readConfiguration() {
  return {
    platform: process.platform,
    gameDataPath: readRegistryString("GameDataPath"),
    editorExePath: process.env.PE_ENGINE_EDITOR_EXE || readRegistryString("EngineHostExePath"),
    releaseExe: join(REPO_ROOT, "x64", "Release", "ParticleEditor.exe"),
  };
}

/** @returns {boolean} */
function hasGuiDesktop() {
  /** @type {import('node:child_process').SpawnSyncReturns<string>} */
  const result = spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command", `
    Add-Type 'using System; using System.Runtime.InteropServices; public class PreviewDesktop {
      [DllImport("user32.dll")] public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
      [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr desktop);
    }';
    $sessionId = (Get-Process -Id $PID).SessionId;
    $locked = Get-Process LogonUI -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq $sessionId };
    $desktopReadObjects = 1;
    $desktop = [PreviewDesktop]::OpenInputDesktop(0, $false, $desktopReadObjects);
    $usable = [Environment]::UserInteractive -and $sessionId -gt 0 -and !$locked -and $desktop -ne [IntPtr]::Zero;
    if ($desktop -ne [IntPtr]::Zero) { [void][PreviewDesktop]::CloseDesktop($desktop) };
    if ($usable) { 'OK' }
  `], { encoding: "utf8", windowsHide: true, timeout: REQUEST_TIMEOUT_MS });
  return result.status === 0 && result.stdout.trim() === "OK";
}

/**
 * @param {Configuration} configuration
 * @param {{exists?: (path: string) => boolean, desktop?: () => boolean}} probes
 * @returns {PrerequisiteResult}
 */
export function detectPrerequisites(configuration, { exists = existsSync, desktop = hasGuiDesktop } = {}) {
  /** @type {MissingPrerequisite[]} */
  const missing = [];
  /** @type {string} */
  const configuredRoot = resolve(configuration.gameDataPath);
  /** @type {string} */
  const corruptionRoot = basename(configuredRoot).toLowerCase() === "corruption"
    ? configuredRoot : join(dirname(configuredRoot), "corruption");
  /** @type {boolean} */
  const isGameRoot = ["gamedata", "corruption"].includes(basename(configuredRoot).toLowerCase());
  /** @type {boolean} */
  const hasGameInstall = isGameRoot && exists(configuration.gameDataPath) && exists(join(corruptionRoot, "Data", "config.meg"));
  /** @type {[boolean, string, string][]} */
  const checks = [
    [configuration.platform === "win32" && desktop(), "Windows GUI desktop unavailable", "run on an unlocked interactive Windows desktop"],
    [hasGameInstall,
      "configured game install missing", "choose the installed game data folder in Particle Editor (HKCU GameDataPath)"],
    [Boolean(configuration.editorExePath) && exists(configuration.editorExePath), "EAW Terrain Editor executable missing", "set PE_ENGINE_EDITOR_EXE to the supported source EAW Terrain Editor.exe"],
    [exists(configuration.releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first"],
    [exists(join(dirname(configuration.releaseExe), "engine-host", "package.json")), "bundled engine-host package missing", "stage the verified host package beside the Release executable"],
  ];
  for (const [ok, what, hint] of checks) {
    if (!ok) missing.push({ what, hint });
  }
  return { ok: missing.length === 0, missing };
}

/** @returns {PrerequisiteResult} */
export function probePrerequisites() {
  return detectPrerequisites(readConfiguration());
}

/** @param {FileHashes} before @param {FileHashes} after @returns {{ok: boolean, changed: string[]}} */
export function compareFileHashes(before, after) {
  /** @type {string[]} */
  const paths = [...new Set([...Object.keys(before), ...Object.keys(after)])].sort();
  /** @type {string[]} */
  const changed = paths.filter((path) => before[path] !== after[path]);
  return { ok: changed.length === 0, changed };
}

/** @param {string[]} roots @returns {Promise<FileHashes>} */
async function snapshotFileHashes(roots) {
  /** @type {FileHashes} */
  const hashes = {};
  /** @type {string[]} */
  const pending = [...new Set(roots.map((root) => resolve(root)))];
  while (pending.length > 0) {
    /** @type {string} */
    const folder = pending.pop();
    for (const entry of readdirSync(folder, { withFileTypes: true })) {
      /** @type {string} */
      const path = join(folder, entry.name);
      if (entry.isSymbolicLink()) throw new Error(`Cannot snapshot a source reparse point: ${path}`);
      if (entry.isDirectory()) { pending.push(path); continue; }
      /** @type {import('node:crypto').Hash} */
      const hash = createHash("sha256");
      for await (const bytes of createReadStream(path)) hash.update(bytes);
      hashes[path] = hash.digest("hex");
    }
  }
  return hashes;
}

/** @param {string[]} args @returns {void} */
function runRegistryCommand(args) {
  /** @type {import('node:child_process').SpawnSyncReturns<string>} */
  const result = spawnSync("reg.exe", args, { encoding: "utf8", windowsHide: true });
  assert.equal(result.status, 0, `Registry ${args[0]} failed; preserve the registry snapshot for recovery`);
}

/** @returns {Promise<void>} */
async function assertExclusiveRun() {
  /** @type {import('node:child_process').SpawnSyncReturns<string>} */
  const result = spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command",
    "if (Get-Process ParticleEditor -ErrorAction SilentlyContinue) { exit 1 }"], { windowsHide: true, encoding: "utf8" });
  assert.equal(result.status, 0, "Close existing Particle Editor processes before running this lane");
  // Refuse an occupied endpoint instead of attaching to a different editor's WebView2.
  await new Promise((resolvePort, rejectPort) => {
    /** @type {import('node:net').Server} */
    const server = createServer();
    server.once("error", rejectPort);
    server.listen(CDP_PORT, "127.0.0.1", () => server.close(resolvePort));
  });
}

/** @param {() => Promise<boolean>} predicate @param {number} timeoutMs @param {string} what @returns {Promise<void>} */
async function waitUntil(predicate, timeoutMs, what) {
  /** @type {number} */
  const deadlineMs = Date.now() + timeoutMs;
  while (Date.now() < deadlineMs) {
    if (await predicate()) return;
    await sleep(POLL_INTERVAL_MS);
  }
  throw new Error(`Timed out waiting for ${what}`);
}

/** @param {Page} page @returns {Promise<void>} */
async function registerBridgeHandlers(page) {
  await page.evaluate(({ setupFinishedEvent, liveStateEvent }) => {
    /** @type {BridgeState} */
    const bridge = { pending: new Map(), setupFinished: null, liveMessage: null };
    window.enginePreviewE2e = bridge;
    /** @param {MessageEvent} message @returns {void} */
    function onBridgeMessage(message) {
      /** @type {{type: string, id: string, kind: string, ok: boolean, data: unknown, error?: unknown, payload: SetupFinished}} */
      const reply = typeof message.data === "string" ? JSON.parse(message.data) : message.data;
      if (reply.type === "evt" && reply.kind === setupFinishedEvent) bridge.setupFinished = reply.payload;
      // The live view reports why it stopped or failed to start only through this event.
      if (reply.type === "evt" && reply.kind === liveStateEvent && reply.payload.message) bridge.liveMessage = reply.payload.message;
      if (reply.type !== "res") return;
      /** @type {PendingReply | undefined} */
      const pending = bridge.pending.get(reply.id);
      if (!pending) return;
      clearTimeout(pending.timer);
      bridge.pending.delete(reply.id);
      if (!reply.ok) pending.reject(new Error(JSON.stringify(reply.error)));
      else pending.resolve(reply.data);
    }
    window.chrome.webview.addEventListener("message", onBridgeMessage);
  }, { setupFinishedEvent: SETUP_FINISHED_EVENT, liveStateEvent: LIVE_STATE_CHANGED_EVENT });
}

/** @template T @param {Page} page @param {string} kind @param {Record<string, unknown>} params @returns {Promise<T>} */
async function requestBridge(page, kind, params = {}) {
  return page.evaluate(({ kind, params, timeoutMs }) => new Promise((resolveReply, rejectReply) => {
    /** @type {string} */
    const id = `engine-e2e-${crypto.randomUUID()}`;
    /** @type {BridgeState} */
    const bridge = window.enginePreviewE2e;
    /** @type {number} */
    const timer = setTimeout(() => {
      bridge.pending.delete(id);
      rejectReply(new Error(`Bridge timeout: ${kind}`));
    }, timeoutMs);
    bridge.pending.set(id, { resolve: resolveReply, reject: rejectReply, timer });
    window.chrome.webview.postMessage(JSON.stringify({ type: "req", id, kind, params }));
  }), { kind, params, timeoutMs: REQUEST_TIMEOUT_MS });
}

/** @param {Page} page @param {SetupAction} action @param {string} editorExePath @returns {Promise<void>} */
async function runSetupAction(page, action, editorExePath) {
  // Clear before posting: fast Refresh/Remove may finish before the reply is read.
  await page.evaluate(() => { window.enginePreviewE2e.setupFinished = null; });
  /** @type {{ok: boolean, error?: string}} */
  const accepted = await requestBridge(page, `engineHost/setup/${action === "setup" ? "start" : action}`,
    action === "setup" ? { editorExePath } : {});
  assert.ok(accepted.ok, accepted.error);
  await page.waitForFunction(() => window.enginePreviewE2e.setupFinished !== null, null, { timeout: SETUP_TIMEOUT_MS });
  /** @type {{ok: boolean, action: SetupAction, error?: string}} */
  const finished = await page.evaluate(() => window.enginePreviewE2e.setupFinished);
  assert.equal(finished.action, action);
  assert.ok(finished.ok, finished.error);
  /** @type {SetupStatus} */
  const status = await requestBridge(page, "engineHost/setup/status");
  assert.equal(status.setupAction, null);
  assert.equal(status.isHostRunning, false);
  assert.equal(status.isSetupComplete, action !== "remove");
  assert.equal(status.packageError, null);
  assert.equal(status.isHostOutOfDate, false);
  if (action !== "remove") assert.ok(status.bundledHostVersion);
  else assert.equal(status.managedPath, null);
  console.log(`[engine-preview-e2e] ${action}: passed`);
}

/** @param {Configuration} configuration @returns {Promise<void>} */
async function exerciseEditor(configuration) {
  /** @type {import('node:module').Require} */
  const requireEditor = createRequire(join(REPO_ROOT, "web", "apps", "editor", "package.json"));
  /** @type {typeof import('playwright-core')} */
  const { chromium } = requireEditor("@playwright/test");
  /** @type {import('node:child_process').ChildProcess} */
  const child = spawn(configuration.releaseExe, [], {
    cwd: dirname(configuration.releaseExe), stdio: "ignore",
    env: { ...process.env, WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS: `--remote-debugging-port=${CDP_PORT}` },
  });
  /** @type {Error | null} */
  let launchError = null;
  child.once("error", (error) => { launchError = error; });
  /** @type {import('playwright-core').Browser | null} */
  let browser = null;
  /** @type {Page | null} */
  let page = null;
  /** @type {boolean} */
  let createdSetup = false;
  /** @type {string | null} */
  let managedPath = null;
  /** @type {unknown} */
  let primaryError = null;
  try {
    await waitUntil(async () => {
      if (launchError) throw launchError;
      if (child.exitCode !== null) throw new Error(`Editor exited during startup: ${child.exitCode}`);
      try { return (await fetch(`${CDP_URL}/json/version`, { signal: AbortSignal.timeout(CDP_PROBE_TIMEOUT_MS) })).ok; }
      catch { return false; } // WebView2 has not opened its endpoint yet.
    }, START_TIMEOUT_MS, "WebView2 CDP");
    browser = await chromium.connectOverCDP(CDP_URL, { timeout: START_TIMEOUT_MS });
    await waitUntil(async () => {
      page = browser.contexts().flatMap((context) => context.pages()).find((candidate) => candidate.url().includes("app.local")) ?? null;
      return page !== null;
    }, START_TIMEOUT_MS, "app.local page");
    await registerBridgeHandlers(page);
    // Ask the editor where its setup lives: it falls back to ProgramData for non-ASCII profiles.
    /** @type {SetupStatus} */
    const initial = await requestBridge(page, "engineHost/setup/status");
    assert.equal(initial.managedPath, null, "An existing managed setup must be preserved; remove it before running this lane");
    createdSetup = true;
    await runSetupAction(page, "setup", configuration.editorExePath);
    managedPath = (await requestBridge(page, "engineHost/setup/status")).managedPath;
    /** @type {{ok: boolean, error?: string}} */
    const opened = await requestBridge(page, "file/open", { path: join(REPO_ROOT, "web/apps/editor/tests/fixtures/singleton-emitter.alo"), discardUnsaved: true });
    assert.ok(opened.ok, opened.error);
    /** @type {{state: string}} */
    const started = await requestBridge(page, "engineLive/start");
    /** @type {import('../web/packages/bridge-schema/src/index.ts').EngineLiveStatsDto | null} */
    let lastStats = null;
    try {
      await waitUntil(async () => {
        lastStats = await requestBridge(page, "engineLive/stats");
        return lastStats.state === "live" && lastStats.fps > 0;
      }, LIVE_TIMEOUT_MS, "at least one drawn engine frame");
    } catch (error) {
      // The last stats say whether the host never started, stalled, or ran without drawing.
      /** @type {string | null} */
      const liveMessage = await page.evaluate(() => window.enginePreviewE2e.liveMessage);
      throw new Error(`${errorText(error)}; start replied ${JSON.stringify(started)}, last stats ` +
        `${JSON.stringify(lastStats)}, live view said ${JSON.stringify(liveMessage)}`);
    }
    console.log("[engine-preview-e2e] live frame: passed");
    await requestBridge(page, "engineLive/stop");
    await waitUntil(async () => {
      /** @type {import('../web/packages/bridge-schema/src/index.ts').EngineLiveStatsDto} */
      const stats = await requestBridge(page, "engineLive/stats");
      return stats.state === "off";
    }, STOP_TIMEOUT_MS, "live view stop");
    // Stop ends streaming; Refresh/Remove stop the process that Particle Editor launched.
    await runSetupAction(page, "refresh", configuration.editorExePath);
    await runSetupAction(page, "remove", configuration.editorExePath);
  } catch (error) {
    primaryError = error;
    throw error;
  } finally {
    /** @type {unknown[]} */
    const teardownErrors = await tearDownEditor({ child, browser, page, createdSetup, managedPath, configuration });
    // The first failure explains the run; teardown failures are reported but never replace it.
    for (const error of teardownErrors) console.error(`[engine-preview-e2e] teardown: ${errorText(error)}`);
    if (!primaryError && teardownErrors.length > 0) throw teardownErrors[0];
    const stillRunning = teardownErrors.find((error) => error?.editorStillRunning);
    if (primaryError && stillRunning) primaryError.editorStillRunning = true;
  }
}

/** @param {unknown} error @returns {string} */
function errorText(error) {
  return error instanceof Error ? error.message : String(error);
}

/**
 * Runs every cleanup step even when an earlier one fails, so one bridge error cannot strand a setup.
 * @param {{child: import('node:child_process').ChildProcess, browser: import('playwright-core').Browser | null,
 *   page: Page | null, createdSetup: boolean, managedPath: string | null, configuration: Configuration}} run
 * @returns {Promise<unknown[]>}
 */
async function tearDownEditor({ child, browser, page, createdSetup, managedPath, configuration }) {
  /** @type {unknown[]} */
  const errors = [];
  /** @param {() => Promise<unknown>} step @returns {Promise<void>} */
  const attempt = async (step) => { try { await step(); } catch (error) { errors.push(error); } };
  if (page && !page.isClosed()) {
    await attempt(() => requestBridge(page, "engineLive/stop"));
    // Remove refuses while the live view is still winding down.
    await attempt(() => waitUntil(async () => (await requestBridge(page, "engineLive/stats")).state === "off",
      STOP_TIMEOUT_MS, "live view stop"));
    await attempt(() => requestBridge(page, "engineHost/setup/cancel"));
    await attempt(() => waitUntil(async () => (await requestBridge(page, "engineHost/setup/status")).setupAction === null,
      SETUP_TIMEOUT_MS, "setup cancellation"));
    if (managedPath) {
      // Remove deletes the host's own log, which is the only record of why a live view failed.
      await attempt(async () => {
        /** @type {string} */
        const hostLog = join(managedPath, HOST_LOG_NAME);
        if (existsSync(hostLog)) copyFileSync(hostLog, join(REPO_ROOT, "tests", ".tmp", `engine-preview-e2e-${HOST_LOG_NAME}`));
      });
    }
    if (createdSetup) {
      await attempt(async () => {
        /** @type {SetupStatus} */
        const status = await requestBridge(page, "engineHost/setup/status");
        if (status.managedPath) await runSetupAction(page, "remove", configuration.editorExePath);
      });
    }
    // Closing can destroy the WebView before its bridge reply reaches Playwright.
    await attempt(() => page.evaluate(() => window.chrome.webview.postMessage(JSON.stringify({
      type: "req", id: "engine-e2e-close", kind: "window/close", params: {},
    }))));
    await attempt(() => waitUntil(async () => child.exitCode !== null, STOP_TIMEOUT_MS, "editor shutdown"));
  }
  // Close CDP before any kill check so a stuck kill cannot keep Node alive and hang the gate.
  if (browser) await attempt(() => browser.close());
  if (child.pid && child.exitCode === null) {
    // Scope forced teardown to our process tree, including a host left by a failed bridge.
    spawnSync("taskkill.exe", ["/PID", String(child.pid), "/T", "/F"], { windowsHide: true, stdio: "ignore" });
    await attempt(() => waitUntil(async () => child.exitCode !== null || child.signalCode !== null, STOP_TIMEOUT_MS,
      "forced editor shutdown"));
  }
  if (child.pid && child.exitCode === null && child.signalCode === null) {
    /** @type {Error & {editorStillRunning?: boolean}} */
    const stillRunning = new Error(`Editor process ${child.pid} is still running; registry not restored`);
    stillRunning.editorStillRunning = true;
    errors.push(stillRunning);
  }
  if (managedPath) {
    /** @type {number[]} */
    const strays = listProcessesUnder(managedPath);
    if (strays.length > 0) errors.push(new Error(`Processes still run from ${managedPath}: ${strays.join(", ")}`));
    if (existsSync(managedPath)) errors.push(new Error(`Managed setup left behind at ${managedPath}`));
  }
  return errors;
}

/** @param {string} folder @returns {number[]} */
function listProcessesUnder(folder) {
  /** @type {import('node:child_process').SpawnSyncReturns<string>} */
  const result = spawnSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command",
    "Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($env:PE_E2E_FOLDER, 'OrdinalIgnoreCase') } | ForEach-Object Id"],
  { encoding: "utf8", windowsHide: true, env: { ...process.env, PE_E2E_FOLDER: folder } });
  return result.stdout.split(/\s+/).filter(Boolean).map(Number);
}

/** @returns {Promise<void>} */
async function main() {
  /** @type {Configuration} */
  const configuration = readConfiguration();
  /** @type {PrerequisiteResult} */
  const prerequisites = detectPrerequisites(configuration);
  if (!prerequisites.ok) {
    for (const { what, hint } of prerequisites.missing) console.error(`[engine-preview-e2e] missing prereq: ${what} — ${hint}`);
    throw new Error("Run through the gate with --allow-missing engine-preview-e2e to report a visible SKIP");
  }
  await assertExclusiveRun();
  /** @type {string} */
  const artifactRoot = join(REPO_ROOT, "tests", ".tmp");
  mkdirSync(artifactRoot, { recursive: true });
  /** @type {string} */
  const snapshotRoot = mkdtempSync(join(artifactRoot, "engine-preview-e2e-"));
  /** @type {string} */
  const registrySnapshot = join(snapshotRoot, "settings.reg");
  runRegistryCommand(["export", SETTINGS_KEY, registrySnapshot, "/y"]);
  /** @type {string} */
  const gameRoot = dirname(resolve(configuration.gameDataPath));
  /** @type {string[]} */
  const roots = [gameRoot, dirname(configuration.editorExePath)];
  console.log(`[engine-preview-e2e] snapshots: ${snapshotRoot}`);
  /** @type {FileHashes} */
  const before = await snapshotFileHashes(roots);
  writeFileSync(join(snapshotRoot, "hashes-before.json"), JSON.stringify(before, null, SNAPSHOT_INDENT_SPACES));
  /** @type {boolean} */
  let editorStillRunning = false;
  try {
    await exerciseEditor(configuration);
  } catch (error) {
    editorStillRunning = Boolean(error?.editorStillRunning);
    throw error;
  } finally {
    try {
      /** @type {FileHashes} */
      const after = await snapshotFileHashes(roots);
      writeFileSync(join(snapshotRoot, "hashes-after.json"), JSON.stringify(after, null, SNAPSHOT_INDENT_SPACES));
      /** @type {{ok: boolean, changed: string[]}} */
      const verdict = compareFileHashes(before, after);
      assert.ok(verdict.ok, `Source files changed: ${verdict.changed.join(", ")}`);
      console.log("[engine-preview-e2e] game and map-editor hashes unchanged");
    } finally {
      // A live editor would write its settings back over the restore when it exits.
      if (editorStillRunning) {
        console.error(`[engine-preview-e2e] registry NOT restored; import ${registrySnapshot} after closing the editor`);
      } else {
        // Opening files and closing the normal app also persist recents and view preferences.
        runRegistryCommand(["delete", SETTINGS_KEY, "/f"]);
        runRegistryCommand(["import", registrySnapshot]);
        console.log("[engine-preview-e2e] registry restored");
      }
    }
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => { console.error(`[engine-preview-e2e] FAIL: ${error.message}`); process.exitCode = 1; });
}
