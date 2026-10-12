// Native capture is the first marker; publish the final marker only after assembly.
import * as fs from "node:fs";
import { spawn } from "node:child_process";
import { createHash } from "node:crypto";
import { dirname, join, resolve, relative, isAbsolute, win32 } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { resolveReplayGameRoots } from "./replay-proof.mjs";
import { buildFfmpegArgs } from "./clip-encode/encode.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const PNG = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
export const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
const serialise = (value) => JSON.stringify(value, null, 2) + "\n";
const fail = (message, exitCode = 1) => Object.assign(new Error(message), { exitCode });
const playbackPath = (index) => `playback-frames/frame_${String(index).padStart(5, "0")}.png`;

// Check both lexical and real paths: a bundle-local symlink can still escape it.
export function bundlePath(directory, name, io = fs) {
  if (typeof name !== "string" || !name || name.includes("\\") || name.includes(":") ||
      isAbsolute(name) || win32.isAbsolute(name) || name.split("/").some((v) => !v || v === "." || v === ".."))
    throw fail(`path is not bundle-relative: ${name}`);
  const path = resolve(directory, name);
  const rel = relative(io.realpathSync(directory), io.realpathSync(path));
  if (rel === ".." || rel.startsWith(`..${win32.sep}`) || rel.startsWith("../") || isAbsolute(rel))
    throw fail(`path escapes bundle: ${name}`);
  return path;
}

export function playbackMetrics(capture) {
  const stride = capture.spec.output.playbackStride;
  const [start, end] = capture.lifecycle.reviewWindow;
  const count = stride ? Math.floor((end - start) / stride) + 1 : 0;
  const fps = stride ? 60 / stride : null;
  return { frameCount: count, duration: count ? count / fps : 0, fps };
}

// Per-emitter telemetry is optional: a bundle from before it has neither the
// lifecycle's emitters nor any line's perEmitter. When present, both must be,
// every line must account for its particles, and the landmarks must follow
// from the lines.
function checkPerEmitter(lifecycle, samples, optionalFrame, require) {
  const count = (value) => Number.isSafeInteger(value) && value >= 0;
  const emitters = lifecycle.emitters;
  const lines = samples.filter((v) => v.perEmitter !== undefined).length;
  if (emitters === undefined && lines === 0) return;
  if (!Array.isArray(emitters) || lines !== samples.length) {
    require(false, "per-emitter telemetry must be in the lifecycle and on every line, or in neither");
    return;
  }
  require(emitters.every((v, i) => v?.index === i && typeof v.name === "string" &&
    ["firstActive", "peakFrame", "lastActive"].every((key) => optionalFrame(v[key])) && count(v.peakParticles)),
    "invalid lifecycle emitters");
  require(samples.every((v) => Array.isArray(v.perEmitter) && v.perEmitter.length === emitters.length &&
    v.perEmitter.every(count) && v.perEmitter.reduce((a, b) => a + b, 0) === v.particles),
    "per-emitter counts do not account for every particle");
  const landmarks = emitters.map((_, i) => {
    const mark = { firstActive: null, peakFrame: null, lastActive: null, peakParticles: 0 };
    for (const sample of samples) {
      const n = sample.perEmitter?.[i] ?? 0;
      if (n <= 0) continue;
      if (mark.firstActive === null) mark.firstActive = sample.frame;
      if (n > mark.peakParticles) { mark.peakParticles = n; mark.peakFrame = sample.frame; }
      mark.lastActive = sample.frame;
    }
    return mark;
  });
  require(emitters.every((v, i) => Object.entries(landmarks[i]).every(([key, value]) => v?.[key] === value)),
    "lifecycle emitter landmarks differ from the per-emitter telemetry");
}

// Which file an asset's bytes came from: the fields its kind requires, no others.
function validWinner(winner) {
  const count = (value) => Number.isSafeInteger(value) && value >= 0;
  if (winner?.kind === "loose") return typeof winner.path === "string" && winner.path.length > 0 &&
    winner.member === undefined && winner.resourceId === undefined;
  if (winner?.kind === "archive") return typeof winner.path === "string" && winner.path.length > 0 &&
    typeof winner.member === "string" && winner.member.length > 0 && count(winner.start) && count(winner.size) &&
    winner.resourceId === undefined;
  if (winner?.kind === "embedded") return typeof winner.resourceId === "string" && winner.resourceId.length > 0 &&
    winner.path === undefined;
  return false;
}

