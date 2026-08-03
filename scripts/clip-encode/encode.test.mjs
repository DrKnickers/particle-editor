import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildFfmpegArgs, buildPosterArgs, countFrames } from "./encode.mjs";

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
import { assertZoom, buildZoomFilters, evalZoomAt } from "./encode.mjs";

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
  // frame domain is post-start-trim: t0=2s @60fps, start=30 → n=90.
  // crop keeps the plain index; scale is fed n-1 (see SCALE_N_LEAD in encode.mjs).
  assert.ok(cropF.includes("(n-90)"), "crop ease-in must start at n=90 (t0*fps - start)");
  assert.ok(cropF.includes("(330-n)"), "crop ease-out must end at n=330 (t1*fps - start)");
  assert.ok(scaleF.includes("((n-1)-90)"), "scale must be fed n-1 (its n leads crop's by one frame)");
  assert.ok(scaleF.includes("(330-(n-1))"), "scale must be fed n-1 on the fall ramp too");
  // both filters land in the -vf chain between the chrome trim and the invariants
  const args = buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4", start: 30, crop: "1264:950:0:0", zoom: ZOOM1 }).join(" ");
  assert.ok(args.indexOf("crop=1264:950:0:0") < args.indexOf("eval=frame"), "zoom must run after the chrome trim");
  assert.ok(args.indexOf("eval=frame") < args.indexOf("format=yuv420p"), "zoom must run before the invariants");
  // no zoom → no zoom filters
  assert.ok(!buildFfmpegArgs({ framesDir: "d", fps: 60, out: "o.mp4" }).join(" ").includes("eval=frame"), "zoom should be opt-in");
});

// The clip <video> tags carry `loop` (scripts/build-guide.mjs), so a segment that
// does not complete its ease-out leaves the last frame zoomed while frame 0 is
// wide — a hard cut on every repeat. NOTHING else catches this: every build.mjs
// gate runs on the RAW pre-encode frames, and the zoom is applied after them.
test("zoom: a segment must finish inside the stream (loop-safety)", () => {
  // 144-frame clip (2.4s @60): t1=2.3 -> n1=138, fine.
  assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 2.3, rect: [450, 153, 702, 528], easeMs: 350 }] },
             { fps: 60, frameCount: 144 });
  // t1=2.5 -> n1=150 > last frame 143: the ease-out never completes.
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 2.5, rect: [450, 153, 702, 528], easeMs: 350 }] },
                                 { fps: 60, frameCount: 144 }), /past the last frame/);
  // exactly the last frame is allowed
  assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 143 / 60, rect: [450, 153, 702, 528], easeMs: 350 }] },
             { fps: 60, frameCount: 144 });
  // honours --start (n is post-trim)
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 1, t1: 2.3, rect: [450, 153, 702, 528], easeMs: 350 }] },
                                 { fps: 60, start: 30, frameCount: 100 }), /past the last frame/);
  // assertZoom itself can't check the bound without a count, but buildFfmpegArgs
  // must not let a caller opt out of it — it counts the frames itself (below).
  assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 99, rect: [450, 153, 702, 528], easeMs: 350 }] }, { fps: 60 });
});

test("zoom: buildFfmpegArgs derives frameCount itself, so the loop bound can't be opted out of", (t) => {
  // A caller omitting frameCount would otherwise skip the ONE check that stops a
  // looping <video> snapping. Real frames on disk => the bound must still bite.
  const dir = mkdtempSync(join(tmpdir(), "zoomfc-"));
  // -start_number 0: the image2 MUXER numbers from 1 by default, but --record writes
  // frame_00000.png and countFrames scans from `start`, so a 1-based fixture would
  // count 0 frames and vacuously skip the very bound this test exists to prove.
  const r = spawnSync("ffmpeg", ["-v", "error", "-y", "-f", "lavfi",
    "-i", "testsrc=size=64x48:rate=60:duration=0.2", "-start_number", "0", join(dir, "frame_%05d.png")], { encoding: "utf8" });
  if (r.status !== 0) return; // no ffmpeg -> skip
  assert.ok(countFrames(dir, 0) > 0, "fixture must write frame_00000.png (countFrames scans from `start`)");
  const over = { w: 64, h: 48, segments: [{ t0: 0, t1: 5, rect: [16, 12, 32, 24], easeMs: 100 }] };
  assert.throws(
    () => buildFfmpegArgs({ framesDir: dir, fps: 60, out: "o.mp4", crop: "64:48:0:0", zoom: over }),
    /past the last frame/,
    "buildFfmpegArgs must count the frames and reject a segment that never completes");
});

