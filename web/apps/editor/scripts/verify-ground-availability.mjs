// verify-ground-availability.mjs -- live confirmation that the runtime-loaded
// grass/sand/snow ground textures resolve from the user's game install and that
// the host emits per-slot availability.
//
//   node scripts/verify-ground-availability.mjs [--exe <ParticleEditor.exe>]
//
// Launches ParticleEditor.exe --test-host (uses the HKCU GameDataPath -- the
// real FoC install on this box), connects over CDP, and via window.bridge:
//   1. reads engine/state/snapshot.groundSlotAvailable (expect dirt/grass/sand/
//      snow/solid all true on a box WITH the install),
//   2. selects each game-sourced slot (grass=1, sand=2, snow=3) and re-reads
//      groundTexture -- if the install resolves the texture the selection sticks;
//      if it had failed, ReloadGroundTexture would bounce the index back to 0.
// Exit 0 = all assertions pass. The viewport itself is black under CDP (expected
// -- see docs/CAPTURE_MODES.md); this checks the host/engine logic, not pixels.
// For a faithful RENDER of the grass ground use the non-CDP --drive path.
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
const exe = resolve(arg("--exe", defaultExe));
// --game <dir>: point the host at this game install (extra positional arg the
// host treats as the EaW/FoC path). Omit ⇒ the registry GameDataPath (real
// install). --degraded inverts the expectation: grass/sand/snow must be
// UNAVAILABLE (e.g. an empty fixture install with no ground textures).
const gameDir = arg("--game");
const degraded = process.argv.includes("--degraded");
if (!existsSync(exe)) {
  console.error(`verify-ground: exe not found: ${exe}`);
  process.exit(2);
}
const CDP = "http://localhost:9222";
const probeCdp = async () => {
  try { return (await fetch(`${CDP}/json/version`)).ok; } catch { return false; }
};
function killTestHost() {
  return new Promise((res) => {
    const cmd =
      "Get-CimInstance Win32_Process -Filter \"Name='ParticleEditor.exe'\" | " +
      "Where-Object { $_.CommandLine -like '*--test-host*' } | " +
      "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }";
    const p = spawn("powershell.exe",
      ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", cmd],
      { stdio: "ignore", shell: false });
    p.on("exit", () => res());
    p.on("error", () => res());
  });
}

await killTestHost();
await sleep(300);
const spawnArgs = gameDir ? ["--test-host", gameDir] : ["--test-host"];
const child = spawn(exe, spawnArgs, { cwd: repoRoot, stdio: ["ignore", "ignore", "ignore"], detached: false });
let childExited = false;
child.on("exit", () => { childExited = true; });

let browser;
let failed = 0;
const ok = (m) => console.log(`  ok: ${m}`);
const fail = (m) => { console.log(`  FAIL: ${m}`); failed++; };
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

  const snapshot = () => page.evaluate(() => window.bridge.request({ kind: "engine/state/snapshot", params: {} }));
  const setSlot = (slot) => page.evaluate((s) => window.bridge.request({ kind: "engine/set/ground-texture", params: { slot: s } }), slot);

  console.log(degraded
    ? "MODE: degraded (empty fixture install -- grass/sand/snow must be UNAVAILABLE)"
    : "MODE: happy path (real install -- grass/sand/snow must be available)");

  const snap0 = await snapshot();
  const avail = snap0.groundSlotAvailable;
  console.log(`groundSlotAvailable = ${JSON.stringify(avail)}`);
  if (!Array.isArray(avail) || avail.length !== 8) fail(`groundSlotAvailable not an 8-array: ${JSON.stringify(avail)}`);
  else {
    // Dirt + Solid Color are always available regardless of the install.
    if (avail[0] === true) ok("slot 0 (dirt) available"); else fail("dirt should always be available");
    if (avail[4] === true) ok("slot 4 (solid color) available"); else fail("solid color should always be available");
    for (const [slot, name] of [[1, "grass"], [2, "sand"], [3, "snow"]]) {
      const want = !degraded;
      if (avail[slot] === want) {
        ok(degraded ? `slot ${slot} (${name}) correctly UNAVAILABLE (greyed)` : `slot ${slot} (${name}) resolves from the install`);
      } else {
        fail(`slot ${slot} (${name}) availability=${avail[slot]} (expected ${want})`);
      }
    }
  }

  // Selection behaviour: happy path sticks at the slot; degraded is refused by
  // SetGroundTexture (!IsGroundSlotAvailable) so the index stays at dirt (0).
  for (const [slot, name] of [[1, "grass"], [2, "sand"], [3, "snow"]]) {
    await setSlot(slot);
    const snap = await snapshot();
    if (!degraded) {
      if (snap.groundTexture === slot) ok(`select ${name} sticks (groundTexture=${slot}, no fallback bounce)`);
      else fail(`select ${name}: groundTexture=${snap.groundTexture} (expected ${slot} -- bounced => resolve failed)`);
    } else {
      if (snap.groundTexture === 0) ok(`select ${name} refused (groundTexture stays 0/dirt -- no hard-fail)`);
      else fail(`select ${name}: groundTexture=${snap.groundTexture} (expected 0 -- an unavailable slot must be refused)`);
    }
  }
} catch (e) {
  console.error(`verify-ground: FAILED: ${e.message}`);
  failed++;
} finally {
  try { await browser?.close(); } catch { /* ignore */ }
  try { child.kill(); } catch { /* ignore */ }
  await sleep(300);
  await killTestHost();
}
if (failed > 0) { console.log(`[verify-ground] FAILED (${failed})`); process.exit(1); }
console.log("[verify-ground] ALL PASS");
process.exit(0);
