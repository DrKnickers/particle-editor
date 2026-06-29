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
    "-show_entries", "stream=pix_fmt,width,height", "-of", "json", out,
  ], { encoding: "utf8" });
  const s = JSON.parse(probe.stdout).streams[0];
  assert.equal(s.pix_fmt, "yuv420p");
  assert.equal(s.width % 2, 0);
  assert.equal(s.height % 2, 0);
  // faststart => moov atom present AND before mdat.
  const trace = spawnSync("ffprobe", ["-v", "trace", out], { encoding: "utf8" }).stderr || "";
  const moov = trace.indexOf("type:'moov'");
  const mdat = trace.indexOf("type:'mdat'");
  assert.ok(moov >= 0 && mdat >= 0, "moov/mdat atoms not found");
  assert.ok(moov < mdat, "+faststart not applied (moov after mdat)");
});
