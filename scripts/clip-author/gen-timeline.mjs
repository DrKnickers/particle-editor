#!/usr/bin/env node
import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const USAGE = "usage: node gen-timeline.mjs <beats.json> [--out <timeline.json>]";

// Beats -> --record timeline generator. The verbose parts of a timeline are the
// MECHANICAL parts — char-by-char typing patches, activate click triples, stepped
// value walks, cursor approach/dwell keys — so this expands a compact beats spec
// into the full JSON with the interaction-honest conventions (clip-author skill
// reference.md §4/§4c/§4d) emitted by construction: real activate clicks where
// they're safe, hover + scripted steps where a press would wedge (curve keys,
// spinners). Lint the output (scripts/clip-verify/timeline-lint.mjs) before
// rendering; the committed .json timeline stays the source of truth.
//
// Spec shape (all times ms):
// {
//   "open": "C:/abs/EFFECT.ALO", "fps": 60, "width": 1280, "height": 960,
//   "durationMs": 50000, "out": "clips/x", "openSettleMs": 2000,
//   "saveRoot": "C:/abs/dir",            // only if a beat saves
//   "emitterId": 0,                      // default id for typed/stepped beats
//   "park": { "x": 700, "y": 500 },      // off-interest cursor rest point
//   "prelude": [ {at,kind,params}... ],  // raw at-events copied verbatim (mods/set-layers, spawner, camera setup...)
//   "tweens":  [ ... ],                  // raw tween entries copied verbatim
//   "beats": [
//     { "at", "do": "click",  "ref"|"point":{x,y} }                                  // real activate click (safe targets only)
//     { "at", "do": "drag",   "from":{ref|point}, "to":{ref|point}, "travelMs"?:600 } // §4b reorder drag: grab a row, move pressed, drop
//     { "at", "do": "type",   "ref", "field"?:"name", "text", "charMs"?:90, "id"? }  // focus-click + char-by-char patches
//     { "at", "do": "step",   "field", "values":[...], "stepMs"?:400, "hover"?, "id"? }        // set-properties walk, cursor hovers
//     { "at", "do": "step-key","track","keyTime","values":[...], "stepMs"?:400, "hover"? }     // set-track-key walk, cursor hovers
//     { "at", "do": "focus-channel", "channel", "click"?:true }                      // scripted guarantee (+ real row click)
//     { "at", "do": "select-key", "track", "time", "hover"?:true|{ref|point} }       // scripted select (+ hover the key ref, or an override target)
//     { "at", "do": "show-panel", "panel" }
//     { "at", "do": "hover",  "ref"|"point", "holdMs"?:400 }
//     { "at", "do": "event",  "kind", "params" }                                     // raw escape hatch
//   ]
// }

const CLICK_APPROACH_MS = 400; // travel arrives this early, dwells, then presses
const CLICK_HOLD_MS = 150;     // press-down -> release
const TYPE_FOCUS_GAP_MS = 700; // click-to-focus -> first char
const HOVER_LEAD_MS = 600;     // hover arrives before a stepped walk begins

const elTarget = (ref) => ({ kind: "element", ref });
const beatTarget = (b) => {
  if (typeof b.ref === "string") return elTarget(b.ref);
  if (b.point && typeof b.point.x === "number" && typeof b.point.y === "number")
    return { kind: "point", x: b.point.x, y: b.point.y };
  throw new Error(`beat at ${b.at} (${b.do}) needs a "ref" or a "point"`);
};

