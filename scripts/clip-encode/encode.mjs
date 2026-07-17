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

// --- dynamic zoom (encode-time, per-clip segments) ---------------------------
//
// zoom = { w, h, segments: [{ t0, t1, rect: [x, y, w, h], easeMs }] }
//   w/h    : the post-chrome-trim frame size the segments are authored against
//            (== the static crop's W:H, or the raw frame size when no crop).
//   t0/t1  : SOURCE seconds — t0 is when the ease-IN begins, t1 when the
//            ease-OUT completes (full zoom is held in between).
//   rect   : target region in post-trim pixels; MUST match the frame aspect
//            (the zoom is a magnification, not a reframe).
//
// ffmpeg mechanics (spiked 2026-07-05): `crop` evaluates w/h ONCE at config —
// they cannot animate — and its `iw` freezes at the link's config-time size, so
// x/y must be pure functions of `n`. The working construct is an animated
// magnification (`scale` with eval=frame; its expressions DO accept `n`)
// followed by a STATIC-size crop whose x/y track the scaled-up target center:
//   scale=w='trunc(W*Z(n)/2)*2':h='trunc(H*Z(n)/2)*2':eval=frame,
//   crop=W:H:x='clip(cx(n)*Z(n)-W/2, 0, W*Z(n)-W)':y=…
// Z(n) eases 1→Zs→1 per segment (smoothstep), cx/cy ease frame-center→rect-center.
// Frame numbers are the POST-start-trim stream indices: n = round(t*fps) - start.
export function assertZoom(zoom, { loop = "none", dropBlackBelow = null, fps, start = 0, frameCount = null } = {}) {
  if (zoom == null) return;
  if (loop !== "none") throw new Error("zoom requires loop=none (loop modes reorder frames; n-based zoom expressions would land on the wrong frames)");
  if (dropBlackBelow != null) throw new Error("zoom cannot combine with drop-black-below (dropped frames renumber the stream)");
  const { w, h, segments } = zoom;
  // Even dims required: the magnification stage floors to even parity
  // (trunc(w*Z/2)*2) so libx264 4:2:0 stays satisfied at every eased Z,
  // including Z=1 at a segment's rest endpoints. An odd w/h would floor
  // BELOW the static crop size at Z=1 and the crop could never fit.
  if (!Number.isInteger(w) || !Number.isInteger(h) || w <= 0 || h <= 0 || w % 2 !== 0 || h % 2 !== 0) throw new Error("zoom.w/h must be positive EVEN integers (the post-trim frame size)");
  if (!Array.isArray(segments) || segments.length === 0) throw new Error("zoom.segments must be a non-empty array");
  if (segments.length > 8) throw new Error("zoom supports at most 8 segments (expression length)");
  let prevEnd = -Infinity;
  for (const s of segments) {
    const { t0, t1, rect, easeMs } = s;
    if (!Number.isFinite(t0) || !Number.isFinite(t1) || t1 <= t0) throw new Error(`zoom segment t0/t1 invalid: ${JSON.stringify(s)}`);
    if (!Number.isFinite(easeMs) || easeMs <= 0) throw new Error("zoom segment easeMs must be > 0");
    // The ease is serialized into the ffmpeg expression as E.toFixed(3) FRAMES
    // (buildZoomFilters). An ease under one frame is meaningless as a ramp, and a
    // sub-millifame one rounds to "0.000" -> the rise becomes clip(x/0,0,1), which
    // is non-finite: the scale expression then has no defined first-frame size and
    // the JS model (full precision) and ffmpeg (rounded) disagree. Reject it here.
    const easeFrames = (easeMs / 1000) * fps;
    if (easeFrames < 1) throw new Error(`zoom segment easeMs=${easeMs} is under one frame at ${fps}fps (${easeFrames.toFixed(4)} frames): the ease serializes to 0.000 and divides by zero`);
    const ease = easeMs / 1000;
    // Strictly greater: at exactly 2*ease the rise and fall ramps meet with
    // zero hold time, which is a degenerate (not just minimal) segment.
    if (t1 - t0 <= 2 * ease) throw new Error(`zoom segment shorter than its two eases: ${JSON.stringify(s)}`);
    if (!Array.isArray(rect) || rect.length !== 4 || !rect.every(Number.isFinite)) throw new Error("zoom segment rect must be [x, y, w, h]");
    const [rx, ry, rw, rh] = rect;
    if (rw <= 0 || rh <= 0 || rx < 0 || ry < 0 || rx + rw > w || ry + rh > h) throw new Error(`zoom rect out of ${w}x${h} bounds: ${JSON.stringify(rect)}`);
    // aspect must match: the construct magnifies, it does not letterbox.
    if (Math.abs(rw / rh - w / h) > 0.005 * (w / h)) throw new Error(`zoom rect aspect ${rw}x${rh} != frame aspect ${w}x${h}`);
    if (t0 < prevEnd) throw new Error("zoom segments must be sorted and non-overlapping");
    prevEnd = t1;
    const n0 = Math.round(t0 * fps) - start;
    if (n0 < 0) throw new Error(`zoom segment starts before the --start trim (t0=${t0}s, start frame ${start})`);
    // A segment must FINISH inside the stream. The profile is 0 (=> Z=1) only at
    // n >= D; if D lands past the last frame the clip ends mid-zoom while frame 0
    // is un-zoomed, so a looping <video> hard-cuts back to wide on every repeat.
    // The 1->Zs->1 shape is the loop-safety invariant — this is what enforces it.
    if (frameCount != null) {
      const n1 = Math.round(t1 * fps) - start;
      if (n1 > frameCount - 1)
        throw new Error(`zoom segment ends past the last frame (t1=${t1}s -> frame ${n1}, last frame ${frameCount - 1}): ` +
                        `the ease-out would never complete and the clip would end mid-zoom, breaking the loop`);
    }
  }
}

