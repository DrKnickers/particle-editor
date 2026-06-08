// verify-force-align.mjs — no-user automated test for the cross-mode
// Force Align write path AND the raw-value panel display, using the
// ALO_SETTINGS_LIVE test seam (see BridgeDispatcher.h).
//
// Why this exists: the Force Align WRITE path (checkbox -> bridge ->
// dispatcher -> LightingForceFillAlignment REG_DWORD) can't be exercised
// on a faithful launch (no CDP) and is gated off under plain --test-host
// (so the a11y harness never mutates the dev registry). ALO_SETTINGS_LIVE
// lifts that gate ONLY for this launch, so the genuine registry round-trip
// runs over CDP with zero user participation and the a11y harness — which
// never sets the env var — stays deterministic.
//
// What it proves, end to end through the REAL React UI + native dispatcher:
//   1. READ  — with the registry flag = 1, the Force Align checkbox renders
//              checked (settings/lighting get -> live registry).
//   2. DISPLAY (Part 2) — the Sun intensity spinner shows the saved raw
//              value (0.5), NOT the folded `intensity=1` the old engine-
//              snapshot seed produced. Cross-checked against the bridge DTO.
//   3. WRITE  — clicking the checkbox flips LightingForceFillAlignment to 0
//              in the registry (settings/lighting-force-align/set -> live).
//
// The original registry value is saved up front and restored in a finally,
// so the user's daily-driver state is untouched. Run on demand:
//   node web/apps/editor/scripts/verify-force-align.mjs
// Requires: composition dist/ built (pnpm --filter @particle-editor/editor build)
// and x64\Release\ParticleEditor.exe built.

import { spawn, execFileSync } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "@playwright/test";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, "../../../..");
const exe = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
const CDP = "http://127.0.0.1:9222";
const REG_KEY = "HKCU\\Software\\AloParticleEditor";
const REG_VAL = "LightingForceFillAlignment";

// ---- registry helpers (REG_DWORD) -----------------------------------------
function regRead() {
  try {
    const out = execFileSync("reg", ["query", REG_KEY, "/v", REG_VAL], {
      encoding: "utf8",
    });
    const m = out.match(/REG_DWORD\s+0x([0-9a-fA-F]+)/);
    return m ? parseInt(m[1], 16) : null;
  } catch {
    return null; // value absent
  }
}
function regWrite(v) {
  execFileSync("reg", ["add", REG_KEY, "/v", REG_VAL, "/t", "REG_DWORD",
    "/d", String(v), "/f"], { stdio: "ignore" });
}
function regDelete() {
  try { execFileSync("reg", ["delete", REG_KEY, "/v", REG_VAL, "/f"], { stdio: "ignore" }); }
  catch { /* already absent */ }
}

function killTestHost() {
  try {
    execFileSync("powershell.exe", ["-NoProfile", "-NonInteractive", "-Command",
      "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
      "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"], { stdio: "ignore" });
  } catch { /* nothing to kill */ }
}

async function probeCdp() {
  try { return (await fetch("http://127.0.0.1:9222/json/version")).ok; }
  catch { return false; }
}

const checks = [];
function check(name, ok, detail) {
  checks.push({ name, ok, detail });
  console.log(`  ${ok ? "PASS" : "FAIL"}  ${name}${detail ? ` — ${detail}` : ""}`);
}

async function main() {
  const original = regRead();
  console.log(`[verify-force-align] original ${REG_VAL} = ${original === null ? "(absent)" : original}`);

  let child;
  let browser;
  try {
    // Known starting state: flag ON.
    regWrite(1);

    killTestHost();
    await sleep(300);

    console.log(`[verify-force-align] launching ${exe} --new-ui --test-host (ALO_SETTINGS_LIVE=1) ...`);
    child = spawn(exe, ["--new-ui", "--test-host"], {
      cwd: repoRoot,
      stdio: ["ignore", "ignore", "ignore"],
      env: { ...process.env, ALO_SETTINGS_LIVE: "1" },
    });
    let exited = false;
    child.on("exit", () => { exited = true; });

    const deadline = Date.now() + 30_000;
    let up = false;
    while (Date.now() < deadline) {
      if (exited) throw new Error("host exited before CDP came up");
      if (await probeCdp()) { up = true; break; }
      await sleep(500);
    }
    if (!up) throw new Error("CDP did not come up at 127.0.0.1:9222 within 30s");

    browser = await chromium.connectOverCDP(CDP);
    const ctx = browser.contexts()[0];
    const page = ctx.pages()[0] ?? (await ctx.waitForEvent("page"));
    await page.waitForFunction(
      () => typeof (window).bridge !== "undefined", null, { timeout: 15_000 });

    // Cross-check the dispatcher DTO directly (live registry under the seam).
    const dto = await page.evaluate(async () =>
      (window).bridge.request({ kind: "settings/lighting", params: {} }));
    check("bridge settings/lighting reads live registry (forceAlign=true)",
      dto.forceAlign === true, `forceAlign=${dto.forceAlign}`);
    check("bridge DTO carries the RAW sun intensity split (0.5, not folded 1)",
      Math.abs(dto.sun.intensity - 0.5) < 1e-6, `sun.intensity=${dto.sun.intensity}`);

    // Open the Lighting pane via the new toolbar button.
    await page.getByRole("button", { name: "Toggle Lighting panel" }).click();
    const checkbox = page.getByLabel("Force Align Fill Lights");
    await checkbox.waitFor({ state: "visible", timeout: 5_000 });

    // (1) READ path — checkbox reflects registry = 1.
    check("checkbox reflects registry flag = 1 (rendered checked)",
      await checkbox.isChecked() === true);

    // (2) DISPLAY (Part 2) — Sun intensity shows the raw 0.5, not folded 1.
    const intensityStr = await page.getByLabel("Sun intensity").inputValue();
    const intensity = parseFloat(intensityStr);
    check("Sun intensity spinner shows the RAW saved value (0.5, not folded 1)",
      Math.abs(intensity - 0.5) < 1e-6, `displayed="${intensityStr}"`);

    // (3) WRITE path — toggling the checkbox writes the registry.
    await checkbox.click();
    await sleep(400); // let the fire-and-forget set round-trip to the host
    const after = regRead();
    check("clicking the checkbox wrote LightingForceFillAlignment = 0",
      after === 0, `registry now = ${after === null ? "(absent)" : after}`);
  } finally {
    if (browser) { try { await browser.close(); } catch { /* ignore */ } }
    if (child) { try { child.kill(); } catch { /* ignore */ } }
    killTestHost();
    // Restore the user's original registry state exactly.
    if (original === null) regDelete(); else regWrite(original);
    console.log(`[verify-force-align] restored ${REG_VAL} = ${original === null ? "(absent)" : original}`);
  }

  const failed = checks.filter((c) => !c.ok);
  console.log(`\n[verify-force-align] ${checks.length - failed.length}/${checks.length} checks passed`);
  process.exit(failed.length === 0 ? 0 : 1);
}

main().catch((err) => {
  console.error("[verify-force-align]", err);
  killTestHost();
  process.exit(1);
});
