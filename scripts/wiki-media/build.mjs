#!/usr/bin/env node
import { spawnSync } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, extname, isAbsolute, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { inflateSync } from "node:zlib";

const DEFAULT_FPS = 60;
const DEFAULT_CRF = 16;
// Stage 3 (headless composite record): the capture is now the client area 1:1 —
// 1264x951, no native border (X=0) and the branded frameless title bar at the top
// (part of the app). Legacy foreground PrintWindow was 1264x952 with an 8px left
// border (X=8). Height rounded DOWN to an even 950 so the crop is a clean pixel
// crop (libx264 4:2:0 needs even dims; an odd 951 would force encode.mjs's
// scale=trunc(ih/2)*2 to RESAMPLE 951->950, blurring the frame).
const DEFAULT_CROP = "1264:950:0:0";
// Exact dims of the headless composite capture (client area 1:1). Every clip crop
// must fit inside these — the preflight validates DEFAULT_CROP against them.
const HEADLESS_CAPTURE_W = 1264;
const HEADLESS_CAPTURE_H = 951;
const DEFAULT_START = 0;
const DEFAULT_CROSSFADE = 1.0;
const DEFAULT_LULL_LUMA_THRESHOLD = 3.0;
const VALID_LOOPS = new Set(["pingpong", "crossfade", "none"]);
const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const DEFAULT_REPO_ROOT = resolve(__dirname, "..", "..");

function hasOwn(object, key) {
  return Object.prototype.hasOwnProperty.call(object, key);
}

function readJson(file) {
  return JSON.parse(readFileSync(file, "utf8"));
}

function writeJson(file, value) {
  mkdirSync(dirname(file), { recursive: true });
  writeFileSync(file, JSON.stringify(value, null, 2) + "\n");
}

function log(logger, message) {
  if (logger) logger(message);
}

function repoRelative(repoRoot, file) {
  return relative(repoRoot, file).replace(/\\/g, "/");
}

function resolveFrom(base, value) {
  if (!value) return value;
  return isAbsolute(value) ? value : resolve(base, value);
}

function quotePart(part) {
  var text = String(part);
  if (text.length === 0 || /\s/.test(text)) return JSON.stringify(text);
  return text;
}

function commandString(bin, args) {
  return [bin].concat(args).map(quotePart).join(" ");
}

function descriptorCommand(desc, args) {
  return commandString(desc.bin, (desc.prefixArgs || []).concat(args));
}

export function parseCliArgs(argv) {
  var parsed = {};
  for (var i = 0; i < argv.length; i++) {
    var arg = argv[i];
    if (arg === "--only") {
      var only = argv[++i];
      if (!only || only.startsWith("--")) throw new Error("--only requires a comma-separated id list");
      parsed.only = only;
    } else if (arg === "--skip-render") {
      parsed.skipRender = true;
    } else if (arg === "--skip-stage") {
      parsed.skipStage = true;
    } else if (arg === "--dry-run") {
      parsed.dryRun = true;
    } else if (arg === "--report") {
      var report = argv[++i];
      if (!report || report.startsWith("--")) throw new Error("--report requires a path");
      parsed.report = report;
    } else if (arg === "--help") {
      parsed.help = true;
    } else {
      throw new Error("unknown argument: " + arg);
    }
  }
  return parsed;
}

function normalizeArgs(args) {
  args = args || {};
  var only = null;
  if (args.only instanceof Set) only = args.only;
  else if (typeof args.only === "string" && args.only.length > 0) {
    only = new Set(args.only.split(",").map(function(id) { return id.trim(); }).filter(Boolean));
  }
  return {
    only: only,
    skipRender: Boolean(args.skipRender),
    skipStage: Boolean(args.skipStage),
    dryRun: Boolean(args.dryRun),
    report: args.report || null,
  };
}

