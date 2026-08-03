// headless-golden — fidelity gate for the headless `--record` capture path.
//
// Compares the LEGACY PrintWindow render against the HEADLESS CapturePreview +
// engine-RT composite on the COMMON CLIENT-CONTENT crop (native title bar +
// resize borders excluded — the two paths differ structurally there: legacy is
// the full window, headless is WebView client content only). Reports per-frame
// SSIM + per-pixel MAE/max across a sample of frames and gates on calibrated
// thresholds (Stage-1 fidelity acceptance, tasks/todo.md §6 / design §6).
//
// Default crops (green-color-edit @ 1280x960 window; empirically calibrated):
//   legacy 1280x960 -> crop 1264:921:8:31  (drop 8px side border + 31px title bar;
//                                            8px bottom border already outside 921)
//   headless 1264x921 -> crop 1264:921:0:0 (whole client)
// The y=31 offset was found by a peak scan (SSIM 0.995 at y=31 vs 0.75-0.86 at ±1px).
//
// Usage:
//   node scripts/clip-verify/headless-golden.mjs \
//     --legacy clips/t1-green-color-edit --headless clips/t1-green-color-edit-headless \
//     [--frames 60,300,600,...] [--legacy-crop 1264:921:8:39] [--headless-crop 1264:921:0:0] \
//     [--ssim-min 0.99] [--mae-max 3.0]
import { spawnSync } from "node:child_process";
import { mkdtempSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { ssim } from "../lib/ssim.mjs";

function arg(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : def;
}

const legacyDir = arg("legacy");
const headlessDir = arg("headless");
if (!legacyDir || !headlessDir) {
  console.error("usage: --legacy <dir> --headless <dir> [--frames a,b,c] [--legacy-crop WxH:X:Y] [--headless-crop ...] [--ssim-min N] [--mae-max N]");
  process.exit(2);
}
const legacyCrop = arg("legacy-crop", "1264:921:8:31");
const headlessCrop = arg("headless-crop", "1264:921:0:0");
const ssimMin = Number(arg("ssim-min", "0.99"));    // calibrated: measured min ~0.9949
const maeMax = Number(arg("mae-max", "0.5"));       // mean luma Δ; measured ~0.09
const frames = (arg("frames", "60,300,600,700,900,990,1200,1500,1800"))
  .split(",").map((s) => Number(s.trim())).filter((n) => Number.isFinite(n));

const tmp = mkdtempSync(join(tmpdir(), "headless-golden-"));
const pad = (n) => String(n).padStart(5, "0");

function crop(src, expr, out) {
  const r = spawnSync("ffmpeg", ["-y", "-hide_banner", "-loglevel", "error", "-i", src, "-vf", `crop=${expr}`, out], { encoding: "utf8" });
  return r.status === 0;
}

// Per-pixel luma diff via ffmpeg blend=difference + signalstats: YAVG = mean
// abs luma diff (the robust metric — a single AA/particle-edge pixel shouldn't
// gate), YMAX = worst single-pixel diff (reported, not gated).
function pixelStats(a, b) {
  const r = spawnSync("ffmpeg", ["-hide_banner", "-i", a, "-i", b, "-filter_complex", "blend=all_mode=difference,signalstats,metadata=print:file=-", "-f", "null", "-"], { encoding: "utf8" });
  const text = `${r.stderr || ""}${r.stdout || ""}`;
  const yavg = text.match(/signalstats\.YAVG=([0-9.]+)/);
  const ymax = text.match(/signalstats\.YMAX=([0-9.]+)/);
  return { yavg: yavg ? Number(yavg[1]) : NaN, ymax: ymax ? Number(ymax[1]) : NaN };
}

let minSsim = 1, sumSsim = 0, maxYavg = 0, worstYmax = 0, n = 0, fails = 0;
console.log(`frame     SSIM      meanLumaΔ  maxLumaΔ`);
for (const f of frames) {
  const lg = `${legacyDir}/frame_${pad(f)}.png`;
  const hd = `${headlessDir}/frame_${pad(f)}.png`;
  if (!existsSync(lg) || !existsSync(hd)) { console.log(`${String(f).padStart(5)}  MISSING`); fails++; continue; }
  const lgc = join(tmp, `lg_${f}.png`);
  const hdc = join(tmp, `hd_${f}.png`);
  if (!crop(lg, legacyCrop, lgc) || !crop(hd, headlessCrop, hdc)) { console.log(`${String(f).padStart(5)}  CROP-FAIL`); fails++; continue; }
  const s = ssim(lgc, hdc);
  if (!s.ok) { console.log(`${String(f).padStart(5)}  SSIM-FAIL ${s.why}`); fails++; continue; }
  const px = pixelStats(lgc, hdc);
  console.log(`${String(f).padStart(5)}   ${s.all.toFixed(6)}  ${px.yavg.toFixed(4).padStart(8)}   ${String(px.ymax).padStart(6)}`);
  minSsim = Math.min(minSsim, s.all); sumSsim += s.all;
  maxYavg = Math.max(maxYavg, px.yavg); worstYmax = Math.max(worstYmax, px.ymax); n++;
}
const avgSsim = n ? sumSsim / n : 0;
console.log(`\nframes=${n} fails=${fails}  min_SSIM=${minSsim.toFixed(6)} avg_SSIM=${avgSsim.toFixed(6)}  max_meanLumaΔ=${maxYavg.toFixed(4)} worst_maxLumaΔ=${worstYmax}`);
console.log(`gate: SSIM_MIN=${ssimMin} meanLumaΔ_MAX=${maeMax}`);
const pass = fails === 0 && minSsim >= ssimMin && maxYavg <= maeMax;
console.log(pass ? "RESULT: PASS" : "RESULT: FAIL");
process.exit(pass ? 0 : 1);
