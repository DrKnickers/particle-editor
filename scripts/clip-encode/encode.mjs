#!/usr/bin/env node
// Encode a numbered PNG sequence (frame_%05d.png) into a looping, web-safe H.264
// mp4 + a poster. The -vf invariants are LOAD-BEARING: format=yuv420p (else black
// on Safari/iOS), even dims (libx264 4:2:0), +faststart (plays before full
// download), -an (silent). Usage:
//   node encode.mjs --frames <dir> --fps <n> --out <clip.mp4> [--poster <poster.jpg>]
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { join } from "node:path";

export function buildFfmpegArgs({ framesDir, fps, out }) {
  return [
    "-y",
    "-framerate", String(fps),
    "-i", join(framesDir, "frame_%05d.png"),
    "-c:v", "libx264", "-crf", "20", "-preset", "slow",
    "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2,format=yuv420p",
    "-movflags", "+faststart",
    "-an",
    out,
  ];
}

export function buildPosterArgs({ framesDir, poster }) {
  return ["-y", "-i", join(framesDir, "frame_00000.png"), "-frames:v", "1", poster];
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
  for (let i = 0; i < argv.length; i += 2) a[argv[i].replace(/^--/, "")] = argv[i + 1];
  return a;
}

// Run only when invoked directly (not when imported by the test).
if (process.argv[1] && process.argv[1].endsWith("encode.mjs")) {
  const a = parseArgs(process.argv.slice(2));
  if (!a.frames || !a.fps || !a.out) {
    console.error("usage: node encode.mjs --frames <dir> --fps <n> --out <clip.mp4> [--poster <p.jpg>]");
    process.exit(2);
  }
  if (!existsSync(a.frames)) {
    console.error(`frames dir not found: ${a.frames}`);
    process.exit(2);
  }
  run("ffmpeg", buildFfmpegArgs({ framesDir: a.frames, fps: a.fps, out: a.out }));
  if (a.poster) run("ffmpeg", buildPosterArgs({ framesDir: a.frames, poster: a.poster }));
  console.log(`encoded ${a.out}`);
}