export function validateManifest(manifest) {
  var errors = [];
  if (!manifest || !Array.isArray(manifest.items)) return ["manifest must contain an items array"];
  manifest.items.forEach(function(item, index) {
    var label = item && item.id ? item.id : "items[" + index + "]";
    if (!item || typeof item !== "object") {
      errors.push(label + ": item must be an object");
      return;
    }
    if (!item.id) errors.push(label + ": missing required id");
    if (hasOwn(item, "postprocess")) errors.push(label + ": postprocess is removed in manifest v2");
    if (item.manual) return;
    // A planned backlog item without a timeline is a production TODO, not yet
    // renderable — exempt it so a backlog entry can't wedge the whole batch.
    // Once it gains a timeline (or leaves "planned") the full field
    // requirements apply. Selecting one anyway reports PENDING (processItem).
    if (item.status === "planned" && !item.timeline) return;
    var kind = item.kind || "clip";
    // Every non-manual pipeline item is rendered from a timeline — require it up
    // front so a missing field fails validation loudly, not mid-pipeline.
    if (!item.timeline) errors.push(label + ": missing required timeline");
    if (kind === "clip") {
      if (item.framing == null || item.framing === "") errors.push(label + ": missing required framing");
      if (item.loop == null || item.loop === "") errors.push(label + ": missing required loop");
      else if (!VALID_LOOPS.has(item.loop)) errors.push(label + ": unknown loop \"" + item.loop + "\"");
      if (!item.output) errors.push(label + ": missing required output");
    } else if (kind === "image") {
      if (item.framing == null || item.framing === "") errors.push(label + ": missing required framing");
      if (item.frame == null) errors.push(label + ": image item requires frame");
      if (!item.output) errors.push(label + ": missing required output");
    } else {
      errors.push(label + ": unknown kind \"" + kind + "\"");
    }
  });
  return errors;
}

// Tutorial media must never show a reference object (the editor's default
// AT_ST_Walker would float in every shot otherwise). Every clip/image timeline
// is required to clear it: an `engine/set/reference-object {name:""}` event AND
// an `engine/set/reference-object-visible {visible:false}` event. A manifest item
// may opt in only when the reference object itself is the documented subject.
// Enforced per item before render so a missing clear fails loudly, not in review.
export function validateTimelineNoReferenceObject(timeline, label, allowReferenceObject = false) {
  if (allowReferenceObject) return [];
  var tracks = Array.isArray(timeline.tracks) ? timeline.tracks : [];
  var cleared = false;
  var hidden = false;
  var setsName = false;    // any event that sets a NON-empty reference object
  var setsVisible = false; // any event that makes the reference object visible
  tracks.forEach(function(tr) {
    if (!tr || typeof tr !== "object" || !tr.kind) return;
    if (tr.kind === "engine/set/reference-object" && tr.params) {
      if (tr.params.name === "") cleared = true;
      else if (typeof tr.params.name === "string" && tr.params.name !== "") setsName = true;
    }
    if (tr.kind === "engine/set/reference-object-visible" && tr.params) {
      if (tr.params.visible === false) hidden = true;
      else if (tr.params.visible === true) setsVisible = true;
    }
  });
  var errors = [];
  if (!cleared) errors.push(label + ": timeline must clear the reference object (engine/set/reference-object {name:\"\"})");
  if (!hidden) errors.push(label + ": timeline must hide the reference object (engine/set/reference-object-visible {visible:false})");
  // A later event that re-sets a name or re-shows it would put a reference object
  // on screen despite the earlier clears — reject regardless of clear/hide order.
  if (setsName) errors.push(label + ": timeline sets a non-empty reference object (tutorial media must show none)");
  if (setsVisible) errors.push(label + ": timeline makes the reference object visible (tutorial media must show none)");
  return errors;
}

export function loadConfig(options) {
  options = options || {};
  var repoRoot = options.repoRoot || DEFAULT_REPO_ROOT;
  var localPath = options.configLocalPath || join(repoRoot, "scripts", "wiki-media", "config.local.json");
  var examplePath = options.configExamplePath || join(repoRoot, "scripts", "wiki-media", "config.example.json");
  if (existsSync(localPath)) return readJson(localPath);
  if (options.dryRun) return readJson(examplePath);
  throw new Error("missing scripts/wiki-media/config.local.json; create it from config.example.json or use --dry-run");
}

function defaultTools(repoRoot, config) {
  var node = process.execPath;
  function script() { return join.apply(null, [repoRoot].concat(Array.from(arguments))); }
  return {
    render: { bin: resolveFrom(repoRoot, config.exe), prefixArgs: [] },
    stage: { bin: "powershell.exe", prefixArgs: ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script("scripts", "wiki-media", "stage-assets.ps1")] },
    cursor: { bin: node, prefixArgs: [script("scripts", "clip-verify", "cursor-on-target.mjs")] },
    partial: { bin: node, prefixArgs: [script("scripts", "clip-verify", "partial-scan.mjs")] },
    seam: { bin: node, prefixArgs: [script("scripts", "clip-verify", "seam-churn.mjs")] },
    encode: { bin: node, prefixArgs: [script("scripts", "clip-encode", "encode.mjs")] },
    ffmpeg: { bin: "ffmpeg", prefixArgs: [] },
    env: {},
  };
}

function mergeTools(repoRoot, config, overrides) {
  var merged = Object.assign({}, defaultTools(repoRoot, config), overrides || {});
  merged.env = Object.assign({}, overrides && overrides.env ? overrides.env : {});
  return merged;
}

