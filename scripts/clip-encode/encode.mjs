#!/usr/bin/env node
// Encode a numbered PNG sequence (frame_%05d.png) into a looping, web-safe H.264
// mp4 + a poster. The output invariants are LOAD-BEARING: format=yuv420p (else
// black on Safari/iOS), even dims (libx264 4:2:0), +faststart (plays before full
// download), -an (silent). Usage:
//   node encode.mjs --frames <dir> --fps <n> --out <clip.mp4> [--poster <p.jpg>]
//     [--start <n>]              first frame number to read (trim pre-roll head)
//     [--loop pingpong]          forward+reverse so the clip loops seamlessly
//     [--loop crossfade]         dissolve the tail back over the lead-in (for a
//                                one-shot effect that never returns to black);
//                                the source MUST open with --crossfade seconds
//                                of effect-free lead-in (delay the trigger) so
//                                the dissolve lands on empty background, not the
//                                next ignition.
//     [--crossfade <sec>]        crossfade length for --loop crossfade (def 1.0)
//     [--crf <n>]                x264 quality (def 20; lower = better). Bright
//                                fine particles on black flicker at the keyframe
//                                pulse — use ~16 for explosion/spark clips.
//     [--drop-black-below <luma>] drop frames whose whole-frame avg luma (YAVG)
//                                 is at/below <luma> — the capture-race guard:
//                                 --record's PrintWindow can grab the window
//                                 mid-composite, blacking the viewport for a
//                                 stray frame; pick a threshold between the UI
//                                 chrome's luma and the real content's (~35 for
//                                 the editor's dark theme).
//     [--poster-frame <n>]       frame number for the poster (default --start)
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

// Compose the per-frame filter chain shared by the -vf and -filter_complex paths.
// `pre` runs before any loop construction (per-source-frame); `post` carries the
// even-dims + yuv420p invariants and runs last on the final stream.
// Validate a crop "W:H:X:Y" (four non-negative ints) before it reaches the ffmpeg
// filtergraph — an unvalidated string would let an extra filter be injected
// (e.g. "1264:952:8:0,transpose=1"). Throws on anything else; null is a no-op.
function assertCrop(crop) {
  if (crop != null && !/^\d+:\d+:\d+:\d+$/.test(crop)) {
    throw new Error(`--crop must be W:H:X:Y (non-negative integers), got "${crop}"`);
  }
}

function buildFilterParts({ fps, dropBlackBelow, crop }) {
  const pre = [];
  if (dropBlackBelow != null) {
    // Measure per-frame luma, keep only frames brighter than the threshold, then
    // renumber timestamps so the surviving frames play at an even cadence.
    pre.push("signalstats");
    pre.push(`metadata=mode=select:key=lavfi.signalstats.YAVG:value=${dropBlackBelow}:function=greater`);
    pre.push(`setpts=N/${fps}/TB`);
  }
  // crop=W:H:X:Y trims the captured window's non-content chrome (e.g. the Win11
  // phantom ~8px resize border GetWindowRect/PrintWindow includes around the editor
  // — crop=1264:952:8:0 keeps the title bar, drops the black L/R/B border). Runs
  // first so the even-dims + yuv420p invariants apply to the trimmed frame.
  const cropPart = crop ? `crop=${crop},` : "";
  const post = `${cropPart}scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p`;
  return { pre, post };
}

export function buildFfmpegArgs({ framesDir, fps, out, start = 0, loop = "none",
                                  dropBlackBelow = null, crossfadeSec = 1.0, frameCount = null, crf = 20,
                                  crop = null }) {
  assertCrop(crop);
  const args = ["-y", "-framerate", String(fps)];
  if (start) args.push("-start_number", String(start));
  args.push("-i", join(framesDir, "frame_%05d.png"));

  const { pre, post } = buildFilterParts({ fps, dropBlackBelow, crop });
  const preChain = pre.length ? `${pre.join(",")},` : "";

  if (loop === "pingpong") {
    // forward → reverse(minus the duplicated turnaround frame) → concat, so the
    // clip plays out-and-back and loops without a hard cut. Labels require
    // -filter_complex (an -vf graph can't name pads).
    const fc =
      `[0:v]${preChain}split[fwd][tmp];` +
      `[tmp]reverse,trim=start_frame=1,setpts=PTS-STARTPTS[rev];` +
      `[fwd][rev]concat=n=2:v=1,${post}[v]`;
    args.push("-filter_complex", fc, "-map", "[v]");
  } else if (loop === "crossfade") {
    // Play the body, then crossfade the faint tail back over the lead-in so the
    // clip dissolves into its own start — a seamless loop for a one-shot effect
    // that never returns to a black frame. The xfade lands at offset = D-2C, so
    // the last C seconds dissolve into the first C seconds; the source MUST open
    // with C seconds of effect-free lead-in (delay the trigger) or the dissolve
    // shows the next ignition. Needs frameCount to derive D.
    if (!frameCount || frameCount <= 0) {
      throw new Error("loop=crossfade requires a positive frameCount");
    }
    // drop-black removes frames before the split, so the actual stream is
    // shorter than frameCount → the offset (derived from frameCount) would land
    // the xfade past the end. The two are for different scene types anyway
    // (drop-black = bright scene w/ full-black race frames; crossfade = dark
    // one-shot), so reject the combination rather than guess the post-drop count.
    if (dropBlackBelow != null) {
      throw new Error("loop=crossfade cannot combine with drop-black-below (dropped frames change the stream length the offset is derived from)");
    }
    const C = crossfadeSec;
    const D = frameCount / Number(fps);
    // xfade offset = D - 2C must be ≥ 0, i.e. the clip must be longer than two
    // crossfades, or ffmpeg gets a negative offset and emits corrupt output.
    if (!(D > 2 * C)) {
      throw new Error(`loop=crossfade needs clip duration (${D.toFixed(2)}s) > 2× crossfade (${(2 * C).toFixed(2)}s)`);
    }
    const offset = (D - 2 * C).toFixed(4);
    const fc =
      `[0:v]${preChain}split[main][pre];` +
      `[pre]trim=duration=${C},setpts=PTS-STARTPTS[pre];` +
      `[main]trim=start=${C},setpts=PTS-STARTPTS[main];` +
      `[main][pre]xfade=transition=fade:duration=${C}:offset=${offset},${post}[v]`;
    args.push("-filter_complex", fc, "-map", "[v]");
  } else {
    args.push("-vf", [...pre, post].join(","));
  }

  args.push("-c:v", "libx264", "-crf", String(crf), "-preset", "slow");
  // Tag an explicit, consistent color MATRIX. Without this ffmpeg leaves
  // color_space UNSET → "unknown", which forces players to guess the YUV→RGB
  // coefficients: a hardware-overlay path and a composited path can guess
  // differently and toggle between them, flickering the frame even when the video
  // is PAUSED. Tag the matrix + primaries bt709 + limited (tv) range. We do NOT
  // force -color_trc: the PNG source is sRGB (iec61966-2-1), already a DEFINED
  // transfer, so tagging it bt709 would mislabel the curve we never converted to —
  // and the flicker was the unknown matrix, not the transfer. LOAD-BEARING.
  args.push("-colorspace", "bt709", "-color_primaries", "bt709", "-color_range", "tv");
  // After dropping frames (or building a loop) the source PTS are gappy/rebuilt —
  // pin the output cadence so playback timing is exact.
  if (dropBlackBelow != null || loop === "pingpong" || loop === "crossfade") {
    args.push("-r", String(fps));
  }
  args.push("-movflags", "+faststart", "-an", out);
  return args;
}

