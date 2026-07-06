import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildFfmpegArgs, buildPosterArgs } from "./encode.mjs";

const has = (bin) => spawnSync(bin, ["-version"]).status === 0;
const ffmpegReady = has("ffmpeg") && has("ffprobe");

test("buildFfmpegArgs carries the load-bearing invariants", () => {
  const args = buildFfmpegArgs({ framesDir: "d", fps: 30, out: "o.mp4" }).join(" ");
  assert.ok(args.includes("format=yuv420p"), "missing yuv420p");
  assert.ok(args.includes("trunc(iw/2)*2:trunc(ih/2)*2"), "missing even-dims scale");
  assert.ok(args.includes("+faststart"), "missing +faststart");
  assert.ok(args.includes("-an"), "missing -an");
  assert.ok(args.includes("-framerate 30"), "missing -framerate");
  // anti-flicker: the matrix (colorspace) must be tagged, never left "unknown".
  // We deliberately do NOT tag -color_trc (the sRGB source transfer is already
  // defined; mislabelling it bt709 is wrong and unnecessary).
  assert.ok(args.includes("-colorspace bt709"), "missing bt709 colorspace tag");
  assert.ok(args.includes("-color_primaries bt709"), "missing bt709 primaries");
  assert.ok(args.includes("-color_range tv"), "missing tv color range");
  assert.ok(!args.includes("-color_trc"), "should not mislabel the sRGB transfer as bt709");
});