function runTool(tools, name, args, options) {
  options = options || {};
  var desc = tools[name];
  var fullArgs = (desc.prefixArgs || []).concat(args);
  var env = Object.assign({}, process.env, tools.env || {}, desc.env || {}, options.env || {});
  var result = spawnSync(desc.bin, fullArgs, { cwd: options.cwd, encoding: "utf8", env: env, windowsHide: true });
  return {
    command: commandString(desc.bin, fullArgs),
    status: result.error ? 1 : (result.status == null ? 1 : result.status),
    stdout: result.stdout || "",
    stderr: (result.stderr || "") + (result.error ? "\n" + result.error.message : ""),
    error: result.error || null,
  };
}

function defaultGates() {
  return {
    "cursor-on-target": "skipped",
    "partial-scan": "skipped",
    "seam-churn": "skipped",
    "lull-luma": "skipped",
    readability: "pending",
    "no-reference-object": "skipped",
  };
}

function reportEntry(item) {
  return {
    id: item.id,
    status: "FAIL",
    commands: { render: "", verify: [], encode: "" },
    exit: {},
    gates: defaultGates(),
    outputs: [],
    acceptance: Array.isArray(item.acceptance) ? item.acceptance : [],
    rerender: "node scripts/wiki-media/build.mjs --only " + item.id,
  };
}

function effectiveEncode(item) {
  var encode = item.encode || {};
  return {
    fps: encode.fps == null ? DEFAULT_FPS : encode.fps,
    crf: encode.crf == null ? DEFAULT_CRF : encode.crf,
    crop: encode.crop == null ? DEFAULT_CROP : encode.crop,
    start: encode.start == null ? DEFAULT_START : encode.start,
    crossfade: encode.crossfade == null ? DEFAULT_CROSSFADE : encode.crossfade,
    posterFrame: encode.posterFrame,
    dropBlackBelow: encode.dropBlackBelow,
    lullLumaThreshold: encode.lullLumaThreshold == null ? DEFAULT_LULL_LUMA_THRESHOLD : encode.lullLumaThreshold,
    // Dynamic zoom segments ([{t0,t1,rect,easeMs}], post-trim px / source seconds).
    // Deep validation lives in encode.mjs assertZoom — build.mjs only threads the
    // segments through with the post-trim frame size derived from the crop.
    zoom: encode.zoom,
  };
}

function parseCrop(crop) {
  var parts = String(crop).split(":").map(Number);
  if (parts.length !== 4 || !parts.every(Number.isFinite)) throw new Error("invalid crop \"" + crop + "\": expected W:H:X:Y");
  return { w: parts[0], h: parts[1], x: parts[2], y: parts[3] };
}

function framePath(frameDir, frameNumber) {
  return join(frameDir, "frame_" + String(frameNumber).padStart(5, "0") + ".png");
}

function listFrameNumbers(frameDir) {
  if (!existsSync(frameDir)) return [];
  return readdirSync(frameDir).map(function(name) { return /^frame_(\d+)\.png$/i.exec(name); }).filter(Boolean).map(function(match) { return Number(match[1]); }).sort(function(a, b) { return a - b; });
}

function resolveTimelinePath(repoRoot, item) {
  if (!item.timeline) throw new Error(item.id + ": missing timeline");
  return resolveFrom(repoRoot, item.timeline);
}

function resolveFrameDir(repoRoot, timelinePath, timeline) {
  if (!timeline.out) throw new Error(timelinePath + ": missing out frame directory");
  return isAbsolute(timeline.out) ? timeline.out : resolve(repoRoot, timeline.out);
}

function hasTargetKey(value) {
  if (!value || typeof value !== "object") return false;
  if (hasOwn(value, "target")) return true;
  if (Array.isArray(value)) return value.some(hasTargetKey);
  return Object.values(value).some(hasTargetKey);
}

export function timelineHasTargetBearingCursorTrack(timeline) {
  var tracks = Array.isArray(timeline && timeline.tracks) ? timeline.tracks : [];
  return tracks.some(function(track) {
    var looksLikeCursor = track && (track.kind === "cursor" || track.type === "cursor" || hasOwn(track, "cursor") || hasOwn(track, "points") || hasOwn(track, "elements"));
    return looksLikeCursor && hasTargetKey(track);
  });
}

function isBurstLull(item) {
  return Boolean(item.encode && item.encode.lullLuma) || /burst-lull/i.test(String(item.notes || ""));
}

