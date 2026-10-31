// Builds nothing.
import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, statSync, writeFileSync, unlinkSync, openSync, closeSync, readSync, fstatSync } from "node:fs";
import { dirname, join, resolve, basename } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { createHash } from "node:crypto";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const fixtures = join(repoRoot, "web", "apps", "editor", "tests", "fixtures");
export const TEXTURES = ["tests\\fixtures\\bumptest\\zz_bumpgold_rock.tga", "tests\\fixtures\\bumptest\\zz_bumptest_nm.tga"];
export const SCENARIOS = [
  { name: "singleton-emitter", file: "singleton-emitter.alo", latencyRequired: true },
  { name: "bump-cutout", file: "bump-asteroids.alo", latencyRequired: true },
  { name: "replay-weather", file: "replay-weather.alo", weather: true },
  { name: "replay-guard", file: "replay-guard.alo", guard: true },
  { name: "singleton-velocity", file: "singleton-emitter.alo", velocity: true },
];
// The kinds the host refuses while the timeline is open, in E12's order
// (preview/attach is record-only, so the web mock's list leaves it out).
export const TIMELINE_GATED_KINDS = ["engine/set/paused", "engine/action/step-frames", "engine/action/clear",
  "spawner/start", "spawner/trigger", "spawner/stop", "preview/attach"];
export const OPTIONAL_STOCK = ["P_Large_Explosion.alo", "P_Explosion_Space_Large.alo"];

function megPaths(root, read = readFileSync) {
  const bytes = read(join(root, "Data", "MegaFiles.xml"));
  const xml = bytes[0] === 0xff && bytes[1] === 0xfe ? bytes.subarray(2).toString("utf16le") : bytes.toString("utf8");
  if (!/<Mega_Files(?:\s[^>]*)?>/i.test(xml)) return [];
  return [...xml.matchAll(/<File\s*>([^<]+)<\/File>/gi)].map((m) =>
    join(root, ...m[1].trim().replace(/&amp;/g, "&").split(/[\\/]/)));
}

function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value ^= byte;
    for (let bit = 0; bit < 8; ++bit) value = (value >>> 1) ^ ((value & 1) ? 0xedb88320 : 0);
  }
  return (value ^ 0xffffffff) >>> 0;
}

// The host's MegaFiles.cpp reads this legacy header, name table and 20-byte
// FileInfo index. Read only bounded metadata and the selected asset, never an
// entire multi-GB archive. These are the same caps as ResourceLimits.h.
export function findMegAsset(metadata, asset, archiveSize) {
  const maxEntries = 1024 * 1024, maxNames = 16 * 1024 * 1024;
  if (metadata.length < 8) return null;
  const strings = metadata.readUInt32LE(0), files = metadata.readUInt32LE(4);
  if (strings > maxEntries || files > maxEntries || strings > archiveSize / 2 || files > archiveSize / 20) return null;
  const wanted = asset.toUpperCase(), expectedCrc = crc32(Buffer.from(wanted, "latin1"));
  const matches = new Set();
  let at = 8;
  for (let i = 0; i < strings; ++i) {
    if (at + 2 > metadata.length) return null;
    const length = metadata.readUInt16LE(at); at += 2;
    if (length > 32768 || at + length > metadata.length || at + length - 8 > maxNames) return null;
    if (metadata.toString("latin1", at, at + length) === wanted) matches.add(i);
    at += length;
  }
  if (at + files * 20 > metadata.length) return null;
  let found = null;
  for (let i = 0; i < files; ++i, at += 20) {
    const hash = metadata.readUInt32LE(at), size = metadata.readUInt32LE(at + 8);
    const start = metadata.readUInt32LE(at + 12), name = metadata.readUInt32LE(at + 16);
    if (name >= strings || start > archiveSize || size > archiveSize - start) return null;
    if (matches.has(name) && hash === expectedCrc && !found) found = { start, size };
  }
  return found;
}