test("crop trims the window chrome and runs before the even-dims/yuv420p invariants", () => {
  const args = buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4", crop: "1264:952:8:0" }).join(" ");
  assert.ok(args.includes("crop=1264:952:8:0"), "missing crop");
  // crop must precede scale (trim first, then enforce even dims on the trimmed frame)
  assert.ok(args.indexOf("crop=1264:952:8:0") < args.indexOf("scale=trunc"), "crop must run before scale");
  assert.ok(args.includes("format=yuv420p"), "invariant dropped");
  // no crop arg when none is requested
  assert.ok(!buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4" }).join(" ").includes("crop="), "crop should be opt-in");
  // the poster path carries the same crop, and none when absent
  assert.ok(buildPosterArgs({ framesDir: "d", poster: "p.jpg", crop: "1264:952:8:0" }).join(" ").includes("-vf crop=1264:952:8:0"), "poster missing crop");
  assert.ok(!buildPosterArgs({ framesDir: "d", poster: "p.jpg" }).join(" ").includes("crop="), "poster crop should be opt-in");
  // a crop that isn't W:H:X:Y is rejected — no extra-filter injection
  assert.throws(() => buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4", crop: "1264:952:8:0,transpose=1" }), /crop must be/);
  assert.throws(() => buildPosterArgs({ framesDir: "d", poster: "p.jpg", crop: "8:8:0:0;rm -rf" }), /crop must be/);
});

test("invariants survive the loop + drop-black + start options", () => {
  const args = buildFfmpegArgs({
    framesDir: "d", fps: 30, out: "o.mp4",
    start: 22, loop: "pingpong", dropBlackBelow: 35,
  }).join(" ");
  // load-bearing invariants must still be present in the composed graph
  assert.ok(args.includes("format=yuv420p"), "missing yuv420p");
  assert.ok(args.includes("trunc(iw/2)*2:trunc(ih/2)*2"), "missing even-dims scale");
  assert.ok(args.includes("+faststart"), "missing +faststart");
  assert.ok(args.includes("-an"), "missing -an");
  // new behaviours
  assert.ok(args.includes("-start_number 22"), "missing head trim");
  assert.ok(args.includes("concat=n=2:v=1"), "missing ping-pong concat");
  assert.ok(args.includes("-filter_complex"), "loop must use filter_complex");
  assert.ok(args.includes("lavfi.signalstats.YAVG"), "missing black-frame guard");
  assert.ok(args.includes("-r 30"), "missing pinned output cadence after drops/loop");
});

test("default (no loop) uses a plain -vf chain", () => {
  const args = buildFfmpegArgs({ framesDir: "d", fps: 30, out: "o.mp4" });
  assert.ok(args.includes("-vf"), "default should use -vf");
  assert.ok(!args.includes("-filter_complex"), "default should not use filter_complex");
  assert.ok(!args.includes("-start_number"), "default should not trim");
});

test("crossfade loop builds an xfade dissolve at offset D-2C", () => {
  // 210 frames @ 30fps = 7s; C=1 → offset = 7 - 2 = 5
  const args = buildFfmpegArgs({
    framesDir: "d", fps: 30, out: "o.mp4",
    loop: "crossfade", crossfadeSec: 1.0, frameCount: 210,
  }).join(" ");
  assert.ok(args.includes("xfade=transition=fade:duration=1"), "missing xfade");
  assert.ok(args.includes("offset=5.0000"), "wrong xfade offset");
  assert.ok(args.includes("-filter_complex"), "crossfade must use filter_complex");
  assert.ok(args.includes("format=yuv420p"), "invariant dropped");
  assert.ok(args.includes("+faststart") && args.includes("-an"), "invariant dropped");
  assert.ok(args.includes("-r 30"), "missing pinned cadence");
});

test("crf defaults to 20 and is overridable", () => {
  assert.ok(buildFfmpegArgs({ framesDir: "d", fps: 30, out: "o.mp4" }).join(" ").includes("-crf 20"));
  assert.ok(buildFfmpegArgs({ framesDir: "d", fps: 30, out: "o.mp4", crf: 16 }).join(" ").includes("-crf 16"));
});

test("crossfade without frameCount throws (offset is underivable)", () => {
  assert.throws(() => buildFfmpegArgs({
    framesDir: "d", fps: 30, out: "o.mp4", loop: "crossfade",
  }), /frameCount/);
});

test("crossfade rejects a clip shorter than 2× the crossfade (negative offset)", () => {
  // 30 frames @ 30fps = 1s, C=1 → offset would be -1
  assert.throws(() => buildFfmpegArgs({
    framesDir: "d", fps: 30, out: "o.mp4", loop: "crossfade", crossfadeSec: 1.0, frameCount: 30,
  }), /2× crossfade|duration/);
});

test("crossfade + drop-black-below is rejected (stream length mismatch)", () => {
  assert.throws(() => buildFfmpegArgs({
    framesDir: "d", fps: 30, out: "o.mp4", loop: "crossfade", crossfadeSec: 1.0,
    frameCount: 210, dropBlackBelow: 35,
  }), /drop-black/);
});

test("encode produces yuv420p + even dims + faststart", { skip: !ffmpegReady && "ffmpeg/ffprobe not installed" }, () => {
  const dir = mkdtempSync(join(tmpdir(), "clip-enc-"));
  // Generate 3 valid 100x100 PNGs WITH ffmpeg (no fragile embedded base64).
  const gen = spawnSync("ffmpeg", [
    "-y", "-f", "lavfi", "-i", "color=c=red:s=100x100:d=1", "-r", "3", "-frames:v", "3",
    join(dir, "frame_%05d.png"),
  ]);
  assert.equal(gen.status, 0, "frame generation failed");
  const out = join(dir, "out.mp4");
  const enc = spawnSync("ffmpeg", buildFfmpegArgs({ framesDir: dir, fps: 30, out }));
  assert.equal(enc.status, 0, "encode failed");
  const probe = spawnSync("ffprobe", [
    "-v", "error", "-select_streams", "v:0",
    "-show_entries", "stream=pix_fmt,width,height,color_space", "-of", "json", out,
  ], { encoding: "utf8" });
  const s = JSON.parse(probe.stdout).streams[0];
  assert.equal(s.pix_fmt, "yuv420p");
  assert.equal(s.width % 2, 0);
  assert.equal(s.height % 2, 0);
  // anti-flicker: the matrix must be tagged bt709, never "unknown" (which makes
  // players guess and Chromium flicker between overlay/composite paths).
  assert.equal(s.color_space, "bt709", "color matrix left unknown/wrong");
  // faststart => moov atom present AND before mdat.
  const trace = spawnSync("ffprobe", ["-v", "trace", out], { encoding: "utf8" }).stderr || "";
  const moov = trace.indexOf("type:'moov'");
  const mdat = trace.indexOf("type:'mdat'");
  assert.ok(moov >= 0 && mdat >= 0, "moov/mdat atoms not found");
  assert.ok(moov < mdat, "+faststart not applied (moov after mdat)");
});

// --- dynamic zoom ------------------------------------------------------------
import { assertZoom, buildZoomFilters } from "./encode.mjs";

const ZOOM1 = { w: 1264, h: 950, segments: [{ t0: 2, t1: 6, rect: [0, 96, 632, 475], easeMs: 400 }] };

test("zoom: validation rejects the unsound combinations", () => {
  // loop modes reorder frames — n-based zoom would land wrong
  assert.throws(() => assertZoom(ZOOM1, { loop: "pingpong", fps: 60 }), /loop=none/);
  assert.throws(() => assertZoom(ZOOM1, { dropBlackBelow: 35, fps: 60 }), /drop-black-below/);
  // rect aspect must match the frame aspect (magnification, not reframe)
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 2, t1: 6, rect: [0, 0, 632, 300], easeMs: 400 }] }, { fps: 60 }), /aspect/);
  // rect must stay in bounds
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 2, t1: 6, rect: [700, 500, 632, 475], easeMs: 400 }] }, { fps: 60 }), /bounds/);
  // segment must be longer than its two eases
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 2, t1: 2.5, rect: [0, 0, 632, 475], easeMs: 400 }] }, { fps: 60 }), /shorter than/);
  // overlapping segments rejected
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [
    { t0: 2, t1: 6, rect: [0, 0, 632, 475], easeMs: 400 },
    { t0: 5, t1: 9, rect: [0, 0, 632, 475], easeMs: 400 },
  ] }, { fps: 60 }), /non-overlapping/);
  // a segment starting before the --start trim is unrepresentable in `n`
  assert.throws(() => assertZoom(ZOOM1, { fps: 60, start: 200 }), /--start/);
  // odd frame dims would floor BELOW the crop size at a segment's Z=1 rest
  // endpoints (trunc(h*1/2)*2 < h when h is odd) — reject before it reaches ffmpeg
  assert.throws(() => assertZoom({ w: 1263, h: 950, segments: ZOOM1.segments }, { fps: 60 }), /EVEN/);
  assert.throws(() => assertZoom({ w: 1264, h: 951, segments: ZOOM1.segments }, { fps: 60 }), /EVEN/);
  // a segment exactly 2x its ease has zero hold time between ramps — degenerate, reject
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 2, t1: 2.8, rect: [0, 0, 632, 475], easeMs: 400 }] }, { fps: 60 }), /shorter than/);
  // the happy path validates clean
  assertZoom(ZOOM1, { loop: "none", fps: 60, start: 30 });
});