export function buildGatePlan(options) {
  var item = options.item;
  var timeline = options.timeline;
  var frameDir = options.frameDir;
  var sidecarPath = options.sidecarPath;
  var repoRoot = options.repoRoot || DEFAULT_REPO_ROOT;
  var activeTools = options.tools || defaultTools(repoRoot, { exe: "" });
  var encode = effectiveEncode(item);
  var statuses = defaultGates();
  // no-reference-object is decided BEFORE the gate plan runs (timeline
  // validation in processItem); the plan must not clobber its verdict.
  delete statuses["no-reference-object"];
  var commands = [];

  if (timelineHasTargetBearingCursorTrack(timeline)) {
    statuses["cursor-on-target"] = "pending";
    var cursorArgs = ["--sidecar", sidecarPath];
    commands.push({ name: "cursor-on-target", tool: "cursor", args: cursorArgs, command: descriptorCommand(activeTools.cursor, cursorArgs) });
  }

  statuses["partial-scan"] = "pending";
  var partialArgs = ["--frames", frameDir, "--start", String(encode.start), "--crop", encode.crop];
  commands.push({ name: "partial-scan", tool: "partial", args: partialArgs, command: descriptorCommand(activeTools.partial, partialArgs) });

  if (item.loop === "crossfade") {
    statuses["seam-churn"] = "pending";
    var crossfadeFrames = Math.round(Number(encode.crossfade) * Number(encode.fps));
    var seamArgs = ["--frames", frameDir, "--fps", String(encode.fps), "--start", String(encode.start), "--crossfade-frames", String(crossfadeFrames), "--roi", encode.crop];
    commands.push({ name: "seam-churn", tool: "seam", args: seamArgs, command: descriptorCommand(activeTools.seam, seamArgs) });
    if (isBurstLull(item)) statuses["lull-luma"] = "pending";
  }

  return { statuses: statuses, commands: commands };
}

function copyStageInputs(config, item, repoRoot) {
  var base = join(resolveFrom(repoRoot, config.focMods), "ParticleTutorial");
  // item.id becomes a directory name — reject anything that isn't a plain slug so a
  // hostile/typo'd id can't escape the _scratch subtree.
  if (!/^[A-Za-z0-9._-]+$/.test(item.id)) throw new Error("unsafe item id for scratch dir: " + item.id);
  var scratchBase = join(base, "_scratch", item.id);
  (item.stageInputs || []).forEach(function(input) {
    if (typeof input !== "string" || input.indexOf("..") !== -1 || /^[\\/]/.test(input) || /^[A-Za-z]:/.test(input))
      throw new Error("unsafe stageInput (no '..'/absolute): " + input);
    var source = join(base, input);
    // Copy to the scratch root under the input's BASENAME — the mutating clip's
    // timeline opens `_scratch/<id>/<basename>` (flat), so preserving the input's
    // `_stages/…` prefix here would put the file where the timeline never looks.
    var target = join(scratchBase, basename(input));
    if (!existsSync(source)) throw new Error("stage input missing: " + source);
    mkdirSync(dirname(target), { recursive: true });
    if (statSync(source).isDirectory()) {
      rmSync(target, { recursive: true, force: true });
      cpSync(source, target, { recursive: true });
    } else {
      copyFileSync(source, target);
    }
  });
}

