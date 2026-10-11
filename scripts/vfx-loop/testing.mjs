// [vfx-loop] Test doubles for the loop's unit tests: a fake exe (--tool and
// --observe) and a fake ffmpeg behind the same run(bin, args) seam the real
// code spawns through. The fake --observe writes only the exe's own outputs,
// so observe.mjs still assembles and checks every bundle itself.
import * as fs from "node:fs";
import { join } from "node:path";
import { deflateSync } from "node:zlib";
import { assembleBundle, sha256 } from "../observe.mjs";

const json = (value) => JSON.stringify(value, null, 2) + "\n";

function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let i = 0; i < 8; ++i) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const name = Buffer.from(type), length = Buffer.alloc(4), crc = Buffer.alloc(4);
  length.writeUInt32BE(data.length); crc.writeUInt32BE(crc32(Buffer.concat([name, data])));
  return Buffer.concat([length, name, data, crc]);
}
// A 16x16 opaque RGBA frame, like the exe's, tinted so frames hash apart.
export function png(red = 0) {
  const header = Buffer.alloc(13); header.writeUInt32BE(16, 0); header.writeUInt32BE(16, 4); header[8] = 8; header[9] = 6;
  const pixels = Buffer.alloc(16 * 65);
  for (let row = 0; row < 16; ++row) for (let column = 0; column < 16; ++column) {
    const at = row * 65 + 1 + column * 4; pixels[at] = red & 255; pixels[at + 3] = 255;
  }
  return Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", header),
    chunk("IDAT", deflateSync(pixels)), chunk("IEND", Buffer.alloc(0))]);
}

export const PROFILE = { camera: { position: [0, -250, 125], target: [0, 0, 0], up: [0, 0, 1] }, gravity: [0, 0, -1],
  wind: [0, 0, 0], ground: false, bloom: false };

// The native half of a bundle. `particles` is the per-frame count; the run
// lasts particles.length frames. `tint(frame)` picks each selected frame's pixels;
// `capture(c)` edits the marker before it is written.
export function writeNative(directory, spec, { sourceHash = "a".repeat(64), particles = [5, 8, 3, 0],
  draws = particles.map((_, i) => 10 + i), tint = (frame) => frame, profile = PROFILE, crop = [0, 0, 16, 16], capture: edit = () => {} } = {}) {
  fs.mkdirSync(directory, { recursive: true });
  const write = (name, bytes) => { fs.mkdirSync(join(directory, name, ".."), { recursive: true }); fs.writeFileSync(join(directory, name), bytes); };
  const finalFrame = particles.length - 1, interval = spec.output.strideInterval;
  const stride = Array.from({ length: Math.floor(finalFrame / interval) + 1 }, (_, i) => i * interval);
  if (stride.at(-1) !== finalFrame) stride.push(finalFrame);
  const frames = stride.map((frame) => {
    const bytes = png(tint(frame)), name = `selected-frames/frame-${String(frame).padStart(5, "0")}.png`;
    write(name, bytes);
    return { frame, reasons: ["stride"], png: name, sha256: sha256(bytes) };
  });
  const active = particles.map((v, i) => (v > 0 ? i : -1)).filter((i) => i >= 0);
  const peak = active.length ? particles.indexOf(Math.max(...particles)) : null;
  const lifecycle = { status: "natural-end", finalFrame, reviewWindow: [0, finalFrame], endedBeforeReview: false,
    firstActive: active[0] ?? null, peak, lastActive: active.at(-1) ?? null,
    limits: { runtimeGuard: { total: 0, firstFrame: null }, hardGuard: { total: 0, firstFrame: null }, indexCap: { total: 0, firstFrame: null } } };
  const capture = { schemaVersion: 1, tool: "0.4.2", spec,
    source: { path: spec.scenario.aloPath ?? "C:/effect.alo", sha256: sourceHash, bytes: 1 },
    content: { gameRoots: ["D:/game/"], selectedModPaths: [], sha256: "b".repeat(64) },
    random: { algorithm: "legacy-msvc", algorithmVersion: 1, seed: spec.scenario.seed ?? 1 },
    profile, geometry: { crop, rt: [16, 16], viewport: crop }, preparation: [], lifecycle, frames, stride, playback: [] };
  edit(capture);
  write("capture.json", json(capture)); write("telemetry/summary.json", json(lifecycle));
  write("telemetry/frames.jsonl", particles.map((count, frame) => JSON.stringify({ frame, particles: count, emitters: 1,
    instances: 1, rngDrawCount: draws[frame], rngState: draws[frame] * 7, spawnerIdle: true }) + "\n").join(""));
  write("logs/observe.log", "native log\n");
  return capture;
}