export function readStockAsset(roots, filename) {
  return readStockFile(roots, `Art\\Models\\${filename}`);
}

// A file under Data\ (`folder\name`): loose in a root first, then in its MEGs.
export function readStockFile(roots, relative) {
  for (const root of roots) {
    const path = join(root, "Data", ...relative.split("\\"));
    if (existsSync(path)) return readFileSync(path);
  }
  for (const root of roots) {
    for (const path of megPaths(root)) {
      let fd;
      try {
        fd = openSync(path, "r");
        const size = fstatSync(fd).size;
        const metadata = Buffer.alloc(Math.min(size, 8 + 16 * 1024 * 1024 + 20 * 1024 * 1024));
        let got = 0;
        while (got < metadata.length) {
          const n = readSync(fd, metadata, got, metadata.length - got, got);
          if (!n) break;
          got += n;
        }
        const entry = findMegAsset(metadata.subarray(0, got), `DATA\\${relative}`, size);
        if (!entry) continue;
        if (!entry.size || entry.size > 64 * 1024 * 1024) throw new Error(`stock file exceeds the proof asset bound: ${relative}`);
        const bytes = Buffer.alloc(entry.size);
        let copied = 0;
        while (copied < bytes.length) {
          const n = readSync(fd, bytes, copied, bytes.length - copied, entry.start + copied);
          if (!n) throw new Error(`short stock asset read: ${relative}`);
          copied += n;
        }
        return bytes;
      } catch (error) {
        // MegaFiles.xml may list an archive absent in this installation; the
        // host skips unavailable archives too. Keep searching the other MEGs.
        if (!["ENOENT", "EACCES", "EPERM"].includes(error.code)) throw error;
      } finally { if (fd !== undefined) closeSync(fd); }
    }
  }
  return null;
}

export function checkedFrames(maxFrames) {
  return [...new Set([0, 1, 2, 15, 60, 300, maxFrames].filter((f) => f <= maxFrames))].sort((a, b) => a - b);
}

function sameFrame(a, b, image = true) {
  const fields = ["frame", "draws", "state", "instances", "particles", "emitters", "guard", "profile", "geometry", "instance", "completedUpdates"];
  return fields.every((key) => isDeepStrictEqual(a?.[key], b?.[key])) && (!image || a?.sha256 === b?.sha256);
}