export function generateTimeline(spec) {
  for (const req of ["fps", "width", "height", "durationMs", "out"]) {
    if (spec?.[req] == null) throw new Error(`spec missing required field "${req}"`);
  }
  if (60 % spec.fps !== 0) throw new Error(`fps ${spec.fps} must divide 60`);
  const park = spec.park ?? { x: Math.round(spec.width / 2), y: Math.round(spec.height / 2) };
  const defaultId = spec.emitterId ?? 0;
  const events = [];
  const cursor = [];
  let group = 0; // one group per beat: envelopes from different beats must not interleave

  const addClick = (t, target) => {
    const g = group;
    cursor.push({ g, t: t - CLICK_APPROACH_MS, target, vis: true, press: false });
    cursor.push({ g, t, target, vis: true, press: true, activate: true });
    cursor.push({ g, t: t + CLICK_HOLD_MS, target, vis: true, press: false, activate: true });
  };
  const addHover = (t0, t1, target) => {
    const g = group;
    cursor.push({ g, t: t0, target, vis: true, press: false });
    cursor.push({ g, t: t1, target, vis: true, press: false });
  };

  for (const b of spec.beats ?? []) {
    if (typeof b.at !== "number") throw new Error(`beat missing "at": ${JSON.stringify(b)}`);
    group++;
    switch (b.do) {
      case "click": {
        addClick(b.at, beatTarget(b));
        break;
      }
      case "drag": {
        // A real §4b reorder gesture: pointerdown on the grabbed row, move while
        // pressed (past the 4px threshold -> chip + gap), release at the drop.
        // Grab by testid ref; drop on a ref for a row-relative gap or a point for
        // a deterministic edge gap. NO activate — a press is either a drag or an
        // activation, never both.
        if (!b.from || !b.to) throw new Error(`drag beat at ${b.at} needs "from" and "to" targets`);
        const from = beatTarget(b.from);
        const to = beatTarget(b.to);
        const travelMs = b.travelMs ?? 600;
        const g = group;
        cursor.push({ g, t: b.at - CLICK_APPROACH_MS, target: from, vis: true, press: false });
        cursor.push({ g, t: b.at, target: from, vis: true, press: true });                      // grab
        cursor.push({ g, t: b.at + 200, target: from, vis: true, press: true });                // dwell: the grab reads
        cursor.push({ g, t: b.at + 200 + travelMs, target: to, vis: true, press: true });       // travel, chip + gap live
        cursor.push({ g, t: b.at + 350 + travelMs, target: to, vis: true, press: true });       // settle on the gap
        cursor.push({ g, t: b.at + 500 + travelMs, target: to, vis: true, press: false });      // drop
        break;
      }
      case "type": {
        if (typeof b.ref !== "string") throw new Error(`type beat at ${b.at} needs the input's "ref"`);
        if (typeof b.text !== "string" || b.text.length === 0) throw new Error(`type beat at ${b.at} needs "text"`);
        const field = b.field ?? "name";
        const charMs = b.charMs ?? 90;
        const id = b.id ?? defaultId;
        const target = elTarget(b.ref);
        addClick(b.at, target); // focus-click; a later click elsewhere commits (blur)
        const first = b.at + TYPE_FOCUS_GAP_MS;
        for (let i = 1; i <= b.text.length; i++) {
          events.push({ at: first + (i - 1) * charMs, kind: "emitters/set-properties",
                        params: { id, patch: { [field]: b.text.slice(0, i) } } });
        }
        // hold on the field through the typing so the caret context reads right
        cursor.push({ g: group, t: first + b.text.length * charMs + 200, target, vis: true, press: false });
        break;
      }
      case "step": {
        if (!Array.isArray(b.values) || b.values.length === 0) throw new Error(`step beat at ${b.at} needs "values"`);
        if (typeof b.field !== "string") throw new Error(`step beat at ${b.at} needs "field"`);
        const stepMs = b.stepMs ?? 400;
        const id = b.id ?? defaultId;
        for (let i = 0; i < b.values.length; i++) {
          events.push({ at: b.at + i * stepMs, kind: "emitters/set-properties",
                        params: { id, patch: { [b.field]: b.values[i] } } });
        }
        if (b.hover) addHover(b.at - HOVER_LEAD_MS, b.at + b.values.length * stepMs, beatTarget(b.hover));
        break;
      }
      case "step-key": {
        if (!Array.isArray(b.values) || b.values.length === 0) throw new Error(`step-key beat at ${b.at} needs "values"`);
        if (typeof b.track !== "string" || typeof b.keyTime !== "number")
          throw new Error(`step-key beat at ${b.at} needs "track" and "keyTime"`);
        const stepMs = b.stepMs ?? 400;
        const id = b.id ?? defaultId;
        for (let i = 0; i < b.values.length; i++) {
          events.push({ at: b.at + i * stepMs, kind: "emitters/set-track-key",
                        params: { id, track: b.track, oldTime: b.keyTime, newTime: b.keyTime, newValue: b.values[i] } });
        }
        if (b.hover) addHover(b.at - HOVER_LEAD_MS, b.at + b.values.length * stepMs, beatTarget(b.hover));
        break;
      }
      case "focus-channel": {
        if (typeof b.channel !== "string") throw new Error(`focus-channel beat at ${b.at} needs "channel"`);
        events.push({ at: b.at, kind: "ui/focus-channel", params: { channel: b.channel } });
        if (b.click) addClick(b.at + 500, elTarget(`channel-row:${b.channel}`));
        break;
      }
      case "select-key": {
        if (typeof b.track !== "string" || typeof b.time !== "number")
          throw new Error(`select-key beat at ${b.at} needs "track" and "time"`);
        events.push({ at: b.at, kind: "ui/select-key", params: { track: b.track, time: b.time } });
        // hover: true auto-targets the key's own ref; an object overrides (same
        // {ref|point} shape as step/step-key hovers).
        if (b.hover === true) addHover(b.at - 300, b.at + 450, elTarget(`curve-key:${b.track}:${b.time}`));
        else if (b.hover) addHover(b.at - 300, b.at + 450, beatTarget(b.hover));
        break;
      }
      case "show-panel": {
        events.push({ at: b.at, kind: "ui/show-panel", params: { panel: b.panel ?? null } });
        break;
      }
      case "hover": {
        addHover(b.at, b.at + (b.holdMs ?? 400), beatTarget(b));
        break;
      }
      case "event": {
        if (typeof b.kind !== "string") throw new Error(`event beat at ${b.at} needs "kind"`);
        events.push({ at: b.at, kind: b.kind, params: b.params ?? {} });
        break;
      }
      default:
        throw new Error(`unknown beat "do": ${JSON.stringify(b.do)} at ${b.at}`);
    }
  }

  events.push(...(spec.prelude ?? []));
  events.sort((a, b) => a.at - b.at);

  const tracks = [...events, ...(spec.tweens ?? [])];

  if (cursor.length > 0) {
    // Two beats whose cursor envelopes interleave would zigzag the cursor and can
    // smear a press across targets (an accidental drag) — fail loud instead.
    const spans = new Map();
    for (const k of cursor) {
      const s = spans.get(k.g) ?? { min: k.t, max: k.t };
      s.min = Math.min(s.min, k.t);
      s.max = Math.max(s.max, k.t);
      spans.set(k.g, s);
    }
    const ordered = [...spans.values()].sort((a, b) => a.min - b.min);
    for (let i = 1; i < ordered.length; i++) {
      if (ordered[i].min < ordered[i - 1].max)
        throw new Error(`cursor envelopes collide: a beat starting at t=${ordered[i].min} overlaps the previous ` +
          `beat's envelope (ends t=${ordered[i - 1].max}) — space the beats apart (clicks reserve ` +
          `${CLICK_APPROACH_MS}ms approach + ${CLICK_HOLD_MS}ms hold; hovers ${HOVER_LEAD_MS}ms lead)`);
    }
    cursor.sort((a, b) => a.t - b.t);
    const first = cursor[0].t;
    const last = cursor[cursor.length - 1].t;
    const parkT = { kind: "point", x: park.x, y: park.y };
    const head = [
      { t: 0, target: parkT, vis: false, press: false },
      { t: Math.max(1, first - 900), target: parkT, vis: false, press: false },
      { t: Math.max(2, first - 700), target: parkT, vis: true, press: false },
    ];
    const tail = [
      { t: Math.min(last + 700, spec.durationMs - 200), target: parkT, vis: true, press: false },
      { t: Math.min(last + 900, spec.durationMs - 100), target: parkT, vis: false, press: false },
    ];
    const keys = [...head, ...cursor.map(({ g, ...k }) => k), ...tail];
    for (let i = 1; i < keys.length; i++) {
      if (keys[i].t < keys[i - 1].t)
        throw new Error(`cursor keys out of order at t=${keys[i].t} (a beat starts before the head/tail park envelope allows)`);
    }
    tracks.push({ cursor: keys });
  }

  // The beat table IS the clip's documentation — auto-derive the _comment when
  // the spec doesn't supply one, carrying each beat's `expect` so the contact
  // sheet and the next author can check frames against stated intent.
  const autoComment = (spec.beats ?? [])
    .map((b) => {
      const target = b.ref ?? (b.point ? `(${b.point.x},${b.point.y})` : b.channel ?? b.track ?? b.kind ?? "");
      return `${b.at} ${b.do} ${target}${Array.isArray(b.values) ? ` ${b.values.join("→")}` : ""}` +
        `${b.text ? ` "${b.text}"` : ""}${b.expect ? ` => ${b.expect}` : ""}`;
    })
    .join("; ");

  const timeline = {
    ...(spec.comment ? { _comment: spec.comment } : autoComment ? { _comment: `beats: ${autoComment}` } : {}),
    ...(spec.open ? { open: spec.open } : {}),
    fps: spec.fps, width: spec.width, height: spec.height,
    durationMs: spec.durationMs, out: spec.out,
    ...(spec.openSettleMs != null ? { openSettleMs: spec.openSettleMs } : {}),
    ...(spec.saveRoot ? { saveRoot: spec.saveRoot } : {}),
    ...(spec.scale != null ? { scale: spec.scale } : {}),
    tracks,
  };
  return timeline;
}

