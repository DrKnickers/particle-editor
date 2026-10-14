// [houdini] Run one registered generator (plan §3.1).
//
//   node scripts/houdini/generate.mjs --generator debug-atlas --seed 7 --tier preview
//        [--params p.json] [--out <root>]
//
// Inputs are checked, the cache is consulted under the machine lock, and on a
// miss hython runs run_generator.py inside a Job Object with an allowlisted
// environment and the stage as its working folder. The frames are checked
// against hython's manifest, packed and encoded by the entry's profile
// (profiles.mjs), compressed to a DDS by texconv when the entry's format asks,
// and the texture is validated. Provenance is written last,
// and the result is renamed into <root>\cache\<key>\ in one step. The texture
// is then published to <root>\Data\Art\Textures\ under its content-addressed
// name, so the root works as a content layer. Every run prints one
// [houdini-gen] JSON line, and every run that got as far as a cache key
// appends a summary of it to <root>\audit.log.
//
// The wrapper deletes things under the root (stale stages, evicted entries),
// so it only works in a root it made: one that is new or empty the first time,
// and then carries its marker file. It only ever deletes names it creates.
//
// Exit: 0 ok, 1 failed, 2 rejected, 3 licence, 4 validator, 5 deadline,
// 6 quota, 7 lock busy, 8 the generator failed.
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { randomBytes } from "node:crypto";
import { dirname, join, resolve, win32 } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { resolveReplayGameRoots } from "../replay-proof.mjs";
import { checkParams, checkRoot, checkTier, defaultParams, fail, lookup, parseRegistry, parseSeed } from "./rules.mjs";
import { buildProvenance, cacheKey, canonical, checkManifest, dirBytes, evict, serialise, sha256, treeHashes, verifyEntry } from "./cache.mjs";
import { HOIIOTOOL_VERSION, HOUDINI_BUILD, PINNED, TEXCONV, THREADS, childEnvironment, verifyPinned, verifyTexconv } from "./houdini.mjs";
import { acquireLock, runContained } from "./contain.mjs";
import { decodeDds, decodeTga } from "./images.mjs";
import { atlasLayout, checkFrames, encodeArgs, packArgs, parseFrameStats, profileName, statsArgs, texconvArgs, textureName, validateDds,
  validateTexture } from "./profiles.mjs";

export const HOUDINI_DIR = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(HOUDINI_DIR, "..", "..");
const RUN_GENERATOR = join(HOUDINI_DIR, "hython", "run_generator.py");
const MiB = 1024 * 1024;
export const CACHE_MAX_MIB = 4096;
export const ROOT_MARKER = ".pe-houdini-root";
export const STAGE_NAME = /^[0-9a-f]{16}-[0-9a-f]{8}$/;
const MAX_PARAMS_BYTES = 65536;
const LICENCE = /licen[cs]e/i;

export function parseArgs(argv) {
  const flags = { "--generator": "generator", "--seed": "seed", "--tier": "tier", "--params": "params", "--out": "out" };
  const args = {};
  for (let i = 0; i < argv.length; i++) {
    const name = Object.hasOwn(flags, argv[i]) ? flags[argv[i]] : null;
    if (!name || argv[i + 1] === undefined || Object.hasOwn(args, name)) throw fail(`usage: unexpected argument ${argv[i]}`);
    args[name] = argv[++i];
  }
  for (const name of ["generator", "seed", "tier"]) if (!Object.hasOwn(args, name)) throw fail(`usage: --${name} is required`);
  return args;
}

export const defaultRoot = (env = process.env) => {
  if (!env.LOCALAPPDATA) throw fail("LOCALAPPDATA is not set; pass --out");
  return win32.join(env.LOCALAPPDATA, "ParticleEditor", "VfxGenerated");
};

// This worktree, and the checkout it belongs to when it is a linked worktree.
export function repoRoots() {
  const roots = [repoRoot];
  const r = spawnSync("git", ["-C", repoRoot, "rev-parse", "--path-format=absolute", "--git-common-dir"],
    { encoding: "utf8", windowsHide: true, timeout: 10000 });
  if (r.status === 0 && r.stdout.trim()) roots.push(dirname(resolve(r.stdout.trim())));
  return roots;
}