export function checkReport(report, definition, scenario) {
  const errors = [];
  const acceptanceBlockers = [];
  const require = (ok, message) => { if (!ok) errors.push(message); };
  const hash = (value) => typeof value === "string" && /^[a-f0-9]{64}$/.test(value);
  const capture = (value) => !!value?.png && hash(value?.sha256);
  try {
    require(report?.schemaVersion === 1, "unsupported report schema");
    require(report?.exitCode === 0 && report?.failure === null, "failed hook / nonzero exit");
    require(Number.isFinite(report.wallMs) && report.wallMs >= 0 && typeof report.limits === "string" && report.limits.length > 0,
      "wall time / proof limits missing");
    require(isDeepStrictEqual(report.scenario, scenario), "report scenario differs from requested scenario");
    const frames = checkedFrames(scenario.maxFrames);
    require(isDeepStrictEqual(report.checkedFrames, frames), "wrong checked frames");
    require(report.resize?.unchanged === true && Number.isSafeInteger(report.resize.before) &&
      report.resize.before === report.resize.after, "resize performed an Update or was not measured");
    require(["overloadGuard", "msaaLevel", "estimatedLoad"].every((kind) =>
      Number.isSafeInteger(report.ignoredSettingsPushes?.[kind]) && report.ignoredSettingsPushes[kind] >= 0),
    "ignored settings push counts missing or invalid");
    require(Array.isArray(report.preparation) && report.preparation.length >= 2 && report.preparation.every((p) => {
      const tail = p.passes?.slice(-2);
      return p.settled === true && p.passes.length <= 8 && tail?.length === 2 &&
        tail.every((v) => v.pending === false) && tail[0].applyCount === tail[1].applyCount;
    }), "bounded preparation not observed before spawn and after undo");
    const rectangle = (v) => Array.isArray(v) && v.length === 4 && v.every(Number.isInteger) && v[2] > 0 && v[3] > 0;
    require(report.geometry?.rt?.length === 2 && report.geometry.rt.every((v) => Number.isInteger(v) && v > 0) &&
      rectangle(report.geometry?.viewport) && rectangle(report.geometry?.crop), "capture geometry missing");
    const p = report.profile;
    require(p?.ground === false && p.bloom === false && p.bloomStrength === 0 && p.bloomCutoff === Math.fround(0.9) &&
      p.bloomSize === Math.fround(0.1) && p.heatDebug === false && p.msaa === 0 && p.skydomeSlot === 1 &&
      p.embeddedSkydome === true && p.primaryDome === "" && p.secondaryDome === "" &&
      p.primaryDomeGpu === false && p.secondaryDomeGpu === false && p.skydomeMeshGpu === true &&
      isDeepStrictEqual(p.camera, { position: [0, -250, 125], target: [0, 0, 0], up: [0, 0, 1] }) &&
      isDeepStrictEqual(p.wind, [0, 0, 0]) && isDeepStrictEqual(p.gravity, [0, 0, -1]) &&
      Number.isInteger(p.background) && p.ambient?.length === 4 && p.lights?.length === 3,
    "fixed profile missing or incorrect");
    const e = report.experiments ?? {};
    for (let i = 1; i <= 12; ++i) require(!!e[`E${i}`], `missing experiment E${i}`);
    const baseline = new Map((e.E1?.frames ?? []).map((v) => [v.frame, v]));
    function samples(run, name, { limit = scenario.overloadGuardLimit, compare = true, stride = 1, probe = false } = {}) {
      require(isDeepStrictEqual(run?.frames?.map((v) => v.frame), frames), `${name}: missing or duplicate checked frame`);
      for (const sample of run?.frames ?? []) {
        const f = sample.frame;
        require(isDeepStrictEqual(sample.geometry, report.geometry), `${name}: geometry drift at ${f}`);
        require(isDeepStrictEqual(sample.profile, report.profile), `${name}: profile drift at ${f}`);
        require(isDeepStrictEqual(sample.guard, { enabled: scenario.overloadGuardEnabled, limit, estimate: scenario.estimatedParticlesPerInstance }),
          `${name}: guard / estimate mismatch at ${f}`);
        require([0, 1].includes(sample.instances) && Number.isInteger(sample.emitters) && sample.emitters >= 0 &&
          Number.isInteger(sample.particles) && sample.particles >= 0 && Number.isSafeInteger(sample.draws) && sample.draws >= 0 &&
          Number.isInteger(sample.state) && sample.state >= 0 && sample.state <= 0xffffffff &&
          sample.completedUpdates === f + 1 && (sample.instances === 0 ? sample.instance === null :
            sample.instance?.position?.length === 3 && sample.instance.position.every(Number.isFinite) && Number.isFinite(sample.instance.age)),
        `${name}: counts / completed updates / instance missing at ${f}`);
        const captured = f % stride === 0;
        require(captured ? capture(sample) : sample.png === "" && sample.sha256 === "", `${name}: capture missing or wrong stride at ${f}`);
        if (compare && !probe) require(sameFrame(baseline.get(f), sample, captured), `${name}: hash / draws / state mismatch at ${f}`);
      }
    }
    function wholeRun(run, name) {
      require(Array.isArray(run?.particles) && run.particles.length === scenario.maxFrames + 1 &&
        run.particles.every((v) => Number.isInteger(v) && v >= 0), `${name}: per-update particles missing`);
      const peak = Math.max(...(run?.particles ?? [0]));
      require(run?.peak === peak && run.peakFrame === run.particles.indexOf(peak), `${name}: whole-run peak incorrect`);
      require(Number.isInteger(run?.estimateRefusals) && run.estimateRefusals === 0 &&
        Number.isInteger(run?.runtimeSuppressionUpdates) && run.runtimeSuppressionUpdates >= 0, `${name}: refusal / suppression measurement missing`);
      for (const sample of run?.frames ?? []) require(sample.particles === run.particles?.[sample.frame], `${name}: particle series disagrees`);
    }
    samples(e.E1, "E1", { compare: false });
    wholeRun(e.E1, "E1");
    const zero = baseline.get(0), one = baseline.get(1);
    require(zero?.instances === 1 && one?.instances === 1 && one.draws > 0, "zero spawn / draws");
    const firstParticle = e.E1?.frames?.find((sample) => sample.particles > 0);
    require(firstParticle?.frame <= 60 && report.positive?.firstParticleFrame === firstParticle.frame,
      "missing or incorrect first checked particle frame by frame 60");
    require(capture(report.positive?.empty), "missing empty-scene capture");
    require(capture(report.positive?.peakCapture) && report.positive.peakCapture.frame === e.E1?.peakFrame &&
      report.positive.peakCapture.particles === e.E1?.peak &&
      isDeepStrictEqual(report.positive.peakCapture.geometry, report.geometry) &&
      isDeepStrictEqual(report.positive.peakCapture.profile, report.profile), "missing peak-frame capture");
    require(Number.isSafeInteger(report.positive?.firstParticleDifferentPixels) && report.positive.firstParticleDifferentPixels > 0 &&
      Number.isSafeInteger(report.positive?.peakDifferentPixels) && report.positive.peakDifferentPixels > 0,
    "missing or zero first-particle / peak differing pixels");
    if (definition.velocity) {
      require(isDeepStrictEqual(zero?.instance?.position, scenario.spawnPosition) && zero?.instance?.age === 0,
        "velocity frame 0 age / position incorrect");
      require(one?.instance?.position?.every((v, i) => Math.abs(v - (scenario.spawnPosition[i] + scenario.spawnVelocity[i] / 60)) < 1e-5) &&
        Math.abs(one?.instance?.age - 1 / 60) < 1e-7, "velocity frame 1 age / position incorrect");
    }
    for (const name of ["E2", "E3", "E4", "E5", "E6"]) samples(e[name], name);
    wholeRun(e.E3, "E3");
    require(isDeepStrictEqual(e.E3?.particles, e.E1?.particles), "E3 whole-run particles differ");
    for (const field of ["xmlParses", "catalogBuilds", "catalogObjects", "crtCalls"])
      require(e.E4?.[field] > 0, `zero hostile ${field}`);
    require(e.E5?.editResponse?.ok === true && e.E5.editResponse.data?.skipped?.length === 0 &&
      e.E5.undoResponse?.ok === true && e.E5.undoResponse.data?.applied === true &&
      e.E5.documentChanged === true && e.E5.documentRestored === true, "E5 edit / undo not applied or document not restored");
    require(e.E6?.newSeed === (scenario.seed + 1) >>> 0 &&
      e.E6.newSeedRun?.frames?.find((v) => v.frame === 1)?.state !== one?.state, "E6 new seed did not change state");
    samples(e.E6?.newSeedRun, "E6 new seed", { compare: false });
    wholeRun(e.E6?.newSeedRun, "E6 new seed");
    require(isDeepStrictEqual(e.E7?.runs?.map((r) => r.stride), [1, 2, 3, 4]), "E7 missing stride");
    for (const r of e.E7?.runs ?? []) { samples(r, "E7", { stride: r.stride }); wholeRun(r, "E7"); }
    samples(e.E8, "E8", { probe: true });
    const equivalent = e.E8?.frames?.every((v) => sameFrame(baseline.get(v.frame), v));
    require(e.E8?.equivalent === equivalent, "E8 equivalence finding disagrees with samples");
    require(e.E9?.applicable === !!definition.guard, "E9 skipped or wrong applicability");
    if (definition.guard) {
      for (const name of ["high", "low", "lowRepeat", "highAgain"]) {
        samples(e.E9?.[name], `E9 ${name}`, { limit: name.startsWith("low") ? 1000 : 10000, compare: false });
        wholeRun(e.E9?.[name], `E9 ${name}`);
      }
      require(e.E9.high?.peak > 1000 && e.E9.low?.runtimeSuppressionUpdates > 0 &&
        e.E9.lowRepeat?.runtimeSuppressionUpdates > 0 &&
        e.E9.high?.frames?.at(-1)?.draws !== e.E9.low?.frames?.at(-1)?.draws, "E9 missing high peak / runtime suppression / draw divergence");
      for (const [a, b] of [[e.E9.high, e.E9.highAgain], [e.E9.low, e.E9.lowRepeat]]) {
        require(isDeepStrictEqual(a?.particles, b?.particles) && a?.frames?.every((v, i) => sameFrame(v, b?.frames?.[i])), "E9 low repeat / high restoration differs");
      }
    }
    require(e.E11?.applicable === !!definition.weather, "E11 weather restoration missing");
    if (definition.weather) samples(e.E11, "E11");
    // E12: the editor's timeline. It spawns at rest at the origin, so a
    // moving-spawn scenario does not apply.
    require(e.E12?.applicable === !definition.velocity, "E12 timeline missing or wrong applicability");
    if (!definition.velocity) {
      const t = e.E12;
      require(t.begin?.ok === true && t.begin.data?.active === true && t.begin.data.frame === 0 &&
        t.begin.data.seed === scenario.seed && t.begin.data.status === "paused" && t.begin.data.refused === false,
      "E12 begin did not open paused at frame 0 under the scenario seed");
      require(isDeepStrictEqual(t.gate?.map((g) => g.kind), TIMELINE_GATED_KINDS) && t.gate.every((g) => g.refused === true),
        "E12 a gated kind ran while the timeline was open");
      samples({ frames: t.frames }, "E12");
      samples({ frames: t.again }, "E12 again");
      for (const field of ["xmlParses", "catalogBuilds", "catalogObjects", "crtCalls"])
        require(t.hostile?.[field] > 0, `E12 zero hostile ${field}`);
      // The changed inputs land on the latest checked frame with the most particles.
      const parked = (e.E1?.frames ?? []).reduce((best, v) => (!best || v.particles >= best.particles ? v : best), null);
      require(t.parked === parked?.frame, "E12 did not park on the most-populated checked frame");
      const atParked = (sample) => capture(sample) && sample.frame === parked?.frame;
      require(atParked(t.edited) && (t.edited.sha256 !== parked.sha256 || t.edited.particles !== parked.particles),
        "E12 the edited replay is missing or shows E1's frame");
      require(t.cameraReplays === !!definition.weather, "E12 camera replay decision does not match the weather document");
      require(atParked(t.cameraMoved) && t.cameraMoved.sha256 !== parked.sha256, "E12 the moved camera is missing or shows E1's image");
      // A re-bake is one same-time Update each way: E1's frame plus two Updates.
      const rebaked = t.cameraReplays ? t.cameraRestored
        : { ...t.cameraRestored, completedUpdates: t.cameraRestored?.completedUpdates - 2 };
      require(atParked(t.cameraRestored) && sameFrame(parked, rebaked), "E12 camera replay differs from E1");
      require(atParked(t.guardLow) && t.guardLow.guard?.limit === 1000 &&
        (parked.particles <= 1000 || t.guardLow.particles !== parked.particles), "E12 the lowered guard did not replay under its cap");
      require(atParked(t.estimateRaised) && t.estimateRaised.guard?.estimate === scenario.estimatedParticlesPerInstance + 1,
        "E12 the raised estimate did not replay");
      for (const [name, sample] of [["undo", t.undone], ["guard", t.guardRestored], ["estimate", t.estimateRestored]])
        require(atParked(sample) && sameFrame(baseline.get(sample?.frame), sample), `E12 ${name} replay differs from E1`);
      // Every E12 capture is its own evidence file.
      const pngs = [...(t.frames ?? []), ...(t.again ?? []), t.edited, t.undone, t.cameraMoved, t.cameraRestored,
        t.guardLow, t.guardRestored, t.estimateRaised, t.estimateRestored].map((v) => basename(String(v?.png ?? "")));
      require(pngs.every((name) => name.startsWith("E12-")) && new Set(pngs).size === pngs.length,
        "E12 captures missing, misnamed or overwritten");
      require(t.variation?.seed !== scenario.seed && t.variation.first?.frame === 1 && t.variation.first.instances === 1 &&
        t.variation.first.completedUpdates === 2 &&
        sameFrame(t.variation.first, t.variation.second, false) && t.variation.first.state !== one?.state,
        "E12 new variation did not change frame 1 or its seeks disagree");
      require(Number.isFinite(t.latencyMs?.forwardToEnd) && t.latencyMs.forwardToEnd >= 0 &&
        Number.isFinite(t.latencyMs?.resetTo300) && t.latencyMs.resetTo300 >= 0, "E12 latency not recorded");
      require(t.end?.ok === true && t.end.data?.active === false && t.end.data.status === "closed" && t.end.data.frame === 300,
        "E12 end did not close the session");
    }
    const timings = e.E10?.timings ?? [];
    for (const timing of timings) {
      const values = timing.samplesMs;
      require(Array.isArray(values) && values.length === 5 && values.every((v) => Number.isFinite(v) && v >= 0) &&
        isDeepStrictEqual(values, [...values].sort((a, b) => a - b)) && timing.medianMs === values[2] &&
        timing.maximumMs === values[4], "E10 invalid measurement");
    }
    for (const f of frames) for (const intermediate of [true, false]) {
      const found = timings.filter((t) => t.frame === f && t.renderIntermediate === intermediate);
      // Missing latency is a phase blocker, as is a threshold miss; neither is
      // a deterministic lane failure. An incorrectly claimed acceptance is.
      if (found.length !== 1) acceptanceBlockers.push(`E10 missing measurement frame ${f}, render=${intermediate}`);
    }
    // Acceptance uses the destination-only seek (plan amendment, 2026-10-05);
    // rendered-intermediate seeks pace to the display refresh and are informational.
    const latency = timings.find((t) => t.frame === 300 && t.renderIntermediate === false);
    if (definition.latencyRequired && (!latency || latency.medianMs > 250))
      acceptanceBlockers.push("frame-300 destination-only seek requires median <= 250 ms");
    const accepted = !definition.latencyRequired || (!!latency && latency.medianMs <= 250);
    require(report.acceptance?.latencyRequired === !!definition.latencyRequired && report.acceptance.thresholdMs === 250 &&
      report.acceptance.accepted === accepted, "incorrect phase acceptance status");
  } catch (error) { errors.push(`malformed report: ${error.message}`); }
  return { errors, acceptanceBlockers };
}

