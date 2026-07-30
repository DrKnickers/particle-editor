// probe-cdp-viewport.mjs -- the H1/H2 measurement harness for the
// "can CDP + a faithful D3D9 viewport coexist?" experiment.
// Spec: docs/experiments/2026-06-21-cdp-viewport-coexistence-probe.md
//
//   node scripts/probe-cdp-viewport.mjs --tag baseline [--out-dir C:\tmp\cdp-probe]
//        [--background 0x3A6EA5] [--settle 2000] [--exe <ParticleEditor.exe>]
//        [--open C:\path\to.alo] [--skydome <slot>]
//
// A thin SUPERSET of snap-composed.mjs: it launches ParticleEditor.exe --test-host,
// attaches a LIVE Playwright CDP session (:9222), forces a solid background colour
// into the engine viewport (the asset-independent, shader-independent "did the D3D9
// device render anything under CDP?" discriminator), settles, then captures BOTH:
//   - <tag>-engineRT.png   via debug/capture-frame  -- the ENGINE SCENE RT readback
//                          (AlphaCompositor::CaptureSnapshotToFile). This is the
//                          DECISIVE signal: near-black => #283 bug holds; the bg
//                          colour => the device rendered under CDP.
//   - <tag>-composite.png  via debug/capture-window -- the full PrintWindow composite
//                          (cross-check: React chrome present + viewport not a black hole).
//
// To test a lever you change the BUILD, not a flag here:
//   H1: rebuild the host with --disable-gpu appended at HostWindow.cpp:1182, re-run --tag h1.
//   H2: set ALO_PROBE_ADAPTER in this process's env (inherited by the spawned child) +
//       rebuild the host with the CreateDeviceEx adapter override, re-run --tag h2.
//
// Inherits snap-composed's hardened recipe: exe anchored to this file, stdio ignored
// (host.log captures diagnostics), scope-kill ONLY --test-host instances by command
// line (a daily-driver editor survives), host-death guard, fail-loud on blank PNGs.
// Note (like snap-composed): there is no SIGINT handler -- Ctrl-C mid-run can orphan
// the spawned --test-host host (holding :9222); the NEXT run's pre-spawn scope-kill
// reaps it, so just re-run.
import { chromium } from "@playwright/test";
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, statSync } from "node:fs";
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

const tag = arg("--tag", "probe");
const outDir = resolve(arg("--out-dir", join(repoRoot, "tmp", "cdp-probe")));
const exe = resolve(arg("--exe", defaultExe));
const openAlo = arg("--open");
const skydomeRaw = arg("--skydome");
const skydomeSlot = skydomeRaw != null ? Number(skydomeRaw) : undefined;
// Default background: a distinctive blue. COLORREF is 0x00BBGGRR, so the host
// interprets the low 24 bits as B,G,R -- the exact channel order doesn't matter for
// the probe (any non-black solid colour answers "did it render?"); we just need it
// clearly NOT near-black to the eye when we Read the PNG.
const backgroundRaw = arg("--background", "0x3A6EA5");
const backgroundRgb = Number(backgroundRaw);
const settleRaw = Number(arg("--settle", "2000"));
const settleMs = Number.isFinite(settleRaw) ? settleRaw : 2000;

if (skydomeRaw != null && !Number.isFinite(skydomeSlot)) {
  console.error(`probe: --skydome must be a number, got: ${skydomeRaw}`);
  process.exit(2);
}
if (!Number.isFinite(backgroundRgb)) {
  console.error(`probe: --background must be hex/number (e.g. 0x3A6EA5), got: ${backgroundRaw}`);
  process.exit(2);
}
if (!existsSync(exe)) {
  console.error(`probe: exe not found: ${exe}`);
  process.exit(2);
}
mkdirSync(outDir, { recursive: true });
const engineRtPng = join(outDir, `${tag}-engineRT.png`);
const compositePng = join(outDir, `${tag}-composite.png`);

const CDP = "http://localhost:9222";

async function probeCdp() {
  try {
    return (await fetch(`${CDP}/json/version`)).ok;
  } catch {
    return false;
  }
}

// Scope-kill ONLY --test-host instances by command line (a daily-driver editor of
// the same exe name survives). Mirrors snap-composed/run-native-tests. Fails safe.
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
    p.on("error", () => res());
  });
}

// Pre-spawn cleanup: a stale --test-host process holds :9222, so probeCdp() would
// return ok against the WRONG instance and we'd capture a stale frame (a false
// positive). Kill leftovers, let Windows release the socket + file locks.
await killTestHost();
await sleep(300);

console.error(`probe[${tag}]: launching ${exe} --test-host`);
console.error(`probe[${tag}]: ALO_PROBE_ADAPTER=${process.env.ALO_PROBE_ADAPTER ?? "(unset)"}`);
// env inherited from this process -> ALO_PROBE_ADAPTER (H2) flows to the child.
const child = spawn(exe, ["--test-host"], {
  cwd: repoRoot,
  stdio: ["ignore", "ignore", "ignore"],
  detached: false,
});
let childExited = false;
child.on("exit", () => {
  childExited = true;
});