test("zoom: an ease shorter than one frame is rejected (it serializes to 0.000 -> divide by zero)", () => {
  // E is emitted as .toFixed(3) FRAMES; a sub-milliframe ease becomes "0.000" and the
  // rise is clip(x/0,0,1) -- non-finite, and the JS poster model would disagree with it.
  assert.throws(() => assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 1, rect: [0, 0, 632, 475], easeMs: 0.001 }] },
                                 { fps: 60 }), /under one frame/);
  // exactly one frame is fine
  assertZoom({ w: 1264, h: 950, segments: [{ t0: 0, t1: 1, rect: [0, 0, 632, 475], easeMs: 1000 / 60 }] }, { fps: 60 });
});

test("zoom: the poster's back-projected crop never exceeds the source", () => {
  // Rounding w/z UP can push the window past the edge; ffmpeg then silently clamps x
  // and the poster frames a DIFFERENT region than the video. Sweep awkward rects.
  for (const rect of [[450, 153, 702, 528], [563, 0, 701, 527], [0, 0, 632, 475], [281, 211, 702, 528]]) {
    const zoom = { w: 1264, h: 950, segments: [{ t0: 0, t1: 2, rect, easeMs: 250 }] };
    const args = buildPosterArgs({ framesDir: "d", poster: "p.jpg", posterFrame: 60, crop: "1264:950:0:0", zoom, fps: 60 }).join(" ");
    const m = args.match(/crop=(\d+):(\d+):(\d+):(\d+),scale=/);
    assert.ok(m, "poster must emit a static crop: " + args);
    const [cw, ch, cx, cy] = m.slice(1).map(Number);
    assert.ok(cx + cw <= 1264, `rect ${rect}: crop right edge ${cx + cw} exceeds 1264`);
    assert.ok(cy + ch <= 950, `rect ${rect}: crop bottom edge ${cy + ch} exceeds 950`);
    assert.equal(cw % 2, 0, "even width"); assert.equal(ch % 2, 0, "even height");
  }
});

// The loop seam itself: the profile must be exactly 0 at both ends, so the first
// and last frames are the identical un-zoomed frame.
test("zoom: profile rests at Z=1 outside the segment, so frame 0 == last frame", () => {
  const z = { w: 1264, h: 950, segments: [{ t0: 0, t1: 2.3, rect: [450, 153, 702, 528], easeMs: 350 }] };
  for (const n of [0, 138, 143]) {
    const s = evalZoomAt(z, { fps: 60, n });
    assert.equal(s.z, 1, `Z must rest at 1 at n=${n}`);
    assert.equal(s.cx, 632, `CX must rest at the frame centre at n=${n}`);
    assert.equal(s.cy, 475, `CY must rest at the frame centre at n=${n}`);
  }
});

// THE user-facing framing contract (tutorial-05, 2026-07-17): the app viewport is
// [338,112,1264,723] -> centre (801,417), which is +169,-58 off the 1264x950 frame
// centre. A rect centred on the VIEWPORT centre maps whatever sits there to the
// FRAME centre during the hold, and holds it there — so the effect is locked dead
// centre for the whole push-in. If this breaks, the explosion drifts off centre.
test("zoom: a rect centred on the viewport centre locks that point to the frame centre", () => {
  const W = 1264, H = 950, VCX = 801, VCY = 417;
  const zoom = { w: W, h: H, segments: [{ t0: 0, t1: 2.3, rect: [450, 153, 702, 528], easeMs: 350 }] };
  // rect centre is the viewport centre
  assert.equal(450 + 702 / 2, VCX);
  assert.equal(153 + 528 / 2, VCY);
  const outAt = (n, sx, sy) => {
    const { z, cx, cy } = evalZoomAt(zoom, { fps: 60, n });
    const cropX = Math.min(Math.max(cx * z - W / 2, 0), W * z - W);
    const cropY = Math.min(Math.max(cy * z - H / 2, 0), H * z - H);
    return [sx * z - cropX, sy * z - cropY];
  };
  // During the HOLD the viewport centre lands on the frame centre and STAYS.
  // Hold == [A+E, D-E] == [21, 117] here (easeMs 350 @60 -> E=21, D=round(2.3*60)=138).
  // Outside it the point deliberately drifts — that IS the push-in/pull-out — which
  // is why the burst must live inside the hold and the ease-out on the dead tail.
  for (const n of [21, 45, 90, 117]) {
    const [x, y] = outAt(n, VCX, VCY);
    assert.ok(Math.abs(x - 632) < 0.001, `hold n=${n}: x=${x} must be 632`);
    assert.ok(Math.abs(y - 475) < 0.001, `hold n=${n}: y=${y} must be 475`);
  }
  // and the clamp never binds anywhere in the sweep (it would break the lock)
  for (let n = 0; n <= 143; n++) {
    const { z, cx, cy } = evalZoomAt(zoom, { fps: 60, n });
    const rawX = cx * z - W / 2, rawY = cy * z - H / 2;
    assert.ok(rawX >= -1e-9 && rawX <= W * z - W + 1e-9, `crop x clamp binds at n=${n}`);
    assert.ok(rawY >= -1e-9 && rawY <= H * z - H + 1e-9, `crop y clamp binds at n=${n}`);
  }
});

