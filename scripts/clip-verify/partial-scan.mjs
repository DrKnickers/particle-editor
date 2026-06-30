#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const DEFAULT_CROP = "660:620:340:110";
const DEFAULT_START = 0;
const DEFAULT_DROP = 3.0;
const USAGE = "usage: node partial-scan.mjs --frames <dir> [--start <n>] [--crop W:H:X:Y] [--drop <yavg>]";

export function detectIsolatedDips(yavgArray, drop) {
  const dips = [];
  for (let i = 1; i < yavgArray.length - 1; i++) {
    const yavg = Number(yavgArray[i]);
    const prev = Number(yavgArray[i - 1]);
    const next = Number(yavgArray[i + 1]);
    if (prev - yavg > drop && next - yavg > drop) {
      dips.push({ index: i, yavg, prev, next });
    }
  }
  return dips;
}

function parseCropArg(str) {
  const parts = String(str).split(":");
  if (parts.length !== 4) throw new Error("invalid crop \"" + str + "\": expected W:H:X:Y");
  const [w, h, x, y] = parts.map((part) => Number(part));
  if (![w, h, x, y].every((value) => Number.isFinite(value))) {
    throw new Error("invalid crop \"" + str + "\": values must be numbers");
  }
  return { w, h, x, y };
}

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i++) {
    const item = argv[i];
    if (item === "--help") return { help: true };
    if (!item.startsWith("--")) throw new Error("unexpected argument: " + item);
    const key = item.slice(2);
    const val = argv[++i];
    if (val == null || val.startsWith("--")) throw new Error("option --" + key + " requires a value");
    args[key] = val;
  }
  return args;
}

function numOpt(args, name, def) {
  if (args[name] == null) return def;
  const value = Number(args[name]);
  if (!Number.isFinite(value)) throw new Error("--" + name + " must be a number, got \"" + args[name] + "\"");
  return value;
}

function frameNumber(name, fallback) {
  const match = name.match(/frame_(\d+)\.png$/i);
  return match ? Number(match[1]) : fallback;
}

function listPngFrames(framesDir, start) {
  return readdirSync(framesDir)
    .filter((name) => name.toLowerCase().endsWith(".png"))
    .sort()
    .map((name, index) => ({ name, number: frameNumber(name, index), file: join(framesDir, name) }))
    .filter((frame) => frame.number >= start);
}

function parseYavg(output) {
  const match = String(output).match(/lavfi\.signalstats\.YAVG=([-+]?\d+(?:\.\d+)?)/);
  if (!match) throw new Error("ffmpeg output did not include lavfi.signalstats.YAVG");
  return Number(match[1]);
}

function measureYavg(file, crop) {
  // file=- forces the metadata print to stdout; without it the print is an INFO-
  // level log line that `-v error` swallows (then YAVG never appears).
  const filter = "crop=" + crop.w + ":" + crop.h + ":" + crop.x + ":" + crop.y + ",signalstats,metadata=print:key=lavfi.signalstats.YAVG:file=-";
  const result = spawnSync("ffmpeg", ["-v", "error", "-i", file, "-vf", filter, "-f", "null", "-"], { encoding: "utf8" });
  if (result.status !== 0) throw new Error("ffmpeg failed for " + file + " (status " + result.status + ")");
  return parseYavg((result.stdout || "") + "\n" + (result.stderr || ""));
}

function main(argv) {
  let args;
  try {
    args = parseArgs(argv);
  } catch (err) {
    console.error(USAGE);
    console.error(err.message);
    process.exit(2);
  }
  if (args.help) {
    console.log(USAGE);
    return;
  }
  if (!args.frames) {
    console.error(USAGE);
    process.exit(2);
  }
  if (!existsSync(args.frames)) {
    console.error("frames dir not found: " + args.frames);
    process.exit(2);
  }

  try {
    const start = numOpt(args, "start", DEFAULT_START);
    const crop = parseCropArg(args.crop || DEFAULT_CROP);
    const drop = numOpt(args, "drop", DEFAULT_DROP);
    const frames = listPngFrames(args.frames, start);
    if (frames.length === 0) throw new Error("no png frames found in " + args.frames + " at or after " + start);
    const yavg = frames.map((frame) => measureYavg(frame.file, crop));
    const dips = detectIsolatedDips(yavg, drop);
    for (const dip of dips) {
      const frame = frames[dip.index];
      console.log("frame " + frame.number + ": " + dip.yavg + " (neighbors " + dip.prev + "," + dip.next + ")");
    }
    if (dips.length > 0) process.exit(1);
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
