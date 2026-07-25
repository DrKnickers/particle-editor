#!/usr/bin/env node
// [clip-batch] Bounded-parallel batch renderer for the landing-page clips.
//
//   node scripts/clip-batch/render-all.mjs [--jobs N] [--only a,b] [--dry-run] [--allow-stale]
//
// Per clip: record (ParticleEditor.exe --record) -> gates -> encode, each clip
// advancing independently through its pipeline (no cross-clip barrier). The
// record processes are safe to run concurrently: --record gets a per-PID
// WebView2 profile + log and never persists state (HostWindow.cpp,
// ComputeUserDataFolder). Windows must not be MINIMIZED while recording
// (PrintWindow reads the DWM composition surface); overlap is fine.
//
// The clip table below is the EXECUTABLE transcription of the recipes in
// tasks/clips/README.md — the README stays the human authority for the *why*
// of every crop/trim/crossfade value; table entries cite their README lines.
// `--dry-run` prints the exact commands so the two can be diffed.
//
// Failure classification (no auto-retry in v1 — exit 3 is NOT reliably
// transient: it also covers bad open paths / preflight key misses / press-
// target misses, ClipRunner.cpp): the summary names the failure class and
// prints the exact solo rerun command. Plan: tasks/todo.md.

import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, unlinkSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
const exePath = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
const distIndex = join(repoRoot, "web", "apps", "editor", "dist", "index.html");
const lockPath = join(repoRoot, "clips", ".render-all.lock");
const localAppData = process.env.LOCALAPPDATA || "";

