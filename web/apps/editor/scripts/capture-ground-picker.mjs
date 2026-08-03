// capture-ground-picker.mjs -- open the Ground picker popover under CDP and
// screenshot it (DOM/React chrome -- renders faithfully under CDP; only the
// separate D3D9 viewport blacks, which is irrelevant to the picker UI).
//
//   node scripts/capture-ground-picker.mjs --out <png> [--game <dir>] [--exe <exe>]
//
// With --game pointed at an install lacking the ground textures, the Grass/Sand/
// Snow tiles render greyed/disabled -- the visual proof of graceful degradation.
import { chromium } from "@playwright/test";
import { spawn } from "node:child_process";
import { existsSync, statSync } from "node:fs";
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
const exe = resolve(arg("--exe", defaultExe));
let out = arg("--out");
const gameDir = arg("--game");
if (!out) { console.error("capture-ground-picker: --out <png> required"); process.exit(2); }
out = resolve(out);
if (!existsSync(exe)) { console.error(`capture-ground-picker: exe not found: ${exe}`); process.exit(2); }

const CDP = "http://localhost:9222";
const probeCdp = async () => { try { return (await fetch(`${CDP}/json/version`)).ok; } catch { return false; } };
function killTestHost() {
  return new Promise((res) => {
    const cmd =
      "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
      "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
    const p = spawn("powershell.exe",
      ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", cmd],
      { stdio: "ignore", shell: false });
    p.on("exit", () => res()); p.on("error", () => res());
  });
}

await killTestHost();
await sleep(300);
const spawnArgs = gameDir ? ["--test-host", gameDir] : ["--test-host"];
const child = spawn(exe, spawnArgs, { cwd: repoRoot, stdio: ["ignore", "ignore", "ignore"], detached: false });
let childExited = false;
child.on("exit", () => { childExited = true; });

let browser;
try {
  let ready = false;
  for (let i = 0; i < 60; i++) {
    if (childExited) throw new Error("Host (--test-host) exited before CDP came up");
    if (await probeCdp()) { ready = true; break; }
    await sleep(500);
  }
  if (!ready) throw new Error("CDP :9222 did not come up within 30s");

  browser = await chromium.connectOverCDP(CDP);
  const ctx = browser.contexts()[0];
  if (!ctx) throw new Error("CDP: no browser contexts");
  const page = ctx.pages()[0] ?? (await ctx.waitForEvent("page"));
  await page.waitForFunction(() => typeof window.bridge !== "undefined", null, { timeout: 15000 });
  await sleep(1500); // let the toolbar render

  // Open the Ground picker popover (trigger has aria-label="Ground").
  await page.getByRole("button", { name: "Ground", exact: true }).click();
  // Wait for a bundled tile to confirm the popover is open.
  await page.getByRole("button", { name: /^(Grass|Grass \(unavailable)/ }).waitFor({ timeout: 8000 });
  await sleep(400); // settle the open animation

  await page.screenshot({ path: out });
  const sz = existsSync(out) ? statSync(out).size : 0;
  if (sz < 1024) throw new Error(`screenshot wrote ${sz} bytes (blank/missing)`);
  console.log(`capture-ground-picker: wrote ${out} (${sz} bytes)`);
} catch (e) {
  console.error(`capture-ground-picker: FAILED: ${e.message}`);
  process.exitCode = 1;
} finally {
  try { await browser?.close(); } catch { /* ignore */ }
  try { child.kill(); } catch { /* ignore */ }
  await sleep(300);
  await killTestHost();
}