export function validateGeneratedFixture(fields, kind) {
  const weather = kind === "weather";
  const expected = {
    kind, reloaded: true, colorTexture: TEXTURES[0], normalTexture: TEXTURES[1],
    useBursts: true, nBursts: 1, nParticlesPerBurst: weather ? 64 : 2048, nParticlesPerSecond: weather ? 64 : 0,
    initialDelay: 0, skipTime: 0, burstDelay: 10000, lifetime: 600, gravity: 0,
    isWeatherParticle: weather, weatherCubeSize: weather ? 128 : 0, weatherCubeDistance: weather ? 200 : 0,
    positionGroup: 1, scale: 8, alpha: 1,
  };
  return Object.entries(expected).filter(([key, value]) => fields?.[key] !== value).map(([key]) => `fixture ${kind}: ${key} differs after reload`);
}

// Mirrors main.cpp: explicit directory overrides, otherwise saved GameDataPath
// plus its EaW/FoC sibling and cwd. The host still validates archive contents.
export function resolveReplayGameRoots({ overrides = [], cwd = repoRoot, savedPath, read = readFileSync, exists = existsSync, size = (p) => statSync(p).size } = {}) {
  if (savedPath === undefined && !overrides.length) {
    const reg = spawnSync("reg.exe", ["query", "HKCU\\Software\\AloParticleEditor", "/v", "GameDataPath"], { encoding: "utf8", windowsHide: true });
    savedPath = reg.status === 0 ? reg.stdout.match(/GameDataPath\s+REG_SZ\s+(.+)/)?.[1]?.trim() : "";
  }
  const candidates = overrides.length ? overrides : [...(savedPath ? [savedPath] : []), cwd];
  if (!overrides.length && savedPath && /(?:GameData|corruption)$/i.test(basename(savedPath.replace(/[\\/]$/, "")))) {
    const sibling = join(dirname(savedPath.replace(/[\\/]$/, "")), /^GameData$/i.test(basename(savedPath.replace(/[\\/]$/, ""))) ? "corruption" : "GameData");
    if (exists(sibling)) candidates.splice(1, 0, sibling);
  }
  const roots = [];
  for (const root of [...new Set(candidates.map((p) => resolve(p)))]) {
    try {
      if (megPaths(root, read).some((path) => exists(path) && size(path) > 0)) roots.push(root);
    } catch { /* Missing roots are an explicit prereq result in the lane. */ }
  }
  return roots;
}