function paeth(a, b, c) {
  var p = a + b - c;
  var pa = Math.abs(p - a);
  var pb = Math.abs(p - b);
  var pc = Math.abs(p - c);
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

function parsePng(buffer) {
  if (buffer.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a") throw new Error("not a PNG file");
  var offset = 8;
  var width = 0;
  var height = 0;
  var bitDepth = 0;
  var colorType = 0;
  var idat = [];
  while (offset < buffer.length) {
    var length = buffer.readUInt32BE(offset);
    var type = buffer.subarray(offset + 4, offset + 8).toString("ascii");
    var data = buffer.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;
    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
    } else if (type === "IDAT") idat.push(data);
    else if (type === "IEND") break;
  }
  if (bitDepth !== 8) throw new Error("unsupported PNG bit depth " + bitDepth);
  var channels = colorType === 0 ? 1 : (colorType === 2 ? 3 : (colorType === 6 ? 4 : 0));
  if (!channels) throw new Error("unsupported PNG color type " + colorType);
  var inflated = inflateSync(Buffer.concat(idat));
  var stride = width * channels;
  var rows = [];
  var pos = 0;
  var previous = Buffer.alloc(stride);
  for (var y = 0; y < height; y++) {
    var filter = inflated[pos++];
    var raw = Buffer.from(inflated.subarray(pos, pos + stride));
    pos += stride;
    var recon = Buffer.alloc(stride);
    for (var x = 0; x < stride; x++) {
      var left = x >= channels ? recon[x - channels] : 0;
      var up = previous[x] || 0;
      var upLeft = x >= channels ? previous[x - channels] : 0;
      var predictor = 0;
      if (filter === 1) predictor = left;
      else if (filter === 2) predictor = up;
      else if (filter === 3) predictor = Math.floor((left + up) / 2);
      else if (filter === 4) predictor = paeth(left, up, upLeft);
      else if (filter !== 0) throw new Error("unsupported PNG filter " + filter);
      recon[x] = (raw[x] + predictor) & 255;
    }
    rows.push(recon);
    previous = recon;
  }
  return { width: width, height: height, colorType: colorType, channels: channels, rows: rows };
}

function pngMeanLuma(file, crop) {
  var png = parsePng(readFileSync(file));
  var rect = crop ? parseCrop(crop) : { w: png.width, h: png.height, x: 0, y: 0 };
  if (rect.x < 0 || rect.y < 0 || rect.w <= 0 || rect.h <= 0 || rect.x + rect.w > png.width || rect.y + rect.h > png.height) {
    throw new Error("crop " + rect.w + ":" + rect.h + ":" + rect.x + ":" + rect.y + " outside " + png.width + "x" + png.height);
  }
  var sum = 0;
  var count = 0;
  for (var y = rect.y; y < rect.y + rect.h; y++) {
    var row = png.rows[y];
    for (var x = rect.x; x < rect.x + rect.w; x++) {
      var i = x * png.channels;
      var r;
      var g;
      var b;
      if (png.colorType === 0) r = g = b = row[i];
      else { r = row[i]; g = row[i + 1]; b = row[i + 2]; }
      sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
      count++;
    }
  }
  return sum / Math.max(1, count);
}

function meanWindowLuma(frameDir, frames, crop) {
  if (frames.length === 0) throw new Error("lull-luma window has no frames");
  var total = 0;
  frames.forEach(function(frame) { total += pngMeanLuma(framePath(frameDir, frame), crop); });
  return total / frames.length;
}

function checkLullLuma(frameDir, item) {
  var encode = effectiveEncode(item);
  var frames = listFrameNumbers(frameDir).filter(function(frame) { return frame >= encode.start; });
  var crossfadeFrames = Math.round(Number(encode.crossfade) * Number(encode.fps));
  if (frames.length < crossfadeFrames * 2) return { ok: false, message: "not enough frames for two blend windows" };
  var lead = frames.slice(0, crossfadeFrames);
  var tail = frames.slice(-crossfadeFrames);
  // Local PNG decoding keeps this gate independent from ffmpeg and from another subprocess mock path.
  var leadLuma = meanWindowLuma(frameDir, lead, encode.crop);
  var tailLuma = meanWindowLuma(frameDir, tail, encode.crop);
  var threshold = Number(encode.lullLumaThreshold);
  return { ok: leadLuma <= threshold && tailLuma <= threshold, message: "lead=" + leadLuma.toFixed(2) + " tail=" + tailLuma.toFixed(2) + " threshold=" + threshold };
}

// Stage 3: clips render through the headless composite path (engine RT +
// CapturePreview UI, composited on the CPU) — occlusion-immune with the editor
// minimized, so a batch can run while the machine is in use. The crop
// (DEFAULT_CROP) is tuned to the headless capture dims, so this is the ONLY
// supported render mode; a foreground fallback would need its own crop.
function runRender(item, timelinePath, tools, repoRoot, entry) {
  var args = ["--record", timelinePath, "--record-minimized"];
  entry.commands.render = descriptorCommand(tools.render, args);
  var result = runTool(tools, "render", args, { cwd: repoRoot, env: { PE_RECORD_HEADLESS: "1" } });
  entry.exit.render = result.status;
  if (result.status !== 0) return { ok: false, message: "render exited " + result.status };
  return { ok: true };
}

function runVerify(item, timeline, frameDir, tools, repoRoot, entry) {
  var sidecarPath = join(frameDir, "cursor-sidecar.json");
  var plan = buildGatePlan({ item: item, timeline: timeline, frameDir: frameDir, sidecarPath: sidecarPath, repoRoot: repoRoot, tools: tools });
  entry.gates = Object.assign(entry.gates, plan.statuses);
  entry.commands.verify = plan.commands.map(function(command) { return command.command; });
  for (var i = 0; i < plan.commands.length; i++) {
    var gate = plan.commands[i];
    if (gate.name === "cursor-on-target" && !existsSync(sidecarPath)) {
      entry.gates[gate.name] = "fail";
      entry.exit[gate.name] = 1;
      return { ok: false, message: "target-bearing cursor track did not produce cursor-sidecar.json" };
    }
    var result = runTool(tools, gate.tool, gate.args, { cwd: repoRoot });
    entry.gates[gate.name] = result.status === 0 ? "pass" : "fail";
    if (result.status !== 0) {
      entry.exit[gate.name] = result.status;
      return { ok: false, message: gate.name + " exited " + result.status };
    }
  }
  if (entry.gates["lull-luma"] === "pending") {
    var verdict = checkLullLuma(frameDir, item);
    entry.gates["lull-luma"] = verdict.ok ? "pass" : "fail";
    if (!verdict.ok) return { ok: false, message: "lull-luma failed: " + verdict.message };
  }
  return { ok: true };
}

function clipOutputPaths(item, config, repoRoot) {
  var outDir = resolveFrom(repoRoot, config.outDir);
  return {
    outDir: outDir,
    clip: resolve(outDir, item.output),
    poster: item.poster ? resolve(outDir, item.poster) : null,
    review: resolve(outDir, "review", item.id + "-720w.mp4"),
  };
}

function runEncode(item, frameDir, config, tools, repoRoot, entry) {
  var encode = effectiveEncode(item);
  var paths = clipOutputPaths(item, config, repoRoot);
  mkdirSync(dirname(paths.clip), { recursive: true });
  if (paths.poster) mkdirSync(dirname(paths.poster), { recursive: true });
  var args = ["--frames", frameDir, "--fps", String(encode.fps), "--out", paths.clip, "--start", String(encode.start), "--loop", item.loop, "--crf", String(encode.crf), "--crop", encode.crop];
  if (item.loop === "crossfade") args.push("--crossfade", String(encode.crossfade));
  if (encode.dropBlackBelow != null) args.push("--drop-black-below", String(encode.dropBlackBelow));
  if (encode.zoom != null) {
    // Segments are authored in the post-trim space, so the frame size the zoom
    // math needs is exactly the chrome-trim crop's W:H.
    var zoomFrame = parseCrop(encode.crop);
    args.push("--zoom", JSON.stringify({ w: zoomFrame.w, h: zoomFrame.h, segments: encode.zoom }));
  }
  if (paths.poster) {
    args.push("--poster", paths.poster);
    if (encode.posterFrame == null) {
      entry.commands.encode = descriptorCommand(tools.encode, args.concat(["--poster-frame", "<missing>"]));
      entry.exit.encode = 1;
      return { ok: false, message: "encode.posterFrame is required when poster is requested" };
    }
    args.push("--poster-frame", String(encode.posterFrame));
  }
  entry.commands.encode = descriptorCommand(tools.encode, args);
  var result = runTool(tools, "encode", args, { cwd: repoRoot });
  if (result.status !== 0) {
    entry.exit.encode = result.status;
    return { ok: false, message: "encode exited " + result.status };
  }
  mkdirSync(dirname(paths.review), { recursive: true });
  var reviewArgs = ["-y", "-i", paths.clip, "-vf", "scale=720:-2", "-an", paths.review];
  var review = runTool(tools, "ffmpeg", reviewArgs, { cwd: repoRoot });
  entry.commands.verify.push(descriptorCommand(tools.ffmpeg, reviewArgs));
  if (review.status !== 0) {
    entry.exit.review = review.status;
    return { ok: false, message: "review ffmpeg exited " + review.status };
  }
  entry.outputs = [paths.clip, paths.poster, paths.review].filter(Boolean).map(function(file) { return repoRelative(repoRoot, file); });
  return { ok: true };
}

function runImageExport(item, frameDir, config, tools, repoRoot, entry) {
  var encode = effectiveEncode(item);
  if (extname(item.output).toLowerCase() !== ".png") {
    entry.exit.export = 1;
    return { ok: false, message: "pipeline image output must be .png" };
  }
  if (item.frame == null) {
    entry.exit.export = 1;
    return { ok: false, message: "image item requires frame" };
  }
  var outDir = resolveFrom(repoRoot, config.outDir);
  var output = resolve(outDir, item.output);
  mkdirSync(dirname(output), { recursive: true });
  var args = ["-y", "-i", framePath(frameDir, item.frame), "-vf", "crop=" + encode.crop, "-frames:v", "1", output];
  entry.commands.encode = descriptorCommand(tools.ffmpeg, args);
  var result = runTool(tools, "ffmpeg", args, { cwd: repoRoot });
  if (result.status !== 0) {
    entry.exit.export = result.status;
    return { ok: false, message: "image export exited " + result.status };
  }
  if (existsSync(output) && statSync(output).size >= 10 * 1024 * 1024) {
    entry.exit.export = 1;
    return { ok: false, message: "image export is 10 MB or larger" };
  }
  entry.outputs = [repoRelative(repoRoot, output)];
  return { ok: true };
}

function processPending(item) {
  var entry = reportEntry(item);
  entry.status = "PENDING";
  return entry;
}

function processDryRun(item, config, repoRoot) {
  var entry = processPending(item);
  if (!item.manual && (item.kind || "clip") === "clip" && item.output) {
    var paths = clipOutputPaths(item, config, repoRoot);
    entry.outputs = [paths.clip, paths.poster, paths.review].filter(Boolean).map(function(file) { return repoRelative(repoRoot, file); });
  }
  return entry;
}

function processItem(options) {
  var item = options.item;
  var config = options.config;
  var tools = options.tools;
  var repoRoot = options.repoRoot;
  var args = options.args;
  var logger = options.logger;
  if (item.manual) return processPending(item);
  // Planned backlog items with no timeline yet aren't renderable — report
  // PENDING (visible in the report/console) instead of failing mid-pipeline.
  if (item.status === "planned" && !item.timeline) {
    log(logger, item.id + ": planned backlog item (no timeline yet) — PENDING");
    return processPending(item);
  }
  if (args.dryRun) return processDryRun(item, config, repoRoot);
  var entry = reportEntry(item);
  try {
    var timelinePath = resolveTimelinePath(repoRoot, item);
    var timeline = readJson(timelinePath);
    var refErrors = validateTimelineNoReferenceObject(
      timeline,
      item.id,
      item.allowReferenceObject === true,
    );
    if (refErrors.length > 0) {
      entry.exit.render = 1;
      entry.gates["no-reference-object"] = "fail";
      log(logger, item.id + ": " + refErrors.join("; "));
      return entry;
    }
    entry.gates["no-reference-object"] = "pass";
    var frameDir = resolveFrameDir(repoRoot, timelinePath, timeline);
    if (item.mutates && !args.skipRender) copyStageInputs(config, item, repoRoot);
    if (args.skipRender) {
      if (!existsSync(frameDir)) {
        entry.exit.render = 1;
        return entry;
      }
      entry.commands.render = "skipped";
      entry.exit.render = 0;
    } else {
      var render = runRender(item, timelinePath, tools, repoRoot, entry);
      if (!render.ok) {
        log(logger, item.id + ": " + render.message);
        return entry;
      }
    }
    if ((item.kind || "clip") === "image") {
      var image = runImageExport(item, frameDir, config, tools, repoRoot, entry);
      if (!image.ok) {
        log(logger, item.id + ": " + image.message);
        return entry;
      }
      entry.status = "PASS";
      return entry;
    }
    var verify = runVerify(item, timeline, frameDir, tools, repoRoot, entry);
    if (!verify.ok) {
      log(logger, item.id + ": " + verify.message);
      return entry;
    }
    var encode = runEncode(item, frameDir, config, tools, repoRoot, entry);
    if (!encode.ok) {
      log(logger, item.id + ": " + encode.message);
      return entry;
    }
    entry.status = "PASS";
    return entry;
  } catch (err) {
    entry.exit.error = 1;
    log(logger, item.id + ": " + err.message);
    return entry;
  }
}

function runStageIfNeeded(args, repoRoot, tools, logger) {
  var stageScript = join(repoRoot, "scripts", "wiki-media", "stage-assets.ps1");
  if (args.skipStage || args.dryRun) return;
  if (!existsSync(stageScript)) {
    log(logger, "stage-assets.ps1 is missing; continuing without stage-assets");
    return;
  }
  var result = runTool(tools, "stage", [], { cwd: repoRoot });
  if (result.status !== 0) throw new Error("stage-assets.ps1 exited " + result.status);
}

function assertCropInBounds(crop, width, height) {
  var rect = parseCrop(crop);
  if (rect.x + rect.w > width || rect.y + rect.h > height) throw new Error("crop " + crop + " is outside " + width + "x" + height);
}

function runBatchPreflight(config, tools, repoRoot) {
  var exe = resolveFrom(repoRoot, config.exe);
  if (!existsSync(exe)) return;
  var probe = join(repoRoot, "tasks", "wiki-media", "tutorials", "_stage", "probe.timeline.json");
  var timeline = readJson(probe);
  var frameDir = resolveFrameDir(repoRoot, probe, timeline);
  // Preflight renders the probe FOREGROUND (NOT headless): its only job is to prove
  // the rasterization pin (put_RasterizationScale 1.00) + a non-black first frame. The
  // minimal probe timeline fails the headless capture path (exit 4) even though real
  // clips render headless fine, so keep the probe foreground. Crop bounds are validated
  // against the HEADLESS capture dims below (what every clip encode actually crops from).
  var result = runTool(tools, "render", ["--record", probe], { cwd: repoRoot });
  if (result.status !== 0) throw new Error("preflight render exited " + result.status);
  // The rasterization-pin proof lives in stdout for the MOCK exe but in a per-PID
  // host-record log file for the REAL exe (Log() -> %LOCALAPPDATA%\AloParticleEditor\
  // host-record-<pid>.log, not stdout). Check both: stdout+stderr, then the newest
  // host-record-*.log written by the probe render.
  var logs = result.stdout + "\n" + result.stderr;
  var appDataDir = join(process.env.LOCALAPPDATA || "", "AloParticleEditor");
  try {
    if (existsSync(appDataDir)) {
      var recLogs = readdirSync(appDataDir)
        .filter(function(f) { return /^host-record-.*\.log$/.test(f); })
        .map(function(f) { return { f: f, m: statSync(join(appDataDir, f)).mtimeMs }; })
        .sort(function(a, b) { return b.m - a.m; });
      if (recLogs.length > 0) logs += "\n" + readFileSync(join(appDataDir, recLogs[0].f), "utf8");
    }
  } catch (e) { /* best-effort: fall through to the assertion on stdout alone */ }
  if (!/put_RasterizationScale\(1\.00\).*hr=0x00000000/s.test(logs)) throw new Error("preflight did not log put_RasterizationScale(1.00) hr=0x00000000 (checked stdout + newest host-record log)");
  var first = framePath(frameDir, 0);
  var png = parsePng(readFileSync(first));
  if (png.width <= 0 || png.height <= 0) {
    throw new Error("preflight frame is degenerate: " + png.width + "x" + png.height);
  }
  // Validate the clip crop against the HEADLESS capture dims (1264x951) — the frame
  // every clip encode crops from — NOT the foreground probe's own size.
  assertCropInBounds(DEFAULT_CROP, HEADLESS_CAPTURE_W, HEADLESS_CAPTURE_H);
  if (pngMeanLuma(first) <= 1.0) throw new Error("preflight first frame is all-black");
}

export async function runBatch(options) {
  options = options || {};
  var repoRoot = options.repoRoot ? resolve(options.repoRoot) : DEFAULT_REPO_ROOT;
  var args = normalizeArgs(options.args);
  var config = options.config || loadConfig({ repoRoot: repoRoot, dryRun: args.dryRun, configLocalPath: options.configLocalPath, configExamplePath: options.configExamplePath });
  var manifestPath = options.manifestPath || join(repoRoot, "tasks", "wiki-media", "manifest.json");
  var manifest = readJson(manifestPath);
  var validationErrors = validateManifest(manifest);
  if (validationErrors.length > 0) throw new Error("manifest validation failed:\n" + validationErrors.join("\n"));
  var tools = mergeTools(repoRoot, config, options.tools || {});
  var outDir = resolveFrom(repoRoot, config.outDir);
  var reportPath = resolveFrom(repoRoot, args.report || options.reportPath || join(outDir, "build-report.json"));
  var logger = options.logger || null;
  var selected = manifest.items.filter(function(item) { return !args.only || args.only.has(item.id); });
  if (selected.length > 0) {
    runStageIfNeeded(args, repoRoot, tools, logger);
    if (options.preflight !== false && !args.skipRender && !args.dryRun) runBatchPreflight(config, tools, repoRoot);
  }
  var report = [];
  selected.forEach(function(item) {
    log(logger, item.id + ": start");
    report.push(processItem({ item: item, config: config, tools: tools, repoRoot: repoRoot, args: args, logger: logger }));
  });
  writeJson(reportPath, report);
  return { exitCode: report.some(function(entry) { return entry.status === "FAIL"; }) ? 1 : 0, reportPath: reportPath, report: report };
}

async function main() {
  var parsed = parseCliArgs(process.argv.slice(2));
  if (parsed.help) {
    console.log("[wiki-media] node scripts/wiki-media/build.mjs [--only id1,id2] [--skip-render] [--skip-stage] [--dry-run] [--report <path>]");
    return 0;
  }
  var logger = function(message) { console.log("[wiki-media] " + message); };
  var result = await runBatch({ repoRoot: DEFAULT_REPO_ROOT, args: parsed, logger: logger });
  logger("report: " + repoRelative(DEFAULT_REPO_ROOT, result.reportPath));
  return result.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().then(function(code) { process.exitCode = code; }).catch(function(err) { console.error("[wiki-media] " + err.message); process.exitCode = 1; });
}
