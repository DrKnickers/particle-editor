#!/usr/bin/env node
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const USAGE = "usage: node cursor-on-target.mjs --sidecar <path>";

// A clip's CLICKS/DRAGS (press frames) must land on a real element — a press
// frame whose target didn't resolve means the cursor pressed empty space. The
// host already aborts the render on that, so this is a defensive cross-check.
// A NON-press transit frame may legitimately carry an ok:false (the cursor is
// passing over a not-yet-mounted target and is hidden for that frame) — that is
// expected, NOT a failure. (No on-target distance check: the recorded cursor IS
// the resolved centre by construction for held frames, so a distance assertion
// would be tautological there and a false positive mid-travel.)
export function evaluateSidecar(entries) {
  const pressMisses = [];
  let pressFrames = 0;
  let hiddenTransit = 0;
  const refs = new Set();

  for (const entry of entries) {
    const cursor = entry.cursor || {};
    const resolved = Array.isArray(entry.resolved) ? entry.resolved : [];
    const pressed = cursor.press === true;
    if (pressed) pressFrames += 1;
    let frameHadMiss = false;
    for (const target of resolved) {
      refs.add(target.ref);
      if (target.ok === false) {
        frameHadMiss = true;
        if (pressed) pressMisses.push({ frame: entry.frame, ref: target.ref });
      }
    }
    if (!pressed && frameHadMiss && cursor.vis === false) hiddenTransit += 1;
  }

  return {
    pressMisses,
    stats: { frames: entries.length, pressFrames, hiddenTransit, refs: [...refs] },
  };
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
  if (!args.sidecar) {
    console.error(USAGE);
    process.exit(2);
  }

  try {
    const entries = JSON.parse(readFileSync(args.sidecar, "utf8"));
    if (!Array.isArray(entries)) throw new Error("sidecar JSON must be an array");
    const { pressMisses, stats } = evaluateSidecar(entries);
    console.log(
      "frames=" + stats.frames + " press=" + stats.pressFrames +
      " hidden-transit=" + stats.hiddenTransit + " refs=" + stats.refs.length +
      " [" + stats.refs.join(", ") + "]",
    );
    for (const miss of pressMisses) {
      console.error("frame " + miss.frame + ": press target " + miss.ref + " unresolved");
    }
    if (pressMisses.length > 0) process.exit(1);
    console.log("cursor-on-target OK — every press frame landed on a resolved target");
  } catch (err) {
    console.error(err.message);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