// Effective zoom state at one frame — the same Z/CX/CY the ffmpeg expressions
// compute, evaluated in JS. Used for the poster (a single-image ffmpeg run has
// n=0, so the animated expressions would always collapse to the Z=1 rest state)
// and to pin the centring invariant in tests.
export function evalZoomAt(zoom, { fps, start = 0, n }) {
  if (zoom == null) return null;
  const { w, h, segments } = zoom;
  let z = 1, cx = w / 2, cy = h / 2;
  for (const s of segments) {
    const E = (s.easeMs / 1000) * fps;
    const A = Math.round(s.t0 * fps) - start;
    const D = Math.round(s.t1 * fps) - start;
    const cl = (v) => Math.min(1, Math.max(0, v));
    const ss = (v) => v * v * (3 - 2 * v);
    const P = ss(cl((n - A) / E)) * ss(cl((D - n) / E));
    z += (w / s.rect[2] - 1) * P;
    cx += (s.rect[0] + s.rect[2] / 2 - w / 2) * P;
    cy += (s.rect[1] + s.rect[3] / 2 - h / 2) * P;
  }
  return { z, cx, cy };
}

// Smoothstep-eased 0→1→0 profile for one segment, as an ffmpeg expression in `n`:
// rise over [A, A+E], hold, fall over [D-E, D]. `clip()` bounds each ramp.
function segProfile(A, D, E, nvar = "n") {
  const rise = `clip((${nvar}-${A})/${E},0,1)`;
  const fall = `clip((${D}-${nvar})/${E},0,1)`;
  // smoothstep(u) = u*u*(3-2u), applied to both ramps (their product is the hold-aware profile)
  return `(pow(${rise},2)*(3-2*${rise}))*(pow(${fall},2)*(3-2*${fall}))`;
}

