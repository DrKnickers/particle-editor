// [bench] Test doubles for the corpus pipeline: a fake exe whose ".alo" files
// are JSON emitter dumps, so --tool inspect, --tool patch and --observe can be
// simulated deterministically, and every bundle still passes observe.mjs's own
// checks (per-emitter telemetry, render profile, texture attestation, ledger).
import fs from "node:fs";
import { join } from "node:path";
import { createHash } from "node:crypto";
import { png, fakeFfmpeg } from "../vfx-loop/testing.mjs";

const sha256 = (b) => createHash("sha256").update(b).digest("hex");
const json = (v) => `${JSON.stringify(v, null, 2)}\n`;
const f = Math.fround;

const group = (type = 0, minY = 0, maxY = 0) => ({ type, minX: 0, maxX: 0, minY, maxY, minZ: 0, maxZ: 0, sideLength: 0, sphereRadius: 0,
  sphereEdge: 0, cylinderRadius: 0, cylinderEdge: 0, cylinderHeight: 0, valX: 0, valY: 0, valZ: 0 });
const track = (keys = [[0, 1], [100, 1]], interpolation = 0) => ({ source: 0, explicitUnlock: false, interpolation, keys });

export function emitter(index, name, over = {}) {
  const lifetime = over.lifetime ?? 1, perc = over.randomLifetimePerc ?? 0;
  return { index, name, parent: null, spawnDuringLife: null, spawnOnDeath: null, linkGroup: 0, colorTexture: `${name.toLowerCase()}.tga`, normalTexture: "",
    lifetime, initialDelay: 0, randomLifetimePerc: perc, randomScalePerc: 0, randomColors: [0, 0, 0, 0], randomRotation: false,
    randomRotationDirection: false, isWeatherParticle: false, useBursts: true, nParticlesPerBurst: 8, nParticlesPerSecond: 5, skipTime: 0,
    textureSize: 16, blendMode: 2, groups: [group(), group(1, f(lifetime * f(1 - perc)), lifetime), group()],
    tracks: [track(), track(), track(), track(), track([[0, 20], [100, 20]]), track([[0, 0], [25, 1], [50, 2], [75, 3], [100, 3]], 2), track([[0, 0], [100, 0]])],
    ...over };
}
export const effectBytes = (emitters) => Buffer.from(JSON.stringify({ name: "effect", emitters }));

// Particle counts per emitter and frame: a burst of nParticlesPerBurst from
// the delay for the (seed-varied) lifetime, 30 frames a second. Seed 37 is an
// outlier on any emitter with random lifetime.
function simulate(effect, seed) {
  const spans = effect.emitters.map((e, i) => {
    const u = ((seed * 2654435761 + i * 97) >>> 0) % 1000 / 1000;
    let life = e.lifetime * (1 - e.randomLifetimePerc * u);
    if (seed === 37 && e.randomLifetimePerc > 0) life *= 4;
    const start = Math.round(e.initialDelay * 30);
    return { start, end: start + Math.max(1, Math.round(life * 30)), n: e.nParticlesPerBurst };
  });
  const last = Math.max(...spans.map((s) => s.end));
  return Array.from({ length: last + 1 }, (_, frame) => spans.map((s) => (frame >= s.start && frame < s.end ? s.n : 0)));
}

const loose = (root, name) => ({ kind: "loose", path: `${root}Data/Art/Textures/${name.toUpperCase()}` });