export function spec(overrides = {}) {
  return { schemaVersion: 1,
    scenario: { schemaVersion: 1, aloPath: "C:/effect.alo", seed: 7, algorithmVersion: 1, maxFrames: 300, ...overrides.scenario },
    lifecycle: { type: "oneshot", quietTailFrames: 15 }, output: { strideInterval: 2, playbackStride: 0, maxBytes: 200 * 1024 * 1024 } };
}

// Fake ffmpeg: whatever it is asked to make, it writes a PNG at the last argument.
export function fakeFfmpeg(calls = []) {
  return async (bin, args) => {
    calls.push({ bin, args });
    if (bin !== "ffmpeg") throw new Error(`unexpected ${bin}`);
    fs.writeFileSync(args.at(-1), png(200));
    return { code: 0, signal: null, stdout: "", stderr: "" };
  };
}

export async function bundle(directory, specValue = spec(), options = {}) {
  writeNative(directory, specValue, options);
  await assembleBundle(directory, { run: fakeFfmpeg(), spec: specValue });
  return directory;
}

// Fake exe + ffmpeg. `observeOptions(aloBytes)` shapes each observation (for
// example, more particles for one candidate); `refuse(name)` makes --tool patch
// refuse that emitter with { code, category }.
export function fakeExe({ observeOptions = () => ({}), refuse = () => null, validate = () => 0, calls = [] } = {}) {
  const ffmpeg = fakeFfmpeg(calls);
  return async (bin, args, options) => {
    if (bin === "ffmpeg") return ffmpeg(bin, args, options);
    calls.push({ bin, args });
    const ok = (code = 0) => ({ code, signal: null, stdout: "", stderr: `[fake] exit=${code}` });
    if (args[0] === "--observe") {
      const specValue = JSON.parse(fs.readFileSync(args[1], "utf8")), alo = fs.readFileSync(specValue.scenario.aloPath);
      writeNative(args[2], specValue, { sourceHash: sha256(alo), ...observeOptions(alo) });
      return ok();
    }
    const [, verb, input] = args;
    if (verb === "inspect") {
      fs.writeFileSync(args[4], json({ schema: 1, sourceDocumentHash: sha256(fs.readFileSync(input)), emitters: [] }));
      return ok();
    }
    if (verb === "validate") {
      const code = validate(fs.readFileSync(input));
      fs.writeFileSync(args[4], json({ schema: 1, valid: code === 0, exitCode: code, findings: [] }));
      return ok(code);
    }
    if (verb === "patch") {
      const [, , , requestPath, , out, , reportPath] = args;
      const request = JSON.parse(fs.readFileSync(requestPath, "utf8")), bytes = fs.readFileSync(input);
      const name = request.selector.rolePath.at(-1).name;
      const refusal = sha256(bytes) !== request.selector.sourceDocumentHash
        ? { code: 4, category: "selector-hash-mismatch" } : refuse(name);
      if (refusal) {
        fs.writeFileSync(reportPath, json({ schema: 1, outcome: "refused", exitCode: refusal.code,
          refusal: { category: refusal.category, reason: "fake refusal", op: null } }));
        return ok(refusal.code);
      }
      const candidate = Buffer.concat([bytes, Buffer.from(`|${name}`)]);
      fs.writeFileSync(out, candidate);
      fs.writeFileSync(reportPath, json({ schema: 1, outcome: "ok", exitCode: 0, selector: { candidates: 1, name },
        candidateHash: sha256(candidate), checks: { 1: true, 2: true, 3: true, 4: true, 5: true } }));
      return ok();
    }
    throw new Error(`unexpected exe call ${args.join(" ")}`);
  };
}