let browser;
let verdict = 1;
try {
  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (childExited) throw new Error("Host process (--test-host) exited before CDP came up");
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
  await page.waitForFunction(() => typeof window.bridge !== "undefined", null, { timeout: 15000 });

  // Diagnostic: report Chromium's WebGL renderer -- distinguishes "the lever
  // doesn't help" from "the lever never applied". With --disable-gpu in effect ON
  // WINDOWS, ANGLE falls back to WARP, reported as "Microsoft Basic Render Driver"
  // (observed 2026-06-21) -- THAT string PROVES the flag took effect; a real GPU
  // name (e.g. "RTX 3080") means it was dropped. (On Linux/headless Chromium the
  // software fallback is "SwiftShader"/"llvmpipe" instead.)
  try {
    const renderer = await page.evaluate(() => {
      const c = document.createElement("canvas");
      const gl = c.getContext("webgl") || c.getContext("experimental-webgl");
      if (!gl) return "(no webgl context)";
      const ext = gl.getExtension("WEBGL_debug_renderer_info");
      return ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    });
    console.error(`probe[${tag}]: Chromium WebGL renderer = ${renderer}`);
  } catch (e) {
    console.error(`probe[${tag}]: renderer probe failed: ${e.message}`);
  }

  if (openAlo) {
    const aloPath = resolve(openAlo);
    const r = await page.evaluate(
      (p) => window.bridge.request({ kind: "file/open", params: { path: p } }),
      aloPath,
    );
    if (!r || r.ok === false) {
      throw new Error(`file/open failed for ${aloPath}: ${(r && r.error) ? r.error : "unknown"}`);
    }
    console.error(`probe[${tag}]: file/open -> ${JSON.stringify(r)}`);
  }

  // Force a solid background colour AFTER any --open so it wins over the scene /
  // restored state. This is the discriminator: it routes through the engine Clear(),
  // not a shader, so it's asset- and shader-contamination-independent.
  {
    const r = await page.evaluate(
      (rgb) => window.bridge.request({ kind: "engine/set/background", params: { rgb } }),
      backgroundRgb,
    );
    if (r && r.ok === false) throw new Error(`engine/set/background failed: ${r.error ?? "unknown"}`);
    console.error(`probe[${tag}]: background 0x${backgroundRgb.toString(16).toUpperCase()} -> ${JSON.stringify(r)}`);
  }
  if (skydomeSlot !== undefined) {
    const r = await page.evaluate(
      (slot) => window.bridge.request({ kind: "engine/set/skydome-slot", params: { slot } }),
      skydomeSlot,
    );
    // Exact-match success is the overreach control: applied:true at the
    // requested slot falls through; false or a different actual slot aborts.
    if (!r || r.applied !== true || r.slot !== skydomeSlot) {
      throw new Error(
        `engine/set/skydome-slot failed: requested ${skydomeSlot}, actual ${r?.slot ?? "missing"}, applied ${r?.applied ?? "missing"}`,
      );
    }
    console.error(`probe[${tag}]: skydome-slot ${skydomeSlot} -> ${JSON.stringify(r)}`);
  }

  await sleep(settleMs);

  // (1) DECISIVE: engine scene RT readback.
  const rRt = await page.evaluate(
    (p) => window.bridge.request({ kind: "debug/capture-frame", params: { path: p } }),
    engineRtPng,
  );
  console.error(`probe[${tag}]: debug/capture-frame -> ${JSON.stringify(rRt)}`);
  // (2) CROSS-CHECK: full composite.
  const rWin = await page.evaluate(
    (p) => window.bridge.request({ kind: "debug/capture-window", params: { path: p } }),
    compositePng,
  );
  console.error(`probe[${tag}]: debug/capture-window -> ${JSON.stringify(rWin)}`);

  // Fail loud on blank/missing artifacts (a successful bridge call can still leave a
  // 0-byte PNG). Size only guards blank-vs-written; the BLACK-vs-COLOUR verdict is by
  // visually Reading <tag>-engineRT.png (a uniform RT is tiny regardless of colour).
  const szRt = existsSync(engineRtPng) ? statSync(engineRtPng).size : 0;
  const szWin = existsSync(compositePng) ? statSync(compositePng).size : 0;
  if (szRt < 256) throw new Error(`engine RT capture wrote ${szRt} bytes (blank/missing): ${engineRtPng}`);
  if (szWin < 1024) throw new Error(`composite capture wrote ${szWin} bytes (blank/missing): ${compositePng}`);
  console.error(`probe[${tag}]: OK  engineRT=${engineRtPng} (${szRt}B)  composite=${compositePng} (${szWin}B)`);
  console.error(`probe[${tag}]: >>> now visually inspect ${tag}-engineRT.png : near-black = FAIL, blue = PASS <<<`);
  verdict = 0;
} catch (e) {
  console.error(`probe[${tag}]: FAILED: ${e.message}`);
  verdict = 1;
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
process.exit(verdict);