// Count CONTIGUOUS frame_NNNNN.png from `start` — the image2 demuxer with
// -start_number stops at the first gap, so a glob/readdir count would
// overestimate (a hole would inflate D and push the xfade offset past the end).
export function countFrames(framesDir, start = 0) {
  let n = 0;
  while (existsSync(join(framesDir, `frame_${String(start + n).padStart(5, "0")}.png`))) n++;
  return n;
}

export function buildPosterArgs({ framesDir, poster, posterFrame = 0, crop = null }) {
  assertCrop(crop);
  const name = `frame_${String(posterFrame).padStart(5, "0")}.png`;
  const args = ["-y", "-i", join(framesDir, name)];
  if (crop) args.push("-vf", `crop=${crop}`); // match the video's chrome trim
  args.push("-frames:v", "1", poster);
  return args;
}

function run(bin, args) {
  const r = spawnSync(bin, args, { stdio: "inherit" });
  if (r.status !== 0) {
    console.error(`${bin} failed (status ${r.status})`);
    process.exit(1);
  }
}

function parseArgs(argv) {
  const a = {};
  for (let i = 0; i < argv.length; i += 2) {
    const key = argv[i].replace(/^--/, "");
    const val = argv[i + 1];
    // A missing value, or a value that is itself a flag (`--crf --poster x`),
    // means the option was given without an argument — fail loud rather than
    // silently swallowing the next flag and emitting `-crf NaN` to ffmpeg.
    if (val === undefined || /^--/.test(val)) {
      console.error(`option --${key} requires a value`);
      process.exit(2);
    }
    a[key] = val;
  }
  return a;
}

// Parse a numeric CLI option, failing loud on a non-finite value (`--crf abc`
// → NaN, which some ffmpeg builds accept silently). Returns `def` when absent.
function numOpt(a, name, def) {
  if (a[name] == null) return def;
  const v = Number(a[name]);
  if (!Number.isFinite(v)) {
    console.error(`--${name} must be a number, got "${a[name]}"`);
    process.exit(2);
  }
  return v;
}

// Run only when invoked directly (not when imported by the test).
if (process.argv[1] && process.argv[1].endsWith("encode.mjs")) {
  const a = parseArgs(process.argv.slice(2));
  if (!a.frames || !a.fps || !a.out) {
    console.error("usage: node encode.mjs --frames <dir> --fps <n> --out <clip.mp4> [--poster <p.jpg>]\n" +
                  "  [--start <n>] [--loop pingpong|crossfade] [--crossfade <sec>]\n" +
                  "  [--crf <n>] [--drop-black-below <luma>] [--poster-frame <n>] [--crop W:H:X:Y]");
    process.exit(2);
  }
  if (!existsSync(a.frames)) {
    console.error(`frames dir not found: ${a.frames}`);
    process.exit(2);
  }
  const start = numOpt(a, "start", 0);
  const loop = a.loop || "none";
  run("ffmpeg", buildFfmpegArgs({
    framesDir: a.frames, fps: a.fps, out: a.out,
    start,
    loop,
    dropBlackBelow: a["drop-black-below"] != null ? numOpt(a, "drop-black-below", null) : null,
    crossfadeSec: numOpt(a, "crossfade", 1.0),
    frameCount: loop === "crossfade" ? countFrames(a.frames, start) : null,
    crf: numOpt(a, "crf", 20),
    crop: a.crop ?? null,
  }));
  if (a.poster) {
    const posterFrame = numOpt(a, "poster-frame", start);
    run("ffmpeg", buildPosterArgs({ framesDir: a.frames, poster: a.poster, posterFrame, crop: a.crop ?? null }));
  }
  console.log(`encoded ${a.out}`);
}