// SCALE_N_LEAD — scale (eval=frame) and crop DISAGREE about `n`.
//
// Probed on ffmpeg 8.1.1 (see the "zoom: scale/crop agree on the frame index"
// test, which re-probes it so a version change fails loudly rather than silently
// re-desyncing): on the FIRST frame `scale` sees n=1 while `crop` sees n=0.
// (Their `t` is inconsistent the other way — scale's t is 0 there, crop's is not
// — so neither variable is trustworthy across the pair on its own.)
//
// The two stages are one construct: crop's x/y offset is only correct for the
// magnification scale ACTUALLY applied to that same frame. Left uncorrected they
// describe different frames — at frame 0 scale magnifies by Z(1) while crop
// offsets for Z(0) — so the frame opens ~1px shifted and slightly zoomed instead
// of at rest, and every eased frame carries a one-frame wobble.
//
// This is invisible for a segment that starts late (t0 >= ~1.8s: frame 0 is at
// P=0 either way), which is why it survived unnoticed in the shipped clips. It is
// NOT invisible for a segment with t0=0: the last frame rests at Z=1 but the
// first does not, so the 1->Zs->1 loop invariant breaks and a looping <video>
// snaps on every repeat. Feed scale n-1 so both stages describe the same frame.
const SCALE_N_LEAD = 1;

export function buildZoomFilters({ zoom, fps, start = 0 }) {
  if (zoom == null) return [];
  const { w, h, segments } = zoom;
  // Z/CX/CY as expressions over a caller-chosen frame-index expression.
  const exprs = (nvar) => {
    const zTerms = [];   // Σ (Zs-1)*P_i
    const cxTerms = [];  // Σ (rcx - W/2)*P_i
    const cyTerms = [];
    for (const s of segments) {
      const easeF = (s.easeMs / 1000) * fps;
      const A = (Math.round(s.t0 * fps) - start).toFixed(0);
      const D = (Math.round(s.t1 * fps) - start).toFixed(0);
      const E = easeF.toFixed(3);
      const P = segProfile(A, D, E, nvar);
      const Zs = w / s.rect[2];
      const rcx = s.rect[0] + s.rect[2] / 2;
      const rcy = s.rect[1] + s.rect[3] / 2;
      zTerms.push(`(${(Zs - 1).toFixed(6)})*${P}`);
      cxTerms.push(`(${(rcx - w / 2).toFixed(3)})*${P}`);
      cyTerms.push(`(${(rcy - h / 2).toFixed(3)})*${P}`);
    }
    return {
      Z:  `(1+${zTerms.join("+")})`,
      CX: `(${w / 2}+${cxTerms.join("+")})`,
      CY: `(${h / 2}+${cyTerms.join("+")})`,
    };
  };
  const sc = exprs(SCALE_N_LEAD ? `(n-${SCALE_N_LEAD})` : "n");
  const cr = exprs("n");
  // scale to the magnified size (even dims), then crop the static W:H window
  // centered on the eased target center, clamped inside the scaled frame.
  const scaleF = `scale=w='trunc(${w}*${sc.Z}/2)*2':h='trunc(${h}*${sc.Z}/2)*2':eval=frame:flags=lanczos`;
  const cropF = `crop=${w}:${h}:x='clip(${cr.CX}*${cr.Z}-${w / 2},0,${w}*${cr.Z}-${w})':y='clip(${cr.CY}*${cr.Z}-${h / 2},0,${h}*${cr.Z}-${h})'`;
  return [scaleF, cropF];
}

function buildFilterParts({ fps, dropBlackBelow, crop, zoom = null, start = 0 }) {
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
  // Dynamic zoom operates in the POST-trim space, before the final invariants.
  const zoomPart = zoom ? `${buildZoomFilters({ zoom, fps, start }).join(",")},` : "";
  const post = `${cropPart}${zoomPart}scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p`;
  return { pre, post };
}

