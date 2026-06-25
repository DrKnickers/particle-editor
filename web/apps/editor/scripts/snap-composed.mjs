// snap-composed.mjs -- drive the live editor over CDP and capture the COMPOSED
// window (React UI + viewport) to a PNG via the debug/capture-window bridge.
//
//   node scripts/snap-composed.mjs --out C:\tmp\shot.png [--open C:\path\to.alo]
//        [--exe <path-to-ParticleEditor.exe>] [--settle 1500]
//        [--skydome <slot>] [--background 0xRRGGBB]
//
// Launches ParticleEditor.exe --test-host, waits for CDP (:9222) + window.bridge,
// optionally opens an .alo, optionally forces a skydome slot and/or a solid
// background colour into the viewport (so a downstream probe can assert the
// viewport is non-black), settles, then posts debug/capture-window. Exit 0 ok.
//
// Mirrors the connect + spawn/cleanup recipe of run-native-tests.mjs:
//   - exe path is anchored to this file's location (repoRoot/x64/Release), so the
//     default works regardless of the process cwd.
//   - the host is launched with stdio:"ignore" (host diagnostics still land in
//     %LOCALAPPDATA%\AloParticleEditor\host.log) to avoid the QuickEdit-freeze
//     footgun documented in run-native-tests.mjs.
//   - cleanup scope-kills only --test-host instances by command line, so a
//     daily-driver editor build with the same exe name survives.
//   - in --test-host mode App.tsx swaps window.bridge for a TestHostBridge that
//     routes over the host-object IPC channel (WebView2 drops page->host
//     postMessage while CDP is attached) -- window.bridge.request
//     is the call shape the native specs use, so we mirror it.
import { chromium } from "@playwright/test";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { setTimeout as sleep } from "node:timers/promises";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, "../../../..");
const defaultExe = join(repoRoot, "x64", "Release", "ParticleEditor.exe");

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}
let out = arg("--out");
const openAlo = arg("--open");
// --skydome <slot>: force a known skydome into the viewport (engine/set/skydome-slot).
// --background 0xRRGGBB: force a solid background colour (engine/set/background).
// Either gives a downstream probe a guaranteed non-black viewport to detect, so a
// regression where PrintWindow drops the D3D layer (viewport -> black) is caught.
const skydomeRaw = arg("--skydome");
const skydomeSlot = skydomeRaw != null ? Number(skydomeRaw) : undefined;
const backgroundRaw = arg("--background");
// Number() accepts "0x..." hex (and plain decimal); NaN if malformed.
const backgroundRgb = backgroundRaw != null ? Number(backgroundRaw) : undefined;
if (skydomeRaw != null && !Number.isFinite(skydomeSlot)) {
  console.error(`snap-composed: --skydome must be a number, got: ${skydomeRaw}`);
  process.exit(2);
}
if (backgroundRaw != null && !Number.isFinite(backgroundRgb)) {
  console.error(`snap-composed: --background must be a hex/number (e.g. 0x3A6EA5), got: ${backgroundRaw}`);
  process.exit(2);
}
// A non-numeric --settle yields NaN here, and sleep(NaN) resolves immediately --
// silently skipping the settle. Fall back to the default when not finite.
const settleRaw = Number(arg("--settle", "1500"));
const settleMs = Number.isFinite(settleRaw) ? settleRaw : 1500;
const exe = resolve(arg("--exe", defaultExe));
if (!out) {
  console.error("snap-composed: --out <png> required");
  process.exit(2);
}
// Resolve --out against THIS script's cwd the same way --open is resolved,
// so a relative --out doesn't end up relative to the host process's cwd.
out = resolve(out);
if (!existsSync(exe)) {
  console.error(`snap-composed: exe not found: ${exe}`);
  process.exit(2);
}

const CDP = "http://localhost:9222";

async function probeCdp() {
  try {
    return (await fetch(`${CDP}/json/version`)).ok;
  } catch {
    return false;
  }
}

// Scope the cleanup to ONLY the --test-host instances this script spawns, by
// command line -- a blanket taskkill /IM would also kill a daily-driver editor
// of the same exe name. Mirrors run-native-tests.mjs killAny(). Fails safe.
function killTestHost() {
  return new Promise((res) => {
    const cmd =
      "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
      "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
    const p = spawn(
      "powershell.exe",
      ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", cmd],
      { stdio: "ignore", shell: false },
    );
    p.on("exit", () => res());
    p.on("error", () => res()); // powershell missing -> nothing to clean
  });
}

// Pre-spawn cleanup (mirrors run-native-tests.mjs killAny() before launch).
// A stale --test-host process poisons the next run: the new
// child can't bind :9222, but probeCdp() still returns ok against the STALE
// instance -- so we'd connect to and capture the WRONG window (a false-positive
// PNG). Kill any leftover --test-host, then give Windows a moment to release
// the :9222 socket + file locks before launching fresh.
await killTestHost();
await sleep(300);