const tail = (text, bytes = 2048) => (text.length > bytes ? `...${text.slice(-bytes)}` : text).trim();
const readOr = (io, path) => { try { return io.readFileSync(path, "utf8"); } catch { return ""; } };
const remove = (io, path) => io.rmSync(path, { recursive: true, force: true, maxRetries: 10, retryDelay: 200 });
const plainDirOrAbsent = (io, path) => {
  let stat;
  try { stat = io.lstatSync(path); } catch { return; }
  if (stat.isSymbolicLink() || !stat.isDirectory()) throw fail(`${path} is not a plain folder`, 1);
};

// Classify a finished hython run that did not succeed. hython's own text never
// goes into the record or the audit log: it may name a licence server. For a
// generator failure it is returned as `detail`, which is printed, not stored.
export function hythonFailure(code, stdout, stderr) {
  const line = stdout.split(/\r?\n/).reverse().find((v) => v.startsWith("[houdini-run] "));
  let report = null;
  try { report = line && JSON.parse(line.slice(14)); } catch { /* not ours */ }
  if (code === 2 && report?.ok === false) return fail(`run_generator rejected the job: ${report.rejected}`, 2);
  if (LICENCE.test(stdout) || LICENCE.test(stderr)) return fail("hython could not get a licence", 3);
  return Object.assign(fail(`the generator failed (hython exit ${code})`, 8), { detail: tail(stderr) || tail(stdout) });
}

// Make the root ours on first use, and refuse a folder that someone else filled.
const TEXTURES = ["Data", "Art", "Textures"];
function claimRoot(root, io) {
  io.mkdirSync(root, { recursive: true });
  if (io.realpathSync.native(root).toLowerCase() !== root.toLowerCase()) throw fail(`--out moved under a link: ${root}`);
  const marker = join(root, ROOT_MARKER);
  if (!io.existsSync(marker)) {
    if (io.readdirSync(root).length) throw fail(`--out already holds files the generator did not make; pick a new or empty folder: ${root}`);
    io.writeFileSync(marker, "Generated by scripts/houdini/generate.mjs. It deletes stale stages and evicts old cache entries here.\n", { flag: "wx" });
  }
  for (const name of [".staging", "cache"]) plainDirOrAbsent(io, join(root, name));
  for (let depth = 1; depth <= TEXTURES.length; depth++) plainDirOrAbsent(io, join(root, ...TEXTURES.slice(0, depth)));
}

// hoiiotool is a pinned binary run directly: one process, a deadline, the
// allowlisted environment, the stage as its working folder.
export function runHoiiotool(args, { cwd, env, timeoutMs }) {
  const r = spawnSync(PINNED.hoiiotool.path, args, { cwd, env, timeout: timeoutMs, windowsHide: true,
    encoding: "utf8", maxBuffer: 16 * MiB });
  return { code: r.error ? null : r.status, stdout: r.stdout ?? "", stderr: r.stderr ?? "", error: r.error?.message ?? null,
    timedOut: r.error?.code === "ETIMEDOUT" };
}

// texconv likewise, from the SDK's pinned path.
export function runTexconv(args, { cwd, env, timeoutMs }) {
  const r = spawnSync(TEXCONV.texconv.path, args, { cwd, env, timeout: timeoutMs, windowsHide: true, encoding: "utf8",
    maxBuffer: MiB });
  return { code: r.error ? null : r.status, stdout: r.stdout ?? "", stderr: r.stderr ?? "", error: r.error?.message ?? null,
    timedOut: r.error?.code === "ETIMEDOUT" };
}

// Copy the texture out under its content-addressed name. The name says what
// the bytes are, so a same-named file with other bytes is a torn or altered
// copy (or, at 32 bits of hash, a vanishingly rare collision between two of
// one generator's outputs): it is replaced, through a temporary file in
// .staging (a name the stale-stage sweep owns) and one rename. A link under
// the name is replaced too, never read through.
function publish(root, entryDir, name, temporary, io) {
  const textures = join(root, ...TEXTURES), target = join(textures, name);
  const bytes = io.readFileSync(join(entryDir, name)), want = sha256(bytes);
  io.mkdirSync(textures, { recursive: true });
  let have = null;
  try { if (!io.lstatSync(target).isSymbolicLink()) have = sha256(io.readFileSync(target)); } catch { /* not published yet */ }
  if (have !== want) {
    io.mkdirSync(dirname(temporary), { recursive: true });
    io.writeFileSync(temporary, bytes, { flag: "wx" });
    io.renameSync(temporary, target);
  }
  io.writeFileSync(join(entryDir, "published"), `${name}\n`);
  return target;
}