// ---------------------------------------------------------------------------
// Clip table — transcribed from tasks/clips/README.md (encode + gate blocks).
// partialScan runs for EVERY clip (capture-race dropout guard the sampled
// SSIM/seam gates can't see — README "Capture-clean gate"); its --start is
// the clip's encode trim (frames before the trim never ship) and its crop is
// the clip's content region (default = the 1280x960 viewport region).
// ---------------------------------------------------------------------------
const CLIPS = [
  {
    name: "hero",                               // README.md:37-87
    timeline: "tasks/clips/hero.timeline.json",
    frames: "clips/hero",
    encode: ["--frames", "clips/hero", "--fps", "60", "--loop", "crossfade", "--crossfade", "1.0",
             "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/hero.mp4",
             "--poster", "site/media-local/hero-poster.jpg", "--poster-frame", "180"],
    gates: [
      ["scripts/clip-verify/seam-churn.mjs", "--frames", "clips/hero", "--fps", "60", "--start", "0",
       "--crossfade-frames", "60", "--roi", "600:400:500:200", "--max-ratio", "0.10"],
    ],
    partialScan: { start: 0 },                  // default viewport crop
  },
  {
    name: "interface-overhaul",
    timeline: "tasks/clips/interface-overhaul.timeline.json",
    frames: "clips/interface-overhaul",
    encode: ["--frames", "clips/interface-overhaul", "--fps", "60",
             "--loop", "crossfade", "--crossfade", "1.0",
             "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/interface-overhaul.mp4",
             "--poster", "site/media-local/interface-overhaul-poster.jpg", "--poster-frame", "60"],
    gates: [
      // The toolbar/panel chrome must round-trip exactly. The live particle lifecycle is
      // crossfade-owned and requires the README's full-frame visual seam oracle.
      ["scripts/clip-verify/seam-match.mjs", "--frames", "clips/interface-overhaul",
       "--start", "0", "--crop", "1264:150:8:0"],
    ],
    partialScan: { start: 0 },
  },
  {
    name: "faith",                              // README.md:88-125
    timeline: "tasks/clips/faith.timeline.json",
    frames: "clips/faith",
    encode: ["--frames", "clips/faith", "--fps", "60", "--loop", "crossfade", "--crossfade", "1.0",
             "--start", "120", "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/faith.mp4",
             "--poster", "site/media-local/faith-poster.jpg", "--poster-frame", "360"],
    gates: [
      ["scripts/clip-verify/seam-churn.mjs", "--frames", "clips/faith", "--fps", "60", "--start", "120",
       "--crossfade-frames", "60", "--roi", "460:380:420:200", "--max-ratio", "0.10"],
    ],
    partialScan: { start: 120 },
  },
  {
    name: "f02",                                // README.md:128-146
    timeline: "tasks/clips/f02.timeline.json",
    frames: "clips/f02",
    encode: ["--frames", "clips/f02", "--fps", "60", "--loop", "crossfade", "--crossfade", "0.5",
             "--start", "195", "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/f02.mp4",
             "--poster", "site/media-local/f02-poster.jpg"],
    gates: [
      ["scripts/clip-verify/cursor-on-target.mjs", "--sidecar", "clips/f02/cursor-sidecar.json"],
    ],
    partialScan: { start: 195 },
  },
  {
    name: "f02-reorder",                        // README.md:180-211 (frames dir is clips/f02r)
    timeline: "tasks/clips/f02-reorder.timeline.json",
    frames: "clips/f02r",
    encode: ["--frames", "clips/f02r", "--fps", "60", "--start", "60",
             "--loop", "crossfade", "--crossfade", "1.5", "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/f02-reorder.mp4",
             "--poster", "site/media-local/f02-reorder-poster.jpg"],
    gates: [
      ["scripts/clip-verify/seam-match.mjs", "--frames", "clips/f02r", "--start", "60", "--crop", "324:120:8:140"],
      ["scripts/clip-verify/cursor-on-target.mjs", "--sidecar", "clips/f02r/cursor-sidecar.json"],
    ],
    partialScan: { start: 60 },
  },
  {
    name: "f04",                                // README.md:156-179 (3200x1460 frames; scale:4)
    timeline: "tasks/clips/f04.timeline.json",
    frames: "clips/f04",
    encode: ["--frames", "clips/f04", "--fps", "60", "--start", "60", "--loop", "none",
             "--crop", "1786:1400:330:30", "--crf", "18",
             "--out", "site/media-local/f04.mp4",
             "--poster", "site/media-local/f04-poster.jpg", "--poster-frame", "60"],
    gates: [
      ["scripts/clip-verify/cursor-on-target.mjs", "--sidecar", "clips/f04/cursor-sidecar.json"],
    ],
    // Picker region (the encode crop): the viewport behind it is deliberately
    // empty, so the default viewport crop would probe nothing.
    partialScan: { start: 60, crop: "1786:1400:330:30" },
  },
  {
    name: "spawner",                             // README.md:237-254 (added #503, wired here)
    timeline: "tasks/clips/spawner.timeline.json",
    frames: "clips/spawner",
    encode: ["--frames", "clips/spawner", "--fps", "60", "--loop", "none",
             "--crop", "1264:952:8:0", "--crf", "16",
             "--out", "site/media-local/spawner.mp4",
             "--poster", "site/media-local/spawner-poster.jpg"],
    // No cursor interaction (static invisible cursor point) and no loop round-trip
    // (--loop none, hard cut) — cursor-on-target and seam-match don't apply here;
    // partialScan (run for every clip) is the only meaningful automated gate.
    gates: [],
    partialScan: { start: 0 },                  // default viewport crop
  },
];

// Child-log signatures that fail a clip even when frames look plausible
// (multi-instance GPU/DComp contention -> empty/frozen viewport; plan risk 12).
// Verbatim substrings of the host's actual log lines (HostWindow.cpp,
// the [COMP-engine-*] family) — review-corrected: the original guesses
// ("AttachEngineVisual failed") matched nothing.
const LOG_FAIL_SIGNATURES = [
  "[COMP-engine-fail]",
  "engine visual NOT attached",
  "composition mode continues without engine pixels",
];

