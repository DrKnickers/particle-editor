#!/usr/bin/env node
import { appendFileSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, isAbsolute, join, resolve } from "node:path";

const mode = process.argv[2];
const args = process.argv.slice(3);
const logPath = process.env.WIKI_MEDIA_MOCK_LOG;
const png = Buffer.from("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAFElEQVR4nGOsrKz8z8DAwMDAxAgABxMBgDDqiYQAAAAASUVORK5CYII=", "base64");

function log(line) {
  if (!logPath) return;
  mkdirSync(dirname(logPath), { recursive: true });
  appendFileSync(logPath, line + "\n");
}

function argValue(name) {
  const index = args.indexOf(name);
  return index === -1 ? null : args[index + 1];
}

function write(file, content) {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, content == null ? "" : content);
}

function hasTarget(value) {
  if (!value || typeof value !== "object") return false;
  if (Object.prototype.hasOwnProperty.call(value, "target")) return true;
  if (Array.isArray(value)) return value.some(hasTarget);
  return Object.values(value).some(hasTarget);
}

function render() {
  log("mode=render args=" + args.join(" "));
  const timelinePath = argValue("--record");
  const timeline = JSON.parse(readFileSync(timelinePath, "utf8"));
  const exitCode = Number(timeline.mockExitCode || 0);
  if (exitCode !== 0) process.exit(exitCode);
  const frameDir = isAbsolute(timeline.out) ? timeline.out : resolve(process.cwd(), timeline.out);
  mkdirSync(frameDir, { recursive: true });
  for (let i = 0; i < 80; i++) write(join(frameDir, "frame_" + String(i).padStart(5, "0") + ".png"), png);
  if (hasTarget(timeline) && !timeline.mockNoSidecar) write(join(frameDir, "cursor-sidecar.json"), "[]\n");
  console.log("[record] put_RasterizationScale(1.00) hr=0x00000000");
}

function pass() {
  log("mode=" + mode + " args=" + args.join(" "));
  // Test hook: WIKI_MEDIA_MOCK_FAIL="partial,seam" makes those verify modes exit
  // non-zero, so a test can exercise the gate-FAIL -> item-FAIL -> batch-continues
  // -> non-zero-exit path for a verifier (not just render/encode).
  const failModes = (process.env.WIKI_MEDIA_MOCK_FAIL || "").split(",").filter(Boolean);
  if (failModes.includes(mode)) process.exit(1);
}

function encode() {
  pass();
  const out = argValue("--out");
  const poster = argValue("--poster");
  if (out) write(out, "mock mp4\n");
  if (poster) write(poster, "mock poster\n");
}

function ffmpeg() {
  pass();
  const out = args[args.length - 1];
  if (out && !out.startsWith("-")) write(out, "mock ffmpeg output\n");
}

if (mode === "render") render();
else if (mode === "encode") encode();
else if (mode === "ffmpeg") ffmpeg();
else pass();