// One ledger attempt: which of winner, hash and size are set follows the result.
function validAttempt(attempt, hash) {
  if (typeof attempt?.name !== "string") return false;
  switch (attempt.result) {
    case "not-found": return attempt.winner === null && attempt.sha256 === null;
    case "read-rejected": return validWinner(attempt.winner) && attempt.sha256 === null &&
      Number.isSafeInteger(attempt.size) && attempt.size >= 0 && typeof attempt.reason === "string";
    case "decode-failed":
    case "resolved": return validWinner(attempt.winner) && hash(attempt.sha256);
    default: return false;
  }
}

function validRecord(record, hash) {
  const used = ["resolved", "default-substituted"].includes(record?.outcome);
  return ["texture", "shader"].includes(record?.type) && typeof record.identity === "string" && record.identity.length > 0 &&
    Array.isArray(record.requests) && record.requests.length > 0 && record.requests.every((v) => typeof v === "string") &&
    Array.isArray(record.attempts) && record.attempts.every((v) => validAttempt(v, hash)) &&
    (used ? validWinner(record.winner) && hash(record.sha256) &&
      (record.outcome === "default-substituted") === (record.winner.kind === "embedded")
      : record.outcome === "failed" && record.winner === null && record.sha256 === null);
}

// The texture attestation and the asset ledger are optional together: a bundle
// from before them has none of the four fields. When present, the attested
// names are disjoint, every resolved name has its provenance and nothing else
// does, and every ledger record is well formed.
function checkContentEvidence(content, hash, require) {
  const keys = ["resolvedTextures", "unresolvedTextures", "resolvedTextureProvenance", "loadedAssets"];
  const present = keys.filter((key) => content?.[key] !== undefined);
  if (!present.length) return;
  if (present.length !== keys.length) {
    require(false, "texture attestation and the asset ledger must be present together or not at all");
    return;
  }
  const names = (value) => Array.isArray(value) && value.every((v) => typeof v === "string" && v.length > 0);
  const { resolvedTextures: resolved, unresolvedTextures: unresolved, resolvedTextureProvenance: provenance } = content;
  require(names(resolved) && names(unresolved), "texture attestation entries must be names");
  if (!names(resolved) || !names(unresolved)) return;
  require(!resolved.some((v) => unresolved.includes(v)), "resolved and unresolved textures overlap");
  require(provenance && typeof provenance === "object" && !Array.isArray(provenance) &&
    isDeepStrictEqual(Object.keys(provenance).sort(), [...resolved].sort()) &&
    Object.values(provenance).every((v) => typeof v?.identity === "string" && validWinner(v.winner) &&
      v.winner.kind !== "embedded" && hash(v.sha256)),
    "texture provenance must cover exactly the resolved textures");
  require(Array.isArray(content.loadedAssets) && content.loadedAssets.every((v) => validRecord(v, hash)),
    "invalid asset ledger");
}

// The device the frames came from. Optional, as legacy bundles lack it.
function checkRender(render, require) {
  const count = (value) => Number.isSafeInteger(value) && value >= 0;
  require(typeof render?.adapter?.description === "string" && count(render.adapter.vendorId) &&
    count(render.adapter.deviceId) && /^\d+\.\d+\.\d+\.\d+$/.test(render.adapter.driverVersion ?? "") &&
    count(render.displayFormat) && count(render.depthStencilFormat) &&
    count(render.multisample?.type) && count(render.multisample?.quality) &&
    ["HWVP", "SOFTWARE_VP"].includes(render.vertexProcessing), "invalid render profile");
}

