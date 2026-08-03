#!/usr/bin/env node
// Crossfade-seam churn gate for --record clips. A crossfade loop blends the last
// C seconds (tail) into the first C seconds (lead-in). For a looping but
// non-frame-periodic effect (e.g. the Faith AT-ST death burst), the dissolve only
// reads clean if each paired lead/tail frame is statistically alike over the
// effect's region. This compares every paired lead/tail frame's ROI per-pixel
// (mean abs luma via ffmpeg) and fails if the max ratio exceeds --max-ratio.
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

const USAGE =
  "usage: node seam-churn.mjs --frames <dir> --fps <n> --start <n> --crossfade-frames <n> --roi W:H:X:Y [--max-ratio <0..1>]";

export function parseRoi(str) {
  const p = String(str).split(":");
  if (p.length !== 4) throw new Error(`invalid roi "${str}": expected W:H:X:Y`);
  const [w, h, x, y] = p.map(Number);
  if (![w, h, x, y].every(Number.isFinite)) throw new Error(`invalid roi "${str}": values must be numbers`);
  return { w, h, x, y };
}

// ffmpeg signalstats YAVG of a difference frame is a mean abs luma over 0..255.
export function churnRatio(meanAbsLuma) {
  return Number(meanAbsLuma) / 255;
}

export function churnVerdict(ratio, maxRatio) {
  return { ok: Number(ratio) <= Number(maxRatio), ratio: Number(ratio), maxRatio: Number(maxRatio) };
}

// Mean abs luma diff of two PNGs over an ROI, via ffmpeg blend=difference + signalstats.
function pairChurn(frameA, frameB, roi) {
  const vf =
    `[0:v]crop=${roi.w}:${roi.h}:${roi.x}:${roi.y}[a];` +
    `[1:v]crop=${roi.w}:${roi.h}:${roi.x}:${roi.y}[b];` +
    `[a][b]blend=all_mode=difference,signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-`;
  const r = spawnSync("ffmpeg", ["-hide_banner", "-loglevel", "error", "-i", frameA, "-i", frameB,
    "-filter_complex", vf, "-frames:v", "1", "-f", "null", "-"], { encoding: "utf8" });
  const m = /YAVG=([0-9.]+)/.exec(`${r.stdout}${r.stderr}`);
  if (!m) throw new Error(`ffmpeg produced no YAVG for ${frameA} vs ${frameB}`);
  return Number(m[1]);
}

function frameName(dir, n) {
  return join(dir, `frame_${String(n).padStart(5, "0")}.png`);
}

function main(argv) {
  const a = {};
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--help") { console.log(USAGE); return 0; }
    const key = argv[i].slice(2); a[key] = argv[++i];
  }
  const dir = a.frames, start = Number(a.start);
  const cf = Number(a["crossfade-frames"]), maxRatio = a["max-ratio"] == null ? 0.10 : Number(a["max-ratio"]);
  if (!dir || !existsSync(dir)) { console.error(USAGE); return 2; }
  const roi = parseRoi(a.roi);
  const total = readdirSync(dir).filter((f) => /^frame_\d+\.png$/.test(f)).length;
  const keptCount = total - start;
  let max = 0, sum = 0, n = 0;
  for (let k = 0; k < cf; k++) {
    const lead = frameName(dir, start + k);
    const tail = frameName(dir, start + (keptCount - cf) + k);
    if (!existsSync(lead) || !existsSync(tail)) continue;
    const ratio = churnRatio(pairChurn(lead, tail, roi));
    max = Math.max(max, ratio); sum += ratio; n++;
  }
  const mean = sum / Math.max(1, n);
  const v = churnVerdict(max, maxRatio);
  console.log(`seam-churn: max=${max.toFixed(4)} mean=${mean.toFixed(4)} over ${n} pairs (roi ${a.roi}) max-ratio=${maxRatio}`);
  if (!v.ok) { console.error(`seam-churn FAIL — max ${max.toFixed(4)} > ${maxRatio}`); return 1; }
  console.log("seam-churn OK");
  return 0;
}

// Cross-platform main-guard: pathToFileURL handles Windows backslash/drive paths
// (a bare `file://${argv[1]}` never matches import.meta.url on Windows).
if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  process.exit(main(process.argv.slice(2)));
}