function writeObservation(dir, spec, alo, roots) {
  const effect = JSON.parse(alo.toString("utf8"));
  const per = simulate(effect, spec.scenario.seed);
  const finalFrame = per.length - 1, interval = spec.output.strideInterval;
  const write = (name, bytes) => { fs.mkdirSync(join(dir, name, ".."), { recursive: true }); fs.writeFileSync(join(dir, name), bytes); };
  const stride = Array.from({ length: Math.floor(finalFrame / interval) + 1 }, (_, i) => i * interval);
  if (stride.at(-1) !== finalFrame) stride.push(finalFrame);
  const tintOf = (frame) => (per[frame].reduce((a, b, i) => a + b * (i + 1), 0) * 7 + effect.emitters.reduce((a, e) => a + e.blendMode, 0)) % 256;
  const frames = stride.map((frame) => {
    const bytes = png(tintOf(frame)), name = `selected-frames/frame-${String(frame).padStart(5, "0")}.png`;
    write(name, bytes);
    return { frame, reasons: ["stride"], png: name, sha256: sha256(bytes) };
  });
  const totals = per.map((p) => p.reduce((a, b) => a + b, 0));
  const active = totals.map((v, i) => (v > 0 ? i : -1)).filter((i) => i >= 0);
  const emitters = effect.emitters.map((e, i) => {
    const mark = { index: i, name: e.name, firstActive: null, peakFrame: null, lastActive: null, peakParticles: 0 };
    per.forEach((p, frame) => {
      if (p[i] <= 0) return;
      if (mark.firstActive === null) mark.firstActive = frame;
      if (p[i] > mark.peakParticles) { mark.peakParticles = p[i]; mark.peakFrame = frame; }
      mark.lastActive = frame;
    });
    return mark;
  });
  const lifecycle = { status: "natural-end", finalFrame, reviewWindow: [0, finalFrame], endedBeforeReview: false,
    firstActive: active[0] ?? null, peak: totals.indexOf(Math.max(...totals)), lastActive: active.at(-1) ?? null,
    limits: { runtimeGuard: { total: 0, firstFrame: null }, hardGuard: { total: 0, firstFrame: null }, indexCap: { total: 0, firstFrame: null } }, emitters };
  const root = `${roots[0].replace(/\\/g, "/").replace(/\/+$/, "")}/`;
  const names = [...new Set(effect.emitters.flatMap((e) => [e.colorTexture, e.normalTexture]).filter(Boolean))];
  const missing = (n) => n.includes("absent");
  const resolved = names.filter((n) => !missing(n)), unresolved = names.filter(missing);
  const hashOf = (n) => sha256(n.toUpperCase());
  const assets = [
    { type: "shader", identity: "ENGINE\\PRIMALPHA.FX", requests: ["Engine\\PrimAlpha.fx"], attempts: [], outcome: "resolved",
      winner: { kind: "archive", path: `${root}Data/Shaders.meg`, member: "PRIMALPHA.FX", start: 0, size: 9 }, sha256: "c".repeat(64) },
    ...names.map((n) => (missing(n)
      ? { type: "texture", identity: n.toUpperCase(), requests: [n], attempts: [{ name: n.toUpperCase(), result: "not-found", winner: null, sha256: null }],
        outcome: "default-substituted", winner: { kind: "embedded", resourceId: "IDB_MISSING" }, sha256: "d".repeat(64) }
      : { type: "texture", identity: n.toUpperCase(), requests: [n], attempts: [], outcome: "resolved", winner: loose(root, n), sha256: hashOf(n) })),
  ].sort((a, b) => (a.type + a.identity < b.type + b.identity ? -1 : 1));
  const capture = { schemaVersion: 1, tool: "0.5.0", spec,
    source: { path: spec.scenario.aloPath, sha256: sha256(alo), bytes: alo.length },
    content: { gameRoots: roots.map((r) => `${r.replace(/\\/g, "/").replace(/\/+$/, "")}/`), selectedModPaths: [], sha256: "b".repeat(64),
      resolvedTextures: resolved, unresolvedTextures: unresolved,
      resolvedTextureProvenance: Object.fromEntries(resolved.map((n) => [n, { identity: n.toUpperCase(), winner: loose(root, n), sha256: hashOf(n) }])),
      loadedAssets: assets },
    render: { adapter: { description: "Fake GPU", vendorId: 4318, deviceId: 7, driverVersion: "31.0.15.1", }, displayFormat: 22, depthStencilFormat: 75,
      multisample: { type: 0, quality: 0 }, vertexProcessing: "HWVP" },
    random: { algorithm: "legacy-msvc", algorithmVersion: 1, seed: spec.scenario.seed },
    profile: { camera: { position: [0, -250, 125], target: [0, 0, 0], up: [0, 0, 1] }, gravity: [0, 0, -1], wind: [0, 0, 0], ground: false, bloom: false },
    geometry: { crop: [0, 0, 16, 16], rt: [16, 16], viewport: [0, 0, 16, 16] }, preparation: [], lifecycle, frames, stride, playback: [] };
  write("capture.json", json(capture));
  write("telemetry/summary.json", JSON.stringify(lifecycle, null, 2));   // as the observer writes it (dump(2), no newline)
  write("telemetry/frames.jsonl", per.map((p, frame) => `${JSON.stringify({ frame, instances: 1, emitters: p.length, particles: totals[frame], perEmitter: p,
    rngDrawCount: frame, rngState: frame * 7 + spec.scenario.seed, runtimeGuardRefusals: 0, hardGuardRefusals: 0, indexCapRefusals: 0, overloadActive: false, spawnerIdle: true })}\n`).join(""));
  write("logs/observe.log", "fake\n");
}