export function checkCapture(directory, { io = fs, spec, assembledPngs = [] } = {}) {
  const errors = [];
  const pngHashes = {};
  let capture;
  const require = (ok, message) => { if (!ok) errors.push(message); };
  try {
    capture = JSON.parse(io.readFileSync(bundlePath(directory, "capture.json", io), "utf8"));
    require(capture.schemaVersion === 1, "unsupported capture schema");
    require(["natural-end", "capped", "spawn-limited"].includes(capture.lifecycle?.status) &&
      capture.failure == null, "failed capture status / failure");
    for (const key of ["tool", "spec", "source", "content", "random", "profile", "geometry", "preparation"])
      require(capture[key] != null, `capture missing ${key}`);
    const hash = (value) => typeof value === "string" && /^[a-f0-9]{64}$/.test(value);
    const count = (value) => Number.isSafeInteger(value) && value >= 0;
    const optionalFrame = (value) => value === null || (count(value) && value <= capture.lifecycle.finalFrame);
    require(typeof capture.tool === "string" && capture.tool.length > 0 && capture.spec?.schemaVersion === 1 &&
      typeof capture.spec.scenario === "object", "invalid tool / spec schema");
    require(typeof capture.source?.path === "string" && hash(capture.source?.sha256) && count(capture.source?.bytes), "invalid source evidence");
    require(Array.isArray(capture.content?.gameRoots) && Array.isArray(capture.content?.selectedModPaths) && hash(capture.content?.sha256), "invalid content evidence");
    checkContentEvidence(capture.content, hash, require);
    require(count(capture.random?.seed) && capture.random.seed <= 0xffffffff && capture.random.algorithmVersion === 1,
      "invalid random evidence");
    require(capture.profile && typeof capture.profile === "object" && !Array.isArray(capture.profile) &&
      Array.isArray(capture.geometry?.crop) && capture.geometry.crop.length === 4 && capture.geometry.crop.every(Number.isInteger) &&
      capture.geometry.crop[2] > 0 && capture.geometry.crop[3] > 0 && Array.isArray(capture.preparation), "invalid geometry / profile / preparation");
    require(Array.isArray(capture.frames) && Array.isArray(capture.stride) && Array.isArray(capture.playback), "capture frame arrays missing");
    require(Number.isInteger(capture.lifecycle?.finalFrame) && capture.lifecycle.finalFrame >= 0 &&
      Array.isArray(capture.lifecycle.reviewWindow) && capture.lifecycle.reviewWindow.length === 2 &&
      capture.lifecycle.reviewWindow.every(Number.isInteger) && capture.lifecycle.reviewWindow[0] >= 0 &&
      capture.lifecycle.reviewWindow[0] <= capture.lifecycle.reviewWindow[1] &&
      capture.lifecycle.reviewWindow[1] === capture.lifecycle.finalFrame, "invalid lifecycle window");
    require(typeof capture.lifecycle.endedBeforeReview === "boolean" &&
      ["firstActive", "peak", "lastActive"].every((key) => optionalFrame(capture.lifecycle[key])) &&
      ["runtimeGuard", "hardGuard", "indexCap"].every((key) => count(capture.lifecycle.limits?.[key]?.total) &&
        optionalFrame(capture.lifecycle.limits?.[key]?.firstFrame)), "invalid lifecycle activity / limits");
    const output = capture.spec?.output;
    require(Number.isSafeInteger(output?.maxBytes) && output.maxBytes > 0 &&
      Number.isInteger(output?.strideInterval) && output.strideInterval > 0 &&
      Number.isInteger(output?.playbackStride) && output.playbackStride >= 0 && output.playbackStride <= 4, "invalid capture output spec");
    // Native serialization supplies defaults; compare only the requested keys.
    function supplied(actual, expected) {
      return expected && typeof expected === "object" && !Array.isArray(expected)
        ? Object.entries(expected).every(([key, value]) => supplied(actual?.[key], value))
        : isDeepStrictEqual(actual, expected);
    }
    if (spec) require(supplied(capture.spec, spec), "capture spec differs from requested spec");
    const seen = new Set(), selected = new Map();
    function image(name, hash) {
      require(!seen.has(name), `duplicate PNG: ${name}`);
      seen.add(name);
      try {
        const bytes = io.readFileSync(bundlePath(directory, name, io));
        require(bytes.subarray(0, 8).equals(PNG), `bad PNG signature: ${name}`);
        pngHashes[name] = sha256(bytes);
        if (hash !== undefined) require(typeof hash === "string" && /^[a-f0-9]{64}$/.test(hash) && pngHashes[name] === hash, `bad PNG hash: ${name}`);
      } catch (error) { errors.push(`PNG ${name}: ${error.message}`); }
    }
    for (const value of capture.frames ?? []) {
      require(Number.isInteger(value.frame) && value.frame >= 0 && value.frame <= capture.lifecycle.finalFrame &&
        !selected.has(value.frame) && Array.isArray(value.reasons) && value.reasons.length > 0, "invalid selected frame");
      require(value.png === `selected-frames/frame-${String(value.frame).padStart(5, "0")}.png`, "selected PNG path differs from frame");
      selected.set(value.frame, value);
      image(value.png, value.sha256 ?? "");
    }
    require(selected.has(0), "missing selected frame 0");
    const metrics = playbackMetrics(capture);
    require(capture.playback.length === metrics.frameCount, "playback frame count differs from review map");
    for (const [index, value] of capture.playback.entries()) {
      require(value.index === index && value.frame === capture.lifecycle.reviewWindow[0] + index * output.playbackStride, "invalid dense playback map");
      // PR B's playback map has no hash field. Repeat compares their full bytes;
      // validate signatures here, and the native hashes on selected PNGs above.
      image(playbackPath(index));
    }
    require(capture.stride.every((frame, i) => selected.get(frame)?.reasons.includes("stride") &&
      (i === 0 || frame > capture.stride[i - 1])), "invalid stride order / reason");
    const tagged = capture.frames.filter((v) => v.reasons.includes("stride")).map((v) => v.frame);
    require(isDeepStrictEqual(capture.stride, tagged), "stride map differs from selected reasons");
    for (const name of ["telemetry/frames.jsonl", "telemetry/summary.json", "logs/observe.log"])
      bundlePath(directory, name, io);
    const summary = JSON.parse(io.readFileSync(join(directory, "telemetry/summary.json"), "utf8"));
    require(isDeepStrictEqual(summary, capture.lifecycle), "telemetry summary differs from lifecycle");
    const samples = io.readFileSync(join(directory, "telemetry/frames.jsonl"), "utf8").trim().split("\n").map(JSON.parse);
    require(samples.length === capture.lifecycle.finalFrame + 1 && samples.every((v, i) => v.frame === i), "telemetry missing simulation frame");
    checkPerEmitter(capture.lifecycle, samples, optionalFrame, require);
    if (capture.render !== undefined) checkRender(capture.render, require);
    // Reject extra PNGs, including numbering gaps beyond image2's stopping point.
    for (const name of bundleFiles(directory, io).filter((v) => v.endsWith(".png")))
      require(seen.has(name) || assembledPngs.includes(name), `unlisted PNG: ${name}`);
  } catch (error) { errors.push(`capture marker missing or malformed: ${error.message}`); }
  return { capture, errors, pngHashes };
}