// exit-code + log-signature -> failure class (plan §3 "Failure handling").
function classifyFailure(exitCode, logText) {
  if (exitCode === "timeout") return "hung(parent-timeout)";
  if (/record: ack timeout/.test(logText)) return "ack-timeout";
  if (/startup watchdog|app\/ready never arrived|no high-resolution timer/.test(logText)) return "startup-timeout";
  if (/bad timeline|file\/open failed|preflight: no key|did not resolve|malformed\/missing ack/.test(logText)) return "authoring";
  if (/move tmp -> out failed|sidecar/.test(logText)) return "capture/publish";
  if (exitCode === 2) return "authoring";
  if (exitCode === 4) return "capture/publish";
  if (exitCode === 5) return "startup-timeout";
  return "record-failed";
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------
function parseArgs(argv) {
  const args = { jobs: 3, only: null, dryRun: false, allowStale: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--jobs") args.jobs = Number(argv[++i]);
    else if (a === "--only") args.only = String(argv[++i]).split(",").map((s) => s.trim()).filter(Boolean);
    else if (a === "--dry-run") args.dryRun = true;
    else if (a === "--allow-stale") args.allowStale = true;
    else { console.error(`unknown arg: ${a}\nusage: render-all.mjs [--jobs N] [--only a,b] [--dry-run] [--allow-stale]`); process.exit(2); }
  }
  if (!Number.isInteger(args.jobs) || args.jobs < 1 || args.jobs > 16) {
    console.error("--jobs must be an integer 1..16"); process.exit(2);
  }
  return args;
}

function log(msg) { console.log(`[clip-batch] ${msg}`); }

// ---------------------------------------------------------------------------
// Preflight
// ---------------------------------------------------------------------------
function newestMtime(dir, exts) {
  let newest = 0;
  let newestFile = "";
  const walk = (d) => {
    for (const e of readdirSync(d, { withFileTypes: true })) {
      const p = join(d, e.name);
      if (e.isDirectory()) walk(p);
      else if (exts.some((x) => e.name.toLowerCase().endsWith(x))) {
        const m = statSync(p).mtimeMs;
        if (m > newest) { newest = m; newestFile = p; }
      }
    }
  };
  walk(dir);
  return { newest, newestFile };
}

function preflight(clips, allowStale) {
  const fail = (msg) => { console.error(`[clip-batch] PREFLIGHT FAIL: ${msg}`); process.exit(2); };

  // 1. Exe + web bundle exist and are fresh (README stale-exe trap,:
  //    an exe older than the `scale` feature silently renders f04 at 1x).
  if (!existsSync(exePath)) fail(`missing ${exePath} — build x64 Release first`);
  if (!existsSync(distIndex)) fail(`missing ${distIndex} — build the web bundle first`);
  const exeM = statSync(exePath).mtimeMs;
  const distM = statSync(distIndex).mtimeMs;
  const srcNew = newestMtime(join(repoRoot, "src"), [".cpp", ".h"]);
  const webNew = newestMtime(join(repoRoot, "web", "apps", "editor", "src"), [".ts", ".tsx", ".css"]);
  const staleness = [];
  if (srcNew.newest > exeM) staleness.push(`exe older than ${srcNew.newestFile}`);
  if (webNew.newest > distM) staleness.push(`web dist older than ${webNew.newestFile}`);
  if (staleness.length) {
    if (allowStale) staleness.forEach((s) => log(`WARN (allow-stale): ${s}`));
    else fail(`${staleness.join("; ")} — rebuild or pass --allow-stale`);
  }

  // 2. Parse every timeline; collect out dirs; reject collisions.
  const seen = new Map();
  for (const clip of clips) {
    const tlPath = join(repoRoot, clip.timeline);
    if (!existsSync(tlPath)) fail(`${clip.name}: missing timeline ${clip.timeline}`);
    let tl;
    try { tl = JSON.parse(readFileSync(tlPath, "utf8")); }
    catch (e) { fail(`${clip.name}: timeline parse error: ${e.message}`); }
    if (!tl.out || typeof tl.out !== "string") fail(`${clip.name}: timeline has no "out"`);
    clip.out = tl.out;
    clip.frameCount = Math.round((tl.durationMs * tl.fps) / 1000);
    if (!Number.isFinite(clip.frameCount) || clip.frameCount < 1) fail(`${clip.name}: bad durationMs/fps`);
    const outAbs = resolve(repoRoot, tl.out);
    for (const key of [outAbs, `${outAbs}.tmp`]) {
      if (seen.has(key)) fail(`out-dir collision: ${clip.name} and ${seen.get(key)} both claim ${key}`);
      seen.set(key, clip.name);
    }
    if (resolve(repoRoot, clip.frames) !== outAbs)
      fail(`${clip.name}: table frames dir ${clip.frames} != timeline out ${tl.out}`);
    // Gate scripts referenced must exist.
    for (const g of clip.gates) if (!existsSync(join(repoRoot, g[0]))) fail(`${clip.name}: missing gate script ${g[0]}`);
  }

  // 3. Lockfile: one orchestrator at a time.
  mkdirSync(dirname(lockPath), { recursive: true });
  if (existsSync(lockPath)) {
    const holder = Number(readFileSync(lockPath, "utf8").trim());
    let alive = false;
    try { process.kill(holder, 0); alive = true; } catch { /* dead or invalid */ }
    if (alive) fail(`another render-all is running (PID ${holder}, ${lockPath})`);
    log(`stale lock from dead PID ${holder} — taking over`);
  }
  writeFileSync(lockPath, String(process.pid));
}

// ---------------------------------------------------------------------------
// Stages
// ---------------------------------------------------------------------------
function recordCmd(clip) { return [exePath, "--record", clip.timeline]; }
function partialScanCmd(clip) {
  const c = ["scripts/clip-verify/partial-scan.mjs", "--frames", clip.frames, "--start", String(clip.partialScan.start)];
  if (clip.partialScan.crop) c.push("--crop", clip.partialScan.crop);
  return c;
}

const liveChildren = new Set();

function runRecord(clip) {
  return new Promise((res) => {
    const budgetMs = clip.frameCount * 3000 + 60000;   // parent-side: the editor's own
    // watchdog can't fire while Tick() blocks inside a dispatch/capture (plan risk 14).
    const child = spawn(exePath, ["--record", clip.timeline], { cwd: repoRoot, stdio: "ignore" });
    liveChildren.add(child);
    clip.recordPid = child.pid;
    let done = false;
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      log(`${clip.name}: record exceeded ${Math.round(budgetMs / 1000)}s budget — killing tree ${child.pid}`);
      spawnSync("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" });
      liveChildren.delete(child);
      res({ code: "timeout" });
    }, budgetMs);
    child.on("exit", (code) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      liveChildren.delete(child);
      res({ code: code ?? 1 });
    });
    child.on("error", (e) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      liveChildren.delete(child);
      log(`${clip.name}: spawn error ${e.message}`);
      res({ code: 1 });
    });
  });
}