export function scenarioFor(definition) {
  return {
    schemaVersion: 1, aloPath: resolve(fixtures, definition.file), seed: 0x12345678, algorithmVersion: 1,
    clockAnchor: 0, spawnPosition: [0, 0, 0], spawnVelocity: definition.velocity ? [60, 0, 0] : [0, 0, 0],
    maxFrames: 300, overloadGuardEnabled: true, overloadGuardLimit: 10000, estimatedParticlesPerInstance: 0,
  };
}

export function checkCaptureFiles(report, read = readFileSync) {
  const errors = [];
  const seen = new Set();
  function visit(value) {
    if (!value || typeof value !== "object") return;
    if (value.png && value.sha256 && !seen.has(value.png)) {
      seen.add(value.png);
      try {
        const bytes = read(value.png);
        if (!bytes.subarray(0, 8).equals(Buffer.from([137, 80, 78, 71, 13, 10, 26, 10])) ||
            createHash("sha256").update(bytes).digest("hex") !== value.sha256)
          errors.push(`PNG evidence differs: ${value.png}`);
      } catch { errors.push(`PNG evidence missing: ${value.png}`); }
    }
    for (const child of Object.values(value)) visit(child);
  }
  visit(report);
  return errors;
}

export function main(argv = process.argv.slice(2)) {
  let exe = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
  let out = join(repoRoot, "tests", ".tmp", "replay-proof");
  const overrides = [];
  for (let i = 0; i < argv.length; ++i) {
    if (!["--exe", "--out-dir", "--game-root"].includes(argv[i]) || !argv[i + 1]) throw new Error(`bad replay-proof argument ${argv[i]}`);
    const flag = argv[i], value = resolve(argv[++i]);
    if (flag === "--exe") exe = value;
    if (flag === "--out-dir") out = value;
    if (flag === "--game-root") overrides.push(value);
  }
  if (!existsSync(exe)) throw new Error(`missing executable ${exe}; this script builds nothing`);
  const roots = resolveReplayGameRoots({ overrides });
  if (!roots.length) throw new Error("missing game data: host lookup found no Data/MegaFiles.xml with an available MEG");
  for (const texture of TEXTURES) if (!existsSync(join(repoRoot, ...texture.split("\\")))) throw new Error(`missing committed texture ${texture}`);
  mkdirSync(out, { recursive: true });
  function launch(args, allowFailure = false) {
    const result = spawnSync(exe, [...args, ...roots], { cwd: repoRoot, encoding: "utf8", timeout: 660000,
      windowsHide: true, maxBuffer: 32 * 1024 * 1024 });
    if (!allowFailure && (result.error || result.signal || result.status !== 0))
      throw new Error(`${args[0]} exit ${result.status}: ${result.error?.message ?? result.signal ?? result.stderr}\n${result.stdout ?? ""}`);
    return result;
  }
  for (const kind of ["weather", "guard"]) {
    const path = join(fixtures, `replay-${kind}.alo`);
    const { stdout } = launch(["--gen-replay-proof-fixture", kind, path]);
    const line = stdout.match(/\[replay-fixture\] (.+)/)?.[1];
    const problems = validateGeneratedFixture(line ? JSON.parse(line) : null, kind);
    if (!existsSync(path) || problems.length) throw new Error(problems.join("; ") || `fixture missing ${path}`);
  }
  let failures = 0;
  const latency = [];
  function prove(definition, optional = false) {
    const scenario = scenarioFor(definition);
    const directory = join(out, definition.name);
    mkdirSync(directory, { recursive: true });
    const path = join(directory, "scenario.json");
    writeFileSync(path, JSON.stringify(scenario, null, 2));
    try {
      const reportPath = join(directory, "report.json");
      // A successful child with no new report must not reuse an earlier proof.
      if (existsSync(reportPath)) unlinkSync(reportPath);
      const processResult = launch(["--replay-proof", path, directory], true);
      const report = JSON.parse(readFileSync(reportPath, "utf8"));
      const checked = checkReport(report, definition, scenario);
      checked.errors.push(...checkCaptureFiles(report));
      if (processResult.error || processResult.signal || processResult.status !== 0)
        checked.errors.push(`process exit ${processResult.status}: ${processResult.error?.message ?? processResult.signal ?? processResult.stderr}`);
      if (report.failure) checked.errors.push(`hook=${report.failure.hook} frame=${report.failure.frame}: ${report.failure.message}`);
      if (checked.errors.length) throw new Error(checked.errors.join("; "));
      console.log(`[replay-proof] ${optional ? "optional " : ""}${definition.name}: PASS; wall=${report.wallMs.toFixed(2)} ms; E8 equivalent=${report.experiments.E8.equivalent}`);
      for (const blocker of checked.acceptanceBlockers) console.log(`[phase acceptance BLOCKED] ${definition.name}: ${blocker}`);
      for (const timing of report.experiments.E10.timings) latency.push({ scenario: definition.name, optional,
        frame: timing.frame, intermediate: timing.renderIntermediate, medianMs: timing.medianMs.toFixed(2), maximumMs: timing.maximumMs.toFixed(2) });
    } catch (error) {
      console.error(`[replay-proof] ${optional ? "optional " : ""}${definition.name}: FAIL: ${error.message}`);
      if (!optional) ++failures;
    }
  }
  for (const definition of SCENARIOS) prove(definition);
  for (const file of OPTIONAL_STOCK) {
    try {
      const bytes = readStockAsset(roots, file);
      if (!bytes) { console.log(`[replay-proof] optional ${file}: absent from resolved game data; not run`); continue; }
      const directory = join(out, "stock-inputs");
      mkdirSync(directory, { recursive: true });
      const path = join(directory, file);
      writeFileSync(path, bytes);
      prove({ name: `stock-${basename(file, ".alo")}`, file: path }, true);
    } catch (error) { console.error(`[replay-proof] optional ${file}: unavailable: ${error.message}`); }
  }
  console.log("E10 seek latency (destination render included; acceptance: destination-only frame-300 median <= 250 ms):");
  console.table(latency);
  return failures ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  try { process.exitCode = main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