// A finished bundle: the final marker exists and its deterministic hashes still
// match capture.json and the filmstrip assembly added.
export function checkAssembledBundle(directory, { io = fs } = {}) {
  let manifest;
  try { manifest = JSON.parse(io.readFileSync(bundlePath(directory, "manifest.json", io), "utf8")); }
  catch (error) { return { capture: undefined, errors: [`bundle incomplete (manifest.json): ${error.message}`] }; }
  const filmstripHash = manifest.deterministic?.filmstripSha256 ?? null;
  const checked = checkCapture(directory, { io, assembledPngs: filmstripHash ? ["filmstrip.png"] : [] });
  const errors = [...checked.errors];
  try {
    if (manifest.deterministic?.captureSha256 !== sha256(io.readFileSync(join(directory, "capture.json"))))
      errors.push("manifest capture hash differs from capture.json");
    if (filmstripHash && filmstripHash !== sha256(io.readFileSync(bundlePath(directory, "filmstrip.png", io))))
      errors.push("manifest filmstrip hash differs from filmstrip.png");
  } catch (error) { errors.push(`bundle evidence missing: ${error.message}`); }
  return { capture: checked.capture, errors };
}

export function bundleFiles(directory, io = fs) {
  const result = [];
  function visit(prefix) {
    for (const entry of io.readdirSync(join(directory, prefix), { withFileTypes: true })) {
      const name = prefix ? `${prefix}/${entry.name}` : entry.name;
      if (entry.isSymbolicLink()) throw fail(`bundle contains symlink: ${name}`);
      if (entry.isDirectory()) visit(name);
      else if (entry.isFile()) result.push(name);
      else throw fail(`bundle contains non-file: ${name}`);
    }
  }
  visit("");
  return result.sort();
}