function readChildLog(pid) {
  if (!localAppData || !pid) return "";
  const p = join(localAppData, "AloParticleEditor", `host-record-${pid}.log`);
  try { return readFileSync(p, "utf8"); } catch { return ""; }
}

function runNode(cmdParts) {
  const r = spawnSync(process.execPath, cmdParts, { cwd: repoRoot, encoding: "utf8" });
  return { code: r.status ?? 1, out: `${r.stdout || ""}${r.stderr || ""}`.trim() };
}

async function runClip(clip) {
  const result = { name: clip.name, stages: {}, failure: null, rerun: `"${exePath}" --record ${clip.timeline}` };
  const t0 = Date.now();

  // record
  const rec = await runRecord(clip);
  result.stages.record = { ms: Date.now() - t0, code: rec.code };
  const logText = readChildLog(clip.recordPid);
  if (rec.code !== 0) {
    result.failure = `record:${classifyFailure(rec.code, logText)}`;
    return result;
  }
  // A missing child log silently disables the compositor-signature check —
  // say so loudly instead (LOCALAPPDATA unset, or the host was pointed at a
  // perf artifact dir; see HostWindowImpl::OpenLog).
  if (!logText)
    log(`${clip.name}: WARN no child log at host-record-${clip.recordPid}.log — compositor-signature check skipped`);
  const sigHit = LOG_FAIL_SIGNATURES.find((s) => logText.includes(s));
  if (sigHit) {
    result.failure = `record:compositor(${sigHit})`;   // plan risk 12
    return result;
  }

  // gates: always partial-scan first, then the clip's README gates
  const t1 = Date.now();
  for (const gate of [partialScanCmd(clip), ...clip.gates]) {
    const g = runNode(gate);
    if (g.code !== 0) {
      result.stages.gates = { ms: Date.now() - t1, code: g.code };
      result.failure = `gate:${gate[0].split("/").pop()}`;
      result.gateOutput = g.out.slice(0, 400);
      return result;
    }
  }
  result.stages.gates = { ms: Date.now() - t1, code: 0 };

  // encode
  const t2 = Date.now();
  const e = runNode(["scripts/clip-encode/encode.mjs", ...clip.encode]);
  result.stages.encode = { ms: Date.now() - t2, code: e.code };
  if (e.code !== 0) {
    result.failure = "encode";
    result.gateOutput = e.out.slice(0, 400);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Pool (inline bounded limiter — deliberately not a module, plan "Out" list)
// ---------------------------------------------------------------------------
async function runPool(items, jobs, fn) {
  const results = new Array(items.length);
  let next = 0;
  const worker = async () => {
    for (;;) {
      const i = next++;
      if (i >= items.length) return;
      results[i] = await fn(items[i]);
    }
  };
  await Promise.all(Array.from({ length: Math.min(jobs, items.length) }, worker));
  return results;
}

// ---------------------------------------------------------------------------
// Cleanup (Ctrl+C / fatal): kill child trees, drop known .tmp dirs, unlock.
// The editor only self-cleans the SAME clip's .tmp on its next run, so a
// canceled batch of large clips would strand GBs otherwise (plan risk 11).
// ---------------------------------------------------------------------------
function cleanup(clips) {
  for (const child of liveChildren) {
    try { spawnSync("taskkill", ["/pid", String(child.pid), "/T", "/F"], { stdio: "ignore" }); } catch { /* best effort */ }
  }
  liveChildren.clear();
  for (const clip of clips) {
    const tmp = resolve(repoRoot, `${clip.out ?? clip.frames}.tmp`);
    try { rmSync(tmp, { recursive: true, force: true }); } catch { /* best effort */ }
  }
  try { unlinkSync(lockPath); } catch { /* best effort */ }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
async function main() {
  const args = parseArgs(process.argv.slice(2));
  let clips = CLIPS;
  if (args.only) {
    const unknown = args.only.filter((n) => !CLIPS.some((c) => c.name === n));
    if (unknown.length) { console.error(`[clip-batch] unknown clip(s): ${unknown.join(", ")} (have: ${CLIPS.map((c) => c.name).join(", ")})`); process.exit(2); }
    clips = CLIPS.filter((c) => args.only.includes(c.name));
  }

  if (args.dryRun) {
    for (const clip of clips) {
      console.log(`# ${clip.name}`);
      console.log(`"${exePath}" --record ${clip.timeline}`);
      console.log(`node ${partialScanCmd(clip).join(" ")}`);
      for (const g of clip.gates) console.log(`node ${g.join(" ")}`);
      console.log(`node scripts/clip-encode/encode.mjs ${clip.encode.join(" ")}`);
      console.log("");
    }
    return;
  }

  preflight(clips, args.allowStale);
  process.on("SIGINT", () => { log("interrupted — cleaning up"); cleanup(clips); process.exit(130); });
  process.on("SIGTERM", () => { cleanup(clips); process.exit(143); });

  log(`rendering ${clips.length} clip(s), jobs=${args.jobs}`);
  const t0 = Date.now();
  let results;
  try {
    results = await runPool(clips, args.jobs, runClip);
  } finally {
    // Always release the lock; keep .tmp cleanup for the failure/interrupt path
    // only (a clean run has already renamed its .tmp away).
    try { unlinkSync(lockPath); } catch { /* released */ }
  }
  const wallS = ((Date.now() - t0) / 1000).toFixed(1);

  // Summary
  log(`done in ${wallS}s (jobs=${args.jobs})`);
  const pad = (s, n) => String(s).padEnd(n);
  console.log(pad("clip", 14) + pad("record", 10) + pad("gates", 9) + pad("encode", 9) + "status");
  let failed = 0;
  for (const r of results) {
    const st = (k) => (r.stages[k] ? `${(r.stages[k].ms / 1000).toFixed(1)}s` : "-");
    const status = r.failure ? `FAIL ${r.failure}` : "ok";
    if (r.failure) failed++;
    console.log(pad(r.name, 14) + pad(st("record"), 10) + pad(st("gates"), 9) + pad(st("encode"), 9) + status);
    if (r.failure) {
      console.log(`  rerun solo: ${r.rerun}`);
      if (r.gateOutput) console.log(`  output: ${r.gateOutput.split("\n").slice(0, 5).join("\n          ")}`);
    }
  }
  if (failed) {
    log(`${failed} clip(s) failed`);
    process.exit(1);
  }
}

main().catch((e) => { console.error(`[clip-batch] fatal: ${e.stack || e}`); cleanup(CLIPS); process.exit(1); });