export async function generate(args, {
  io = fs, env = process.env, gameRoots = resolveReplayGameRoots, repos = repoRoots, lock = acquireLock,
  run = runContained, tool = runHoiiotool, verify = verifyPinned, compress = runTexconv, verifyCompressor = verifyTexconv,
  now = () => Date.now(), clock = () => new Date().toISOString(),
  random = () => randomBytes(4).toString("hex"), log = (text) => process.stderr.write(`${text}\n`),
  sleep = (ms) => new Promise((done) => setTimeout(done, ms)), staleWaitMs = 15000,
  houdiniDir = HOUDINI_DIR, cacheMaxMiB = CACHE_MAX_MIB, hython = PINNED.hython.path,
} = {}) {
  const started = now();
  const timings = {};
  const record = { ok: false, exitCode: 1 };
  let auditRoot = null;
  try {
    // 1. Inputs.
    const registryBytes = io.readFileSync(join(houdiniDir, "registry.json"));
    const registry = parseRegistry(registryBytes);
    const entry = lookup(registry, args.generator);
    Object.assign(record, { id: args.generator, version: entry.version, renderer: entry.renderer });
    const tier = checkTier(args.tier), seed = parseSeed(args.seed);
    let params = defaultParams(entry.params);
    if (args.params !== undefined) {
      let stat;
      try { stat = io.statSync(args.params); } catch { throw fail(`--params is not a file: ${args.params}`); }
      if (!stat.isFile()) throw fail(`--params is not a file: ${args.params}`);
      if (stat.size > MAX_PARAMS_BYTES) throw fail("--params file is too large");
      try { params = JSON.parse(io.readFileSync(args.params, "utf8")); } catch { throw fail("--params is not JSON"); }
    }
    params = checkParams(params, entry.params);
    Object.assign(record, { tier, seed });
    const games = gameRoots();
    const root = checkRoot(args.out ?? defaultRoot(env), {
      repoRoot: repos(), env, io, gameRoots: [...games, ...games.map((game) => dirname(game))],
    });
    // Node checks the builder too, so a drifted pin fails before any spawn.
    if (sha256(io.readFileSync(join(houdiniDir, ...entry.builder.split("/")))) !== entry.builderSha256)
      throw fail(`${entry.builder} does not match its pinned builderSha256`, 1);

    // 2. Key.
    const format = entry.format ?? "tga";
    const texconv = format === "tga" ? null : { texconv: TEXCONV.texconv.sha256, d3dx9: TEXCONV.d3dx9.sha256 };
    const key = cacheKey({ id: args.generator, entry, tree: treeHashes(houdiniDir, io), params, seed, tier,
      build: HOUDINI_BUILD, hoiiotool: HOIIOTOOL_VERSION, threads: THREADS, texconv });
    Object.assign(record, { cacheKey: key, paramsSha256: sha256(canonical(params)) });
    const settings = entry.tiers[tier], count = entry.grid * entry.grid;
    claimRoot(root, io);
    auditRoot = root;

    // 3. One Houdini job per machine.
    const held = await lock();
    if (!held) throw fail("another Houdini generation holds the machine lock", 7);
    const cacheDir = join(root, "cache"), staging = join(root, ".staging"), entryDir = join(cacheDir, key);
    let stage = null;
    try {
      // Holding the lock means any stage left behind belongs to a dead run. The
      // lock dies with that run's wrapper, but its launcher may still be
      // draining the job, so a stage still held is waited for, and if it stays
      // held this run refuses rather than run a second tree beside it.
      if (io.existsSync(staging)) {
        for (const name of io.readdirSync(staging).filter((v) => STAGE_NAME.test(v))) {
          for (const deadline = now() + staleWaitMs; ;) {
            try { remove(io, join(staging, name)); break; } catch (error) {
              if (now() >= deadline) throw fail(`a previous run's stage is still in use (${error.code ?? error.message}); try again shortly`, 7);
              await sleep(500);
            }
          }
        }
      }
      if (io.existsSync(entryDir)) {
        let provenance = null;
        try { provenance = verifyEntry(entryDir, key, count, io); } catch (error) {
          log(`[houdini-gen] discarding a cache entry that failed its re-hash: ${error.message}`);
          remove(io, entryDir);
          record.discarded = true;
        }
        if (provenance) {
          const at = new Date(now());
          io.utimesSync(entryDir, at, at);
          const published = publish(root, entryDir, provenance.texture.name, join(staging, `${key.slice(0, 16)}-${random()}`), io);
          Object.assign(record, { ok: true, exitCode: 0, hit: true, entry: entryDir, contentHash: provenance.contentHash,
            texture: provenance.texture.name, published, profile: provenance.outputProfile });
          return record;
        }
      }
      record.hit = false;
      const free = io.statfsSync(root);
      if (free.bavail * free.bsize < settings.maxStageMiB * MiB) throw fail(`less than ${settings.maxStageMiB} MiB free under ${root}`, 6);

      // 4-5. Pinned binaries, then an exclusive stage.
      verify();
      if (texconv) verifyCompressor();
      io.mkdirSync(staging, { recursive: true });
      stage = join(staging, `${key.slice(0, 16)}-${random()}`);
      io.mkdirSync(stage);
      const work = join(stage, "work");
      io.mkdirSync(work);

      // 6. hython, contained.
      const job = { schemaVersion: 1, id: args.generator, params, seed, tier, threads: THREADS, registrySha256: sha256(registryBytes) };
      const out = join(stage, "hython.stdout.txt"), err = join(stage, "hython.stderr.txt");
      const hythonStarted = now();
      const result = await run({ exe: hython, args: [RUN_GENERATOR], cwd: work, env: childEnvironment(work, { inherit: env }),
        input: JSON.stringify(job), stdout: out, stderr: err }, {
        timeoutMs: settings.timeoutMin * 60000,
        watch: () => (dirBytes(stage, io) > settings.maxStageMiB * MiB ? `stage exceeded ${settings.maxStageMiB} MiB` : null),
        onLaunched: (pid) => log(`[houdini-gen] launched ${pid}`),
      });
      timings.hythonMs = now() - hythonStarted;
      if (result.outcome === "deadline") throw fail(`the ${tier} deadline of ${settings.timeoutMin} min passed; the job was killed`, 5);
      if (result.outcome === "stopped") throw fail(`${result.detail}; the job was killed`, 6);
      if (result.outcome !== "exited") throw fail(`the launcher failed: ${result.detail}`, 1);
      if (result.code !== 0) {
        const error = hythonFailure(result.code, readOr(io, out), readOr(io, err));
        if (error.detail) log(`[houdini-gen] hython output (printed, not stored):\n${error.detail}`);
        throw error;
      }

      // 7-8. Check what hython wrote.
      let manifestText;
      try { manifestText = io.readFileSync(join(work, "frames.manifest.json")); }
      catch { throw fail("hython wrote no frames.manifest.json", 4); }
      const manifest = checkManifest(manifestText.toString("utf8"), count, work, io);

      // 7. The profile step: frames checked, packed and encoded by hoiiotool,
      // inside what is left of the tier's deadline. Like hython's, its output
      // is printed, not stored.
      const profileStarted = now(), deadline = hythonStarted + settings.timeoutMin * 60000;
      const layout = atlasLayout(entry.grid, settings.cell);
      const frames = manifest.frames.map((frame) => frame.name);
      const step = (runner, label) => (toolArgs, what) => {
        const late = () => fail(`the ${tier} deadline of ${settings.timeoutMin} min passed while ${label} ran; it was killed`, 5);
        if (deadline - now() <= 0) throw late();
        const r = runner(toolArgs, { cwd: work, env: childEnvironment(work, { inherit: env }), timeoutMs: deadline - now() });
        if (r.timedOut) throw late();
        if (r.code !== 0) {
          if (r.stderr || r.stdout) log(`[houdini-gen] ${label} output (printed, not stored):\n${tail(r.stderr || r.stdout, 2048)}`);
          throw fail(`${label} could not ${what} (${r.code === null ? "it did not start" : `exit ${r.code}`})`, 4);
        }
        return r.stdout;
      };
      const hoiiotool = step(tool, "hoiiotool");
      const frameErrors = checkFrames(parseFrameStats(hoiiotool(statsArgs(frames), "read the frames")), layout);
      if (frameErrors.length) throw fail(`the frames failed the profile: ${frameErrors.join("; ")}`, 4);
      hoiiotool(packArgs(frames, layout, "packed.exr"), "pack the frames");
      hoiiotool(encodeArgs(entry.blend, "packed.exr", "texture.tga"), "encode the texture");
      // texconv exits 0 when it can't write, so the file is the evidence.
      if (texconv) {
        step(compress, "texconv")(texconvArgs(format, "texture.tga"), `write ${format.toUpperCase()}`);
        if (!io.existsSync(join(work, "texture.dds"))) throw fail(`texconv wrote no ${format.toUpperCase()} file`, 4);
      }
      timings.profileMs = now() - profileStarted;

      // 8. The validators: the TGA, then the DDS held to it.
      const tgaBytes = io.readFileSync(join(work, "texture.tga")), tga = decodeTga(tgaBytes);
      const textureErrors = validateTexture(tga, texconv ? null : tgaBytes.length, { blend: entry.blend, layout });
      const file = texconv ? "texture.dds" : "texture.tga", textureBytes = texconv ? io.readFileSync(join(work, file)) : tgaBytes;
      if (texconv && !textureErrors.length)
        textureErrors.push(...validateDds(decodeDds(textureBytes), tga, textureBytes.length, { format, blend: entry.blend, layout }));
      if (textureErrors.length) throw fail(`the texture failed the ${profileName(entry.blend, entry.grid)} profile: ${textureErrors.join("; ")}`, 4);
      const texture = { name: textureName(args.generator, sha256(textureBytes), format), sha256: sha256(textureBytes),
        profile: profileName(entry.blend, entry.grid), cell: layout.cell, pad: layout.pad, size: layout.size, bytes: textureBytes.length };

      // 9-10. Provenance last, then one rename caches the entry and the texture is published.
      const promote = join(stage, "result");
      io.mkdirSync(promote);
      io.renameSync(join(work, "frames"), join(promote, "frames"));
      io.writeFileSync(join(promote, "frames.manifest.json"), manifestText);
      io.renameSync(join(work, file), join(promote, texture.name));
      io.writeFileSync(join(promote, "provenance.json"), serialise(buildProvenance({ id: args.generator, entry, key, params,
        seed, tier, build: HOUDINI_BUILD, hoiiotool: HOIIOTOOL_VERSION, threads: THREADS, manifestSha256: sha256(manifestText), texture,
        texconv })));
      io.mkdirSync(cacheDir, { recursive: true });
      io.renameSync(promote, entryDir);
      const published = publish(root, entryDir, texture.name, join(staging, `${key.slice(0, 16)}-${random()}`), io);
      Object.assign(record, { ok: true, exitCode: 0, entry: entryDir, contentHash: texture.sha256, texture: texture.name,
        published, profile: texture.profile });
      // The entry is published; a failure to trim the cache doesn't undo that.
      try {
        const evicted = evict(cacheDir, cacheMaxMiB * MiB, { keep: key, io });
        if (evicted.length) record.evicted = evicted;
      } catch (error) { log(`[houdini-gen] could not evict old cache entries: ${error.message}`); }
      return record;
    } finally {
      // The lock is released on every path, even if the stage can't be removed.
      try { if (stage) remove(io, stage); }
      catch (error) { log(`[houdini-gen] could not remove the stage ${stage}: ${error.message}`); }
      finally { await held.release(); }
    }
  } catch (error) {
    Object.assign(record, { ok: false, exitCode: error.exitCode ?? 1, error: error.message });
    return record;
  } finally {
    timings.totalMs = now() - started;
    record.timingsMs = timings;
    if (auditRoot) {
      const audit = { time: clock(), id: record.id, version: record.version, cacheKey: record.cacheKey,
        paramsSha256: record.paramsSha256, tier: record.tier, seed: record.seed, hit: record.hit ?? null,
        texture: record.texture ?? null, contentHash: record.contentHash ?? null, exitCode: record.exitCode, error: record.error ?? null, timingsMs: timings };
      try { io.appendFileSync(join(auditRoot, "audit.log"), `${JSON.stringify(audit)}\n`); }
      catch (error) { log(`[houdini-gen] could not append to audit.log: ${error.message}`); }
    }
  }
}

async function main() {
  let record;
  try { record = await generate(parseArgs(process.argv.slice(2))); }
  catch (error) { record = { ok: false, exitCode: error.exitCode ?? 1, error: error.message }; }
  console.log(`[houdini-gen] ${JSON.stringify(record)}`);
  process.exitCode = record.exitCode;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) await main();