// `frames` defaults to the stride order (the filmstrip); the benchmark's
// contact sheet passes every selected frame.
export function buildConcatList(capture, directory, frames = capture.stride) {
  const selected = new Map(capture.frames.map((v) => [v.frame, v.png]));
  return frames.map((frame) => {
    const name = selected.get(frame);
    if (!name) throw fail(`frame ${frame} has no selected PNG`);
    const path = resolve(directory, name).replaceAll("\\", "/").replaceAll("'", "'\\''");
    return `file '${path}'\n`;
  }).join("");
}

export function buildFilmstripArgs({ list, out, frameCount }) {
  if (!Number.isInteger(frameCount) || frameCount < 2) throw fail("tile requires at least two stride frames");
  return ["-y", "-f", "concat", "-safe", "0", "-i", list, "-vf",
    `tile=8x${Math.ceil(frameCount / 8)}:nb_frames=${frameCount}`, "-frames:v", "1", "-c:v", "png", out];
}

export function spawnWithDeadline(bin, args, { timeoutMs = 660000, spawnImpl = spawn,
  clock = { setTimeout, clearTimeout }, cwd = repoRoot, maxBytes = 32 * 1024 * 1024 } = {}) {
  return new Promise((resolvePromise, reject) => {
    let child, timer, expired = false, oversized = false, bytes = 0;
    let stdout = "", stderr = "";
    try { child = spawnImpl(bin, args, { cwd, windowsHide: true, stdio: ["ignore", "pipe", "pipe"] }); }
    catch (error) { reject(fail(`could not start ${bin}: ${error.message}`)); return; }
    const stop = () => child.kill("SIGKILL");
    timer = clock.setTimeout(() => { expired = true; stop(); }, timeoutMs);
    child.stdout?.on("data", (chunk) => { bytes += chunk.length; if (bytes <= maxBytes) stdout += chunk; else { oversized = true; stop(); } });
    child.stderr?.on("data", (chunk) => { bytes += chunk.length; if (bytes <= maxBytes) stderr += chunk; else { oversized = true; stop(); } });
    child.once("error", (error) => { clock.clearTimeout(timer); reject(fail(`could not start ${bin}: ${error.message}`)); });
    child.once("close", (code, signal) => {
      clock.clearTimeout(timer);
      if (expired) reject(fail(`${bin} exceeded deadline (${timeoutMs} ms); child killed`, 5));
      else if (oversized) reject(fail(`${bin} exceeded output limit; child killed`));
      else resolvePromise({ code: code ?? 1, signal, stdout, stderr });
    });
  });
}

export function checkPlaybackProbe(probe, capture) {
  const expected = playbackMetrics(capture), stream = probe.streams?.[0];
  const frameCount = Number(stream?.nb_read_frames), duration = Number(stream?.duration ?? probe.format?.duration);
  if (frameCount !== expected.frameCount || !Number.isFinite(duration) || Math.abs(duration - expected.duration) > 0.001)
    throw fail(`playback differs from map: ${frameCount} frames / ${duration}s; expected ${expected.frameCount} / ${expected.duration}s`);
  // Store the mapped duration, avoiding probe's decimal rounding in repeat evidence.
  return { frameCount, duration: expected.duration };
}

export function buildManifest({ captureHash, filmstripHash = null, playback = { frameCount: 0, duration: 0 },
  mp4Hash = null, mp4Bytes = 0, bundleBytes = 0, temporaryBytes = 0, manifestBytes = 0 }) {
  return { schemaVersion: 1, deterministic: { captureSha256: captureHash, filmstripSha256: filmstripHash, playback },
    nondeterministic: { mp4Sha256: mp4Hash, mp4Bytes, bundleBytes, temporaryBytes, manifestBytes,
      totalBytes: bundleBytes + manifestBytes } };
}