function setPath(obj, path, value) {
  const parts = path.split(".");
  let node = obj;
  for (const p of parts.slice(0, -1)) {
    const m = /^(\w+)\[(\d+)\]$/.exec(p);
    node = m ? node[m[1]][Number(m[2])] : node[p];
  }
  node[parts.at(-1)] = value;
}

export function fakeBenchExe({ calls = [] } = {}) {
  const ffmpeg = fakeFfmpeg(calls);
  return async (bin, args, options) => {
    if (bin === "ffmpeg") return ffmpeg(bin, args, options);
    calls.push({ bin, args });
    const ok = (code = 0, stderr = "") => ({ code, signal: null, stdout: "", stderr });
    if (args[0] === "--observe") {
      const spec = JSON.parse(fs.readFileSync(args[1], "utf8"));
      writeObservation(args[2], spec, fs.readFileSync(spec.scenario.aloPath), args.slice(3));
      return ok();
    }
    const [, verb, input] = args;
    const bytes = fs.readFileSync(input);
    if (verb === "inspect") {
      const effect = JSON.parse(bytes.toString("utf8"));
      fs.writeFileSync(args[4], json({ schema: 1, name: effect.name, leaveParticles: true, linkExempts: [], sourceDocumentHash: sha256(bytes), emitters: effect.emitters }));
      return ok();
    }
    if (verb === "patch") {
      const [, , , requestPath, , out, , reportPath] = args;
      const request = JSON.parse(fs.readFileSync(requestPath, "utf8"));
      if (sha256(bytes) !== request.selector.sourceDocumentHash) return ok(4, "selector hash mismatch");
      const effect = JSON.parse(bytes.toString("utf8"));
      const target = effect.emitters.find((e) => e.name === request.selector.rolePath.at(-1).name);
      for (const m of request.mutations) {
        if (m.op === "setProperty") setPath(target, m.field, Math.fround(m.value));
        else if (m.op === "setColorTexture") target.colorTexture = m.name;
        else if (m.op === "setBlendMode") target.blendMode = m.value;
        else if (m.op === "replaceTrack") target.tracks[m.track].keys = m.keys;
      }
      // The serialiser's derived lifetime group.
      target.groups[1].minY = f(target.lifetime * f(1 - target.randomLifetimePerc));
      target.groups[1].maxY = target.lifetime;
      const candidate = effectBytes(effect.emitters);
      fs.writeFileSync(out, candidate);
      fs.writeFileSync(reportPath, json({ schema: 1, outcome: "ok", candidateHash: sha256(candidate) }));
      return ok();
    }
    throw new Error(`unexpected exe call ${args.join(" ")}`);
  };
}

export const fakeSsim = async () => ({ ok: true, all: 0.9 });