const child = spawn(exe, ["--test-host"], {
  cwd: repoRoot,
  stdio: ["ignore", "ignore", "ignore"],
  detached: false,
});

// Host-death guard. If the host crashes on startup, the CDP
// probe loop would otherwise burn the full 30s ceiling before reporting a
// generic timeout. Track the child's exit so we can break early and report the
// real cause distinctly.
let childExited = false;
child.on("exit", () => {
  childExited = true;
});

let browser;
try {
  let ready = false;
  // WebView2 init + DPI/COM startup can take 5-10s cold; 60 * 500ms = 30s ceiling.
  for (let i = 0; i < 60; i++) {
    if (childExited) {
      throw new Error("Host process (--test-host) exited before CDP came up");
    }
    if (await probeCdp()) {
      ready = true;
      break;
    }
    await sleep(500);
  }
  if (!ready) throw new Error("CDP :9222 did not come up within 30s");

  browser = await chromium.connectOverCDP(CDP);
  const ctx = browser.contexts()[0];
  if (!ctx) throw new Error("CDP: no browser contexts attached");
  const page = ctx.pages()[0] ?? (await ctx.waitForEvent("page"));
  // WebView2 navigation is async vs. host launch; wait until App.tsx attaches
  // window.bridge (the TestHostBridge) before driving it.
  await page.waitForFunction(
    () => typeof window.bridge !== "undefined",
    null,
    { timeout: 15000 },
  );

  if (openAlo) {
    const aloPath = resolve(openAlo);
    // The host's file/open handler reports failures as sendOk({ ok:false, error }) --
    // i.e. the OUTER bridge envelope is { ok:true, data:{ ok:false, error } }, so
    // TestHostBridge.request() (which only throws on an outer { ok:false }) does NOT
    // throw. request() resolves to the inner data, so we must inspect it ourselves
    // and fail loudly -- otherwise we'd capture the WRONG (unloaded) scene and
    // report a false success.
    const r = await page.evaluate(
      (p) => window.bridge.request({ kind: "file/open", params: { path: p } }),
      aloPath,
    );
    if (!r || r.ok === false) {
      throw new Error(
        `file/open failed for ${aloPath}: ${(r && r.error) ? r.error : "unknown"}`,
      );
    }
    console.error(`snap-composed: file/open -> ${JSON.stringify(r)}`);
  }

  // Force viewport content AFTER any --open, so the skydome/background overrides
  // whatever the opened scene (or the restored daily-driver state) had. Both
  // handlers reply sendOk(json::object()), i.e. the OUTER envelope is { ok:true,
  // data:{} } -- request() resolves to the inner {} and only throws on an outer
  // { ok:false }. We still inspect the inner result and fail loudly on { ok:false }
  // (mirrors the --open guard) so a future handler that starts reporting inner
  // failures cannot pass silently.
  if (skydomeSlot !== undefined) {
    const r = await page.evaluate(
      (slot) =>
        window.bridge.request({ kind: "engine/set/skydome-slot", params: { slot } }),
      skydomeSlot,
    );
    if (r && r.ok === false) {
      throw new Error(
        `engine/set/skydome-slot failed: ${r.error ? r.error : "unknown"}`,
      );
    }
    console.error(`snap-composed: skydome-slot ${skydomeSlot} -> ${JSON.stringify(r)}`);
  }
  if (backgroundRgb !== undefined) {
    const r = await page.evaluate(
      (rgb) =>
        window.bridge.request({ kind: "engine/set/background", params: { rgb } }),
      backgroundRgb,
    );
    if (r && r.ok === false) {
      throw new Error(
        `engine/set/background failed: ${r.error ? r.error : "unknown"}`,
      );
    }
    console.error(
      `snap-composed: background 0x${backgroundRgb.toString(16).toUpperCase()} -> ${JSON.stringify(r)}`,
    );
  }

  await sleep(settleMs);

  const res = await page.evaluate(
    (p) => window.bridge.request({ kind: "debug/capture-window", params: { path: p } }),
    out,
  );
  console.error(`snap-composed: debug/capture-window -> ${JSON.stringify(res)}`);
  // request() throws on an outer { ok:false }, but a successful bridge call can
  // still leave a missing/blank PNG (e.g. PrintWindow drew nothing). Verify the
  // artifact before declaring success.
  const { statSync } = await import("node:fs");
  const sz = existsSync(out) ? statSync(out).size : 0;
  if (sz < 1024) throw new Error(`capture wrote ${sz} bytes to ${out} (blank/missing)`);
  console.error(`snap-composed: wrote ${out} (${sz} bytes)`);
} catch (e) {
  console.error(`snap-composed: FAILED: ${e.message}`);
  process.exitCode = 1;
} finally {
  try {
    await browser?.close();
  } catch {
    /* ignore */
  }
  try {
    child.kill();
  } catch {
    /* ignore */
  }
  await sleep(300);
  await killTestHost();
}