export function checkQuota(directory, maxBytes, manifestText = "", { io = fs, owned = [] } = {}) {
  const bundleBytes = bundleFiles(directory, io).reduce((sum, name) => sum + io.statSync(join(directory, name)).size, 0);
  const total = bundleBytes + Buffer.byteLength(manifestText);
  if (total > maxBytes) {
    // The native marker and native evidence belong to the exe. Never erase them.
    for (const name of owned) io.rmSync(join(directory, name), { force: true });
    throw fail(`quota exceeded: ${total} bytes > maxBytes ${maxBytes}`, 16);
  }
  return bundleBytes;
}

export async function assembleBundle(directory, { io = fs, run = spawnWithDeadline, spec } = {}) {
  const checked = checkCapture(directory, { io, spec });
  if (checked.errors.length) throw fail(checked.errors.join("; "));
  const capture = checked.capture, maxBytes = capture.spec.output.maxBytes;
  const owned = [], temporary = [];
  const own = (name, temp = false) => {
    if (io.existsSync(join(directory, name))) throw fail(`assembly output already exists: ${name}`);
    owned.push(name); if (temp) temporary.push(name); return join(directory, name);
  };
  const quota = (text = "") => checkQuota(directory, maxBytes, text, { io, owned });
  const child = async (bin, args, timeoutMs) => {
    const result = await run(bin, args, { timeoutMs });
    if (result.code !== 0) throw fail(`${bin} exit ${result.code}: ${result.stderr}`, result.code || 1);
    return result;
  };
  try {
    let filmstripHash = null;
    if (capture.stride.length) {
      const out = own("filmstrip.png");
      if (capture.stride.length === 1) {
        const frame = capture.frames.find((v) => v.frame === capture.stride[0]);
        io.copyFileSync(join(directory, frame.png), out);
      } else {
        const list = own("filmstrip.concat.txt", true);
        io.writeFileSync(list, buildConcatList(capture, directory)); quota();
        await child("ffmpeg", buildFilmstripArgs({ list, out, frameCount: capture.stride.length }), 120000);
      }
      quota();
      const bytes = io.readFileSync(out);
      if (!bytes.subarray(0, 8).equals(PNG)) throw fail("filmstrip has bad PNG signature");
      filmstripHash = sha256(bytes);
    }
    let playback = { frameCount: 0, duration: 0 }, mp4Hash = null, mp4Bytes = 0;
    if (capture.playback.length) {
      io.mkdirSync(join(directory, "playback"), { recursive: true });
      const temp = own("playback/preview.tmp.mp4", true), out = own("playback/preview.mp4");
      const metrics = playbackMetrics(capture);
      await child("ffmpeg", buildFfmpegArgs({ framesDir: join(directory, "playback-frames"),
        fps: metrics.fps, out: temp, frameCount: metrics.frameCount }), 300000);
      quota();
      const probe = await child("ffprobe", ["-v", "error", "-select_streams", "v:0", "-count_frames",
        "-show_entries", "stream=nb_read_frames,duration:format=duration", "-of", "json", temp], 30000);
      playback = checkPlaybackProbe(JSON.parse(probe.stdout), capture);
      io.renameSync(temp, out);
      mp4Hash = sha256(io.readFileSync(out)); mp4Bytes = io.statSync(out).size;
    }
    const manifestPath = own("manifest.json"), manifestTemp = own("manifest.json.tmp", true);
    const assemblyBytes = quota();
    const temporaryBytes = temporary.filter((v) => io.existsSync(join(directory, v)))
      .reduce((sum, v) => sum + io.statSync(join(directory, v)).size, 0);
    const bundleBytes = assemblyBytes - temporaryBytes;
    let text = "", manifest;
    // The byte totals include the serialized marker itself. Reach a fixed point
    // before checking quota, rather than publishing and measuring too late.
    for (let i = 0; i < 10; ++i) {
      manifest = buildManifest({ captureHash: sha256(io.readFileSync(join(directory, "capture.json"))),
        filmstripHash, playback, mp4Hash, mp4Bytes, bundleBytes, temporaryBytes, manifestBytes: Buffer.byteLength(text) });
      const next = serialise(manifest);
      if (next === text) break;
      text = next;
    }
    quota(text);
    io.writeFileSync(manifestTemp, text); // These bytes were already counted in quota(text).
    for (const name of temporary) if (name !== "manifest.json.tmp") io.rmSync(join(directory, name), { force: true });
    io.renameSync(manifestTemp, manifestPath);
    return manifest;
  } catch (error) {
    for (const name of owned) io.rmSync(join(directory, name), { force: true });
    // Remove the assembly-created directory only if it is empty.
    try { io.rmdirSync(join(directory, "playback")); } catch { /* absent or nonempty */ }
    throw error;
  }
}