// The zoom construct assumes scale's `n` LEADS crop's by exactly one frame
// (SCALE_N_LEAD in encode.mjs). That is an observed ffmpeg behaviour, not a
// documented contract, so re-probe it: if a future ffmpeg changes it, the n-1
// compensation would silently re-desync the two stages and reintroduce the
// loop snap. Probe by making the output WIDTH encode the answer.
test("zoom: scale/crop agree on the frame index (re-probes the ffmpeg n desync)", () => {
  const firstFrameWidth = (filter) => {
    const out = join(mkdtempSync(join(tmpdir(), "zoomprobe-")), "f.png");
    const r = spawnSync("ffmpeg", ["-v", "error", "-y", "-f", "lavfi",
      "-i", "testsrc=size=60x60:rate=60:duration=0.2", "-vf", filter, "-frames:v", "1", out],
      { encoding: "utf8" });
    if (r.status !== 0) return null;
    const p = spawnSync("ffprobe", ["-v", "error", "-select_streams", "v:0",
      "-show_entries", "stream=width", "-of", "csv=p=0", out], { encoding: "utf8" });
    return parseInt((p.stdout || "").trim(), 10);
  };
  // width==20 iff that filter sees the probed n on the first frame. Compare the RAW
  // widths, not booleans: `null === 20` is false, so folding the null into a boolean
  // first would make the no-ffmpeg skip unreachable and fail the assert instead.
  const scaleN0 = firstFrameWidth("scale=w='if(eq(n\\,0)\\,20\\,60)':h=60:eval=frame");
  const cropN0 = firstFrameWidth("crop=w='if(eq(n\\,0)\\,20\\,60)':h=60:x=0:y=0");
  const scaleN1 = firstFrameWidth("scale=w='if(eq(n\\,1)\\,20\\,60)':h=60:eval=frame");
  if (scaleN0 == null || cropN0 == null || scaleN1 == null) return; // no ffmpeg -> skip
  assert.equal(cropN0 === 20, true, "crop's n must start at 0 (the zoom construct's reference)");
  assert.equal(scaleN0 === 20, false,
    "scale's n no longer LEADS crop's by one frame — SCALE_N_LEAD in encode.mjs must be re-derived, " +
    "or the zoom's scale/crop stages will describe different frames and the loop seam will snap");
  assert.equal(scaleN1 === 20, true, "scale's n must lead by exactly 1 (SCALE_N_LEAD=1)");
});

test("zoom: the poster matches the video's framing at posterFrame", () => {
  const zoom = { w: 1264, h: 950, segments: [{ t0: 0, t1: 2.3, rect: [450, 153, 702, 528], easeMs: 350 }] };
  // frame 45 is inside the hold -> the poster must be cropped+rescaled, NOT wide.
  const args = buildPosterArgs({ framesDir: "d", poster: "p.jpg", posterFrame: 45, crop: "1264:950:0:0", zoom, fps: 60 }).join(" ");
  assert.ok(args.includes("crop=1264:950:0:0"), "chrome trim still applied");
  assert.ok(/crop=702:528:/.test(args), `poster must crop to the zoom's source window: ${args}`);
  assert.ok(args.includes("scale=1264:950"), "poster must rescale back to the frame size");
  // no zoom -> unchanged behaviour (chrome trim only)
  const plain = buildPosterArgs({ framesDir: "d", poster: "p.jpg", posterFrame: 45, crop: "1264:950:0:0" }).join(" ");
  assert.ok(!plain.includes("scale="), "poster must not rescale when there is no zoom");
});
