import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { buildFfmpegArgs } from "./encode.mjs";

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