export function parseGenArgs(argv) {
  let outPath = null;
  const inputs = [];
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--help") return { help: true };
    if (a === "--out") {
      if (outPath != null) throw new Error("--out given twice");
      const v = argv[++i];
      if (v == null || v.startsWith("--")) throw new Error("--out requires a path");
      outPath = v;
    } else if (a.startsWith("--")) {
      throw new Error(`unknown option: ${a}`);
    } else {
      inputs.push(a);
    }
  }
  if (inputs.length !== 1) throw new Error("exactly one <beats.json> input is required");
  return { input: inputs[0], outPath };
}

function main(argv) {
  let parsed;
  try {
    parsed = parseGenArgs(argv);
  } catch (e) {
    console.error(USAGE);
    console.error(e.message);
    process.exit(2);
  }
  if (parsed.help) {
    console.log(USAGE);
    return;
  }
  const { input, outPath } = parsed;
  try {
    const spec = JSON.parse(readFileSync(input, "utf8"));
    const json = JSON.stringify(generateTimeline(spec), null, 2) + "\n";
    if (outPath) {
      writeFileSync(outPath, json);
      console.log(`wrote ${outPath} — lint it: node scripts/clip-verify/timeline-lint.mjs ${outPath}`);
    } else {
      process.stdout.write(json);
    }
  } catch (e) {
    console.error(e.message);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
