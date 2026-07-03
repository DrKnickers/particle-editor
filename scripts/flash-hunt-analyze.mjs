// Analyze a screen recording for white-flash frames (the intermittent cross-page
// view-transition flash on the landing/guide site). Companion to flash-hunt.ps1.
//
//   node scripts/flash-hunt-analyze.mjs <capture.mkv> [--delta 40] [--out <dir>] [--crop W:H:X:Y]
//
// --crop confines measurement to a region (ffmpeg crop syntax W:H:X:Y). Use it to measure an
// always-dark strip of the page (e.g. the left gutter) so bright page CONTENT — the landing
// hero clip's fireball — can't trigger false luma spikes; a real flash covers the whole
// viewport including the gutter. Extracted PNGs are always the FULL frame for diagnosis.
//
// Method: per-frame average luma (ffmpeg signalstats YAVG — the same measurement idiom the
// clip pipeline's seam-match uses). The site is near-black (#0b0d12), so a white flash on a
// mostly-browser screen spikes YAVG far above the rolling baseline. We flag frames whose
// YAVG exceeds the recording's median by --delta (default 40 — a full-viewport flash jumps
// ~100+, normal clip playback wobbles ±15), group consecutive flagged frames into EVENTS,
// and extract each event's peak frame as a PNG so the flash's actual shape (full canvas?
// content region? scrollbar band?) identifies the source.
//
// Exit codes: 0 = no flash events, 2 = events found (frames written), 1 = usage/ffmpeg error.
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";

const args = process.argv.slice(2);
const input = args.find((a) => !a.startsWith("--"));
const delta = Number(args[args.indexOf("--delta") + 1] || 40) || 40;
const outDir = args.includes("--out") ? args[args.indexOf("--out") + 1] : path.join(path.dirname(input || "."), "flash-frames");
const crop = args.includes("--crop") ? args[args.indexOf("--crop") + 1] : null;
if (!input || !fs.existsSync(input) || (crop && !/^\d+:\d+:\d+:\d+$/.test(crop))) {
  console.error("usage: node scripts/flash-hunt-analyze.mjs <capture.mkv> [--delta N] [--out dir] [--crop W:H:X:Y]");
  process.exit(1);
}

// Per-frame YAVG via signalstats + metadata=print to stdout (avoids lavfi movie-filter
// path escaping on Windows). Output alternates: "frame:N pts:... pts_time:T" / "...YAVG=V".
const vf = (crop ? `crop=${crop},` : "") + "signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-";
const probe = spawnSync("ffmpeg", [
  "-hide_banner", "-nostats", "-i", input,
  "-vf", vf,
  "-f", "null", "-",
], { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 });
if (probe.status !== 0) { console.error("ffmpeg failed:", (probe.stderr || "").slice(-800)); process.exit(1); }

const frames = [];
let pending = null;
for (const line of probe.stdout.split(/\r?\n/)) {
  const ft = line.match(/^frame:\d+\s+pts:\S+\s+pts_time:([\d.]+)/);
  if (ft) { pending = Number(ft[1]); continue; }
  const yv = line.match(/lavfi\.signalstats\.YAVG=([\d.]+)/);
  if (yv && pending !== null) { frames.push({ t: pending, yavg: Number(yv[1]) }); pending = null; }
}
if (frames.length < 10) { console.error(`only ${frames.length} frames parsed — capture too short or unreadable`); process.exit(1); }

const sorted = frames.map((f) => f.yavg).sort((a, b) => a - b);
const median = sorted[Math.floor(sorted.length / 2)];
const threshold = median + delta;

// Group flagged frames into events. Deliberate tolerance: flagged frames <0.1s apart merge
// into ONE event even if dark frames sit between them — a flicker that dips dark mid-flash
// is one perceptual event, and the count stays comparable across runs.
const events = [];
let cur = null;
for (const f of frames) {
  if (f.yavg > threshold) {
    if (cur && f.t - cur.end < 0.1) { cur.end = f.t; cur.frames++; if (f.yavg > cur.peak) { cur.peak = f.yavg; cur.peakT = f.t; } }
    else { cur = { start: f.t, end: f.t, frames: 1, peak: f.yavg, peakT: f.t }; events.push(cur); }
  }
}

const top = [...frames].sort((a, b) => b.yavg - a.yavg).slice(0, 5);
console.log(`frames analyzed : ${frames.length} (${frames[frames.length - 1].t.toFixed(1)}s)${crop ? `  [measuring crop ${crop}]` : ""}`);
console.log(`baseline YAVG   : median ${median.toFixed(1)}  (flag threshold ${threshold.toFixed(1)} = median + ${delta})`);
console.log(`top-5 YAVG      : ${top.map((f) => `${f.yavg.toFixed(0)}@${f.t.toFixed(2)}s`).join("  ")}`);

if (!events.length) { console.log("flash events    : NONE — no frame exceeded the threshold"); process.exit(0); }

fs.mkdirSync(outDir, { recursive: true });
console.log(`flash events    : ${events.length}`);
events.slice(0, 10).forEach((e, i) => {
  const png = path.join(outDir, `flash-${String(i + 1).padStart(2, "0")}-at-${e.peakT.toFixed(2)}s.png`);
  const ex = spawnSync("ffmpeg", ["-hide_banner", "-loglevel", "error", "-ss", String(e.peakT), "-i", input, "-frames:v", "1", "-y", png], { encoding: "utf8" });
  const saved = ex.status === 0 && fs.existsSync(png);
  console.log(`  #${i + 1}: t=${e.start.toFixed(2)}–${e.end.toFixed(2)}s  ${e.frames} frame(s)  peak YAVG ${e.peak.toFixed(0)}${saved ? `  → ${png}` : "  (frame extract failed)"}`);
});
if (events.length > 10) console.log(`  … ${events.length - 10} more events not extracted`);
process.exit(2);
