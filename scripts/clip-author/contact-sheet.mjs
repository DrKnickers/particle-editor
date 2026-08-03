#!/usr/bin/env node
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";
import { join, relative, dirname, basename, isAbsolute } from "node:path";

const USAGE =
  "usage: node contact-sheet.mjs --frames <dir> --fps <n> (--beats <beats.json> | --timeline <t.json>) [--out <html>]";

// Beat contact sheet: one labeled frame per beat (plus the opening and final
// frames) as a single HTML page — the "watch protocol" for a rendered clip.
// Aesthetics are the one layer no gate checks; this makes reviewing them a
// five-second glance instead of a video watch per iteration. The rendered
// frames already exist as PNGs (frame_%05d.png), so this only picks files and
// writes HTML — no ffmpeg.
//
// Beats mode (preferred): samples each beat's `at` plus a mid/end point for
// walks, and carries the beat's `expect` text into the caption so the reviewer
// (human or model) checks each frame against the stated intent.
// Timeline mode (fallback for hand-written timelines): samples each press
// down-edge and each at-event kind, deduped.

export function samplesFromBeats(spec) {
  const out = [{ ms: 0, label: "open (frame 0)" }];
  for (const b of spec.beats ?? []) {
    if (typeof b.at !== "number") continue;
    const target = b.ref ?? (b.point ? `(${b.point.x},${b.point.y})` : b.channel ?? b.track ?? b.kind ?? "");
    const expect = b.expect ? ` — expect: ${b.expect}` : "";
    // `expect` describes the RESULT — for delayed actions (typing starts 700ms
    // after the click, walks step over time) attach it to the endpoint sample
    // only, or the sheet asks the reviewer to verify a state that can't exist yet.
    const delayed = Array.isArray(b.values) || (b.do === "type" && typeof b.text === "string");
    out.push({ ms: b.at, label: `${b.do} ${target}${delayed ? "" : expect}` });
    if (Array.isArray(b.values)) {
      const endMs = b.at + b.values.length * (b.stepMs ?? 400);
      out.push({ ms: endMs, label: `${b.do} ${target} = ${b.values[b.values.length - 1]} (walk done)${expect}` });
    }
    if (b.do === "type" && typeof b.text === "string") {
      const endMs = b.at + 700 + b.text.length * (b.charMs ?? 90);
      out.push({ ms: endMs, label: `typed "${b.text}"${expect}` });
    }
  }
  if (typeof spec.durationMs === "number") out.push({ ms: spec.durationMs - 1, label: "final frame" });
  return out;
}

export function samplesFromTimeline(timeline) {
  const out = [{ ms: 0, label: "open (frame 0)" }];
  const cursor = (timeline.tracks ?? []).find((t) => Array.isArray(t.cursor))?.cursor ?? [];
  let prevPress = false;
  for (const k of cursor) {
    if (k.press === true && !prevPress)
      out.push({ ms: k.t, label: `press ${k.target?.ref ?? "point"}${k.activate ? " (activate)" : ""}` });
    prevPress = k.press === true;
  }
  for (const ev of (timeline.tracks ?? []).filter((t) => typeof t.at === "number" && typeof t.kind === "string"))
    out.push({ ms: ev.at, label: ev.kind });
  if (typeof timeline.durationMs === "number") out.push({ ms: timeline.durationMs - 1, label: "final frame" });
  out.sort((a, b) => a.ms - b.ms);
  return out;
}

// Frame N covers virtual time [N/fps, (N+1)/fps) — floor gives the frame that
// CONTAINS the timestamp (round would show frame 1 for ms=9 at 60fps, a frame
// whose time hasn't arrived). Endpoint samples land after their change by
// construction, so containing-frame is right for them too.
export function frameIndexForMs(ms, fps, frameCount) {
  return Math.max(0, Math.min(frameCount - 1, Math.floor((ms / 1000) * fps)));
}

export function renderContactSheet(samples, { frameFiles, fps, title, imgDirRel }) {
  const cells = [];
  const seen = new Set();
  for (const s of samples) {
    const idx = frameIndexForMs(s.ms, fps, frameFiles.length);
    const key = `${idx}:${s.label}`;
    if (seen.has(key)) continue; // identical frame+label adds nothing
    seen.add(key);
    const src = imgDirRel ? `${imgDirRel}/${frameFiles[idx]}` : frameFiles[idx];
    cells.push(
      `<figure><img loading="lazy" src="${src}" alt="frame ${idx}">` +
      `<figcaption><b>${(s.ms / 1000).toFixed(1)}s</b> · f${idx} · ${escapeHtml(s.label)}</figcaption></figure>`,
    );
  }
  return `<!doctype html><meta charset="utf-8"><title>${escapeHtml(title)}</title>
<style>
body{background:#111;color:#ddd;font:13px system-ui;margin:16px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:12px}
figure{margin:0}img{width:100%;border:1px solid #333;border-radius:4px}
figcaption{padding:4px 2px;line-height:1.35}
</style>
<h1 style="font-size:16px">${escapeHtml(title)} — beat contact sheet</h1>
<div class="grid">
${cells.join("\n")}
</div>
`;
}

function escapeHtml(s) {
  return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function parseArgs(argv) {
  const args = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--help") return { help: true };
    if (!a.startsWith("--")) throw new Error(`unexpected argument: ${a}`);
    const key = a.slice(2);
    const val = argv[++i];
    if (val == null || val.startsWith("--")) throw new Error(`--${key} requires a value`);
    args[key] = val;
  }
  return args;
}

function main(argv) {
  let args;
  try {
    args = parseArgs(argv);
  } catch (e) {
    console.error(USAGE);
    console.error(e.message);
    process.exit(2);
  }
  if (args.help) { console.log(USAGE); return; }
  if (!args.frames || !args.fps || (!args.beats && !args.timeline)) {
    console.error(USAGE);
    process.exit(2);
  }
  try {
    const fps = Number(args.fps);
    if (!Number.isFinite(fps) || fps <= 0) throw new Error("--fps must be a positive number");
    const frameFiles = readdirSync(args.frames).filter((f) => /^frame_\d+\.png$/.test(f)).sort();
    if (frameFiles.length === 0) throw new Error(`no frame_*.png in ${args.frames}`);
    const samples = args.beats
      ? samplesFromBeats(JSON.parse(readFileSync(args.beats, "utf8")))
      : samplesFromTimeline(JSON.parse(readFileSync(args.timeline, "utf8")));
    const outPath = args.out ?? join(args.frames, "contact-sheet.html");
    // Cross-drive --out on Windows: path.relative returns an ABSOLUTE path
    // (D:\frames), which is not a usable relative URL — fall back to file:// URLs.
    const rel = relative(dirname(outPath), args.frames);
    const imgDirRel = isAbsolute(rel) ? pathToFileURL(args.frames).href : rel.replaceAll("\\", "/");
    const html = renderContactSheet(samples, {
      frameFiles, fps,
      title: basename(args.frames),
      imgDirRel: imgDirRel === "" ? null : imgDirRel,
    });
    writeFileSync(outPath, html);
    console.log(`wrote ${outPath} (${samples.length} samples over ${frameFiles.length} frames)`);
  } catch (e) {
    console.error(e.message);
    process.exit(1);
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  main(process.argv.slice(2));
}
