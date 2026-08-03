#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import { existsSync, readdirSync } from "node:fs";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const DEFAULT_CROP = "660:620:340:110";
const DEFAULT_TOLERANCE = 1.0;
const USAGE = "usage: node seam-match.mjs --frames <dir> --start <n> [--crop W:H:X:Y] [--tolerance <yavg>]";

export function parseCropArg(str) {
  const parts = String(str).split(":");
  if (parts.length !== 4) throw new Error("invalid crop \"" + str + "\": expected W:H:X:Y");
  const [w, h, x, y] = parts.map((part) => Number(part));
  if (![w, h, x, y].every((value) => Number.isFinite(value))) {
    throw new Error("invalid crop \"" + str + "\": values must be numbers");
  }
  return { w, h, x, y };
}

export function computeDelta(a, b) {
  return Math.abs(Number(a) - Number(b));
}

export function isWithinTolerance(delta, tol) {
  return Math.abs(Number(delta)) <= Number(tol);
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

function framePath(framesDir, n) {
  return join(framesDir, "frame_" + String(n).padStart(5, "0") + ".png");
}

function listPngFrames(framesDir) {
  return readdirSync(framesDir).filter((name) => name.toLowerCase().endsWith(".png")).sort();
}

function parseYavg(output) {
  const match = String(output).match(/lavfi\.signalstats\.YAVG=([-+]?\d+(?:\.\d+)?)/);
  if (!match) throw new Error("ffmpeg output did not include lavfi.signalstats.YAVG");
  return Number(match[1]);
}

export function measureYavg(file, crop) {
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
  if (!args.frames || args.start == null) {
    console.error(USAGE);
    process.exit(2);
  }
  if (!existsSync(args.frames)) {
    console.error("frames dir not found: " + args.frames);
    process.exit(2);
  }

  try {
    const crop = parseCropArg(args.crop || DEFAULT_CROP);
    const start = numOpt(args, "start", null);
    const tolerance = numOpt(args, "tolerance", DEFAULT_TOLERANCE);
    const startFile = framePath(args.frames, start);
    if (!existsSync(startFile)) throw new Error("start frame not found: " + startFile);
    const pngs = listPngFrames(args.frames);
    if (pngs.length === 0) throw new Error("no png frames found in " + args.frames);
    const lastFile = join(args.frames, pngs[pngs.length - 1]);
    const startYavg = measureYavg(startFile, crop);
    const lastYavg = measureYavg(lastFile, crop);
    const delta = computeDelta(startYavg, lastYavg);
    console.log("start " + start + ": YAVG " + startYavg);
    console.log("last " + pngs[pngs.length - 1] + ": YAVG " + lastYavg);
    console.log("delta: " + delta);
    if (!isWithinTolerance(delta, tolerance)) {
      console.error("seam mismatch: delta " + delta + " exceeds tolerance " + tolerance);
      process.exit(1);
    }
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