export function compareRepeat(first, second, { io = fs } = {}) {
  const errors = [];
  try {
    const deterministicFiles = (directory) => bundleFiles(directory, io).filter((name) =>
      name === "capture.json" || name.startsWith("telemetry/") || name.endsWith(".png"));
    const names = deterministicFiles(first), other = deterministicFiles(second);
    if (!isDeepStrictEqual(names, other)) errors.push("deterministic file sets differ");
    for (const name of names) {
      if (!other.includes(name) || !io.readFileSync(join(first, name)).equals(io.readFileSync(join(second, name))))
        errors.push(`repeat byte mismatch: ${name}`);
    }
    const projection = (directory) => {
      const value = JSON.parse(io.readFileSync(join(directory, "manifest.json"), "utf8"));
      delete value.nondeterministic;
      return serialise(value);
    };
    if (projection(first) !== projection(second)) errors.push("repeat byte mismatch: deterministic manifest.json (including playback count / duration)");
  } catch (error) { errors.push(`repeat evidence missing: ${error.message}`); }
  return errors;
}

export async function runObservation(specPath, directory, { exe = join(repoRoot, "x64/Release/ParticleEditor.exe"),
  roots = [], run = spawnWithDeadline, io = fs } = {}) {
  if (io.existsSync(directory) && (io.lstatSync(directory).isSymbolicLink() || io.readdirSync(directory).length))
    throw fail(`output directory must be absent or empty: ${directory}`, 13);
  const result = await run(exe, ["--observe", resolve(specPath), resolve(directory), ...roots], { timeoutMs: 660000 });
  if (result.code !== 0) throw fail(`observe exe exit ${result.code}: ${result.stderr}\n${result.stdout}`, result.code);
  return assembleBundle(directory, { io, run, spec: JSON.parse(io.readFileSync(specPath, "utf8")) });
}

// The first run keeps the requested path, so the lane's late-flash bundle has
// a stable address for the owner's visual check. The repeat is its sibling.
export async function observe(specPath, directory, { repeat = false, ...options } = {}) {
  const manifest = await runObservation(specPath, directory, options);
  if (repeat) {
    await runObservation(specPath, directory + "-repeat", options);
    const errors = compareRepeat(directory, directory + "-repeat", options);
    if (errors.length) throw fail(errors.join("; "), 15);
  }
  return manifest;
}

export async function main(argv = process.argv.slice(2)) {
  const [specPath, directory, ...flags] = argv;
  if (!specPath || !directory || specPath.startsWith("--") || directory.startsWith("--"))
    throw fail("usage: node scripts/observe.mjs <observation.json> <out-dir> [--exe path] [--game-root path ...] [--repeat]", 2);
  let exe = join(repoRoot, "x64/Release/ParticleEditor.exe"), repeat = false;
  const overrides = [];
  for (let i = 0; i < flags.length; ++i) {
    if (flags[i] === "--repeat") { repeat = true; continue; }
    if (!["--exe", "--game-root"].includes(flags[i]) || !flags[i + 1] || flags[i + 1].startsWith("--")) throw fail(`bad observe argument ${flags[i]}`, 2);
    const flag = flags[i], value = resolve(flags[++i]);
    if (flag === "--exe") exe = value; else overrides.push(value);
  }
  if (!fs.existsSync(exe)) throw fail(`missing executable ${exe}; this script builds nothing`);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw fail("missing game data: host lookup found no usable MEG archive");
  await observe(resolve(specPath), resolve(directory), { exe, roots, repeat });
  console.log(`[observe] completed ${resolve(directory)}${repeat ? " (repeat identical)" : ""}`);
  return 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = await main(); }
  catch (error) { console.error(`[observe] ${error.message}`); process.exitCode = error.exitCode ?? 1; }
}