test("zoom: generated filters use the spiked construct (animated scale + static crop in n)", () => {
  const [scaleF, cropF] = buildZoomFilters({ zoom: ZOOM1, fps: 60, start: 30 });
  // animated magnification must evaluate per frame and never animate crop w/h
  assert.ok(scaleF.includes("eval=frame"), "scale must be eval=frame");
  assert.ok(cropF.startsWith("crop=1264:950:"), "crop output size must be static");
  // crop x/y must be pure functions of n (crop's iw freezes at config time — spiked)
  assert.ok(!/x='[^']*\biw\b/.test(cropF) && !/y='[^']*\biw\b/.test(cropF), "crop x/y must not reference iw");
  // frame domain is post-start-trim: t0=2s @60fps, start=30 → n=90
  assert.ok(scaleF.includes("(n-90)"), "ease-in must start at n=90 (t0*fps - start)");
  assert.ok(scaleF.includes("(330-n)"), "ease-out must end at n=330 (t1*fps - start)");
  // both filters land in the -vf chain between the chrome trim and the invariants
  const args = buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4", start: 30, crop: "1264:950:0:0", zoom: ZOOM1 }).join(" ");
  assert.ok(args.indexOf("crop=1264:950:0:0") < args.indexOf("eval=frame"), "zoom must run after the chrome trim");
  assert.ok(args.indexOf("eval=frame") < args.indexOf("format=yuv420p"), "zoom must run before the invariants");
  // no zoom → no zoom filters
  assert.ok(!buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4" }).join(" ").includes("eval=frame"), "zoom should be opt-in");
});