export function buildFfmpegArgs({ framesDir, fps, out, start = 0, loop = "none",
                                  dropBlackBelow = null, crossfadeSec = 1.0, frameCount = null, crf = 20,
                                  crop = null, zoom = null }) {
  assertCrop(crop);
  // Derive the count ourselves rather than trust the caller: the loop-safety bound
  // (a segment must finish inside the stream) is only enforced when frameCount is
  // known, so a caller that omits it would silently opt out of the ONE check that
  // keeps a looping <video> from snapping. Skipped when the dir has no frames yet
  // (arg-building tests, and a missing dir fails at ffmpeg anyway).
  if (zoom != null && frameCount == null) {
    const counted = countFrames(framesDir, start);
    if (counted > 0) frameCount = counted;
  }
  assertZoom(zoom, { loop, dropBlackBelow, fps: Number(fps), start, frameCount });
  const args = ["-y", "-framerate", String(fps)];
  if (start) args.push("-start_number", String(start));
  args.push("-i", join(framesDir, "frame_%05d.png"));

  const { pre, post } = buildFilterParts({ fps: Number(fps), dropBlackBelow, crop, zoom, start });
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

export function buildPosterArgs({ framesDir, poster, posterFrame = 0, crop = null, zoom = null, fps = null, start = 0 }) {
  assertCrop(crop);
  const name = `frame_${String(posterFrame).padStart(5, "0")}.png`;
  const args = ["-y", "-i", join(framesDir, name)];
  const vf = [];
  if (crop) vf.push(`crop=${crop}`); // match the video's chrome trim
  if (zoom != null) {
    // Match the video's FRAMING at this frame. The animated expressions can't be
    // reused: ffmpeg sees a single image here, so n=0 and every segment profile
    // would evaluate to the Z=1 rest state — the poster would show the wide,
    // un-pushed frame while the video at that timestamp is pushed in. Instead
    // evaluate Z/CX/CY at posterFrame and emit the equivalent STATIC crop, back-
    // projected into source pixels (divide the magnified window by Z).
    const { w, h } = zoom;
    const { z, cx, cy } = evalZoomAt(zoom, { fps: Number(fps), start, n: posterFrame - start });
    const cropX = Math.min(Math.max(cx * z - w / 2, 0), w * z - w);
    const cropY = Math.min(Math.max(cy * z - h / 2, 0), h * z - h);
    // Even dims for the same 4:2:0 reason as the video path. Rounding UP can push
    // the window past the source edge (ffmpeg then silently clamps x, so the poster
    // would frame a different region than the video) — clamp the origin into bounds.
    const sw = Math.min(Math.round(w / z / 2) * 2, Math.floor(w / 2) * 2);
    const sh = Math.min(Math.round(h / z / 2) * 2, Math.floor(h / 2) * 2);
    const sx = Math.min(Math.max(Math.round(cropX / z), 0), w - sw);
    const sy = Math.min(Math.max(Math.round(cropY / z), 0), h - sh);
    vf.push(`crop=${sw}:${sh}:${sx}:${sy}`, `scale=${w}:${h}:flags=lanczos`);
  }
  if (vf.length) args.push("-vf", vf.join(","));
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
                  "  [--crf <n>] [--drop-black-below <luma>] [--poster-frame <n>] [--crop W:H:X:Y]\n" +
                  "  [--zoom '<json>']  {w,h,segments:[{t0,t1,rect:[x,y,w,h],easeMs}]} — dynamic\n" +
                  "                     zoom segments in post-trim px / source seconds (loop=none only)");
    process.exit(2);
  }
  if (!existsSync(a.frames)) {
    console.error(`frames dir not found: ${a.frames}`);
    process.exit(2);
  }
  const start = numOpt(a, "start", 0);
  const loop = a.loop || "none";
  let zoom = null;
  if (a.zoom != null) {
    try { zoom = JSON.parse(a.zoom); }
    catch { console.error(`--zoom must be valid JSON, got "${a.zoom}"`); process.exit(2); }
  }
  run("ffmpeg", buildFfmpegArgs({
    framesDir: a.frames, fps: a.fps, out: a.out,
    start,
    loop,
    dropBlackBelow: a["drop-black-below"] != null ? numOpt(a, "drop-black-below", null) : null,
    crossfadeSec: numOpt(a, "crossfade", 1.0),
    // crossfade DERIVES its xfade offset from this; zoom only VALIDATES against it
    // (a segment must finish inside the stream), so count for every loop mode.
    frameCount: (loop === "crossfade" || zoom != null) ? countFrames(a.frames, start) : null,
    crf: numOpt(a, "crf", 20),
    crop: a.crop ?? null,
    zoom,
  }));
  if (a.poster) {
    const posterFrame = numOpt(a, "poster-frame", start);
    run("ffmpeg", buildPosterArgs({
      framesDir: a.frames, poster: a.poster, posterFrame, crop: a.crop ?? null,
      zoom, fps: a.fps, start,
    }));
  }
  console.log(`encoded ${a.out}`);
}
