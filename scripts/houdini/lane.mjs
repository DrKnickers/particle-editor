// [houdini] The opt-in `houdini` gate lane (plan §5 "Houdini lane"):
//   node scripts/run-all-tests.mjs --lane houdini
//
// Runs debug-atlas through the real wrapper, launcher, Job Object and Houdini
// in a throwaway root, then checks:
//   - the published texture holds the oracle's 16 colours in order, each frame
//     at its size, inset in its cell and upright (its orientation mark is top
//     left);
//   - the texture passed its profile and was published under its name;
//   - the editor loads it from the root alone, and plays the cells 0..15 in
//     order (validate.mjs, through the Release build);
//   - the same inputs in a new root give the same texture bytes;
//   - its provenance records the oracle's cell steps (red up 85 along a row,
//     down 255 with green up 85 at a row's end), and cell-steps.mjs measures
//     the game's own fire sheet, P_PARTICLE_EXPLOSION1, the stock baseline;
//   - debug-atlas-dds, the same frames compressed by texconv: a DXT5 DDS with
//     a full mip chain, held to the TGA that passed the oracle (so its cells,
//     frames and orientation marks are where they were); the editor loads it
//     as DXT5 and plays its cells 0..15 in order; a new root reproduces it
//     byte for byte;
//   - each profile's encode, through the real hoiiotool, passes its
//     validators, keeps colour where alpha is partial, and a broken input
//     fails them;
//   - an identical re-run is a verified cache hit;
//   - a run while the machine lock is held exits 7;
//   - killing hython mid-run exits 8, with no stage and no cache entry left;
//   - killing the wrapper alone (no tree kill) still ends hython, and the next
//     run sweeps the abandoned stage;
//   - each of the three generators, at the validate tier over four seeds,
//     passes its profile, re-runs as a hit, reproduces byte for byte in a new
//     root and passes editor validation (its flipbook plays, for the
//     fireball), and the four seeds give four textures;
//   - the puff and the fireball stay inside their edge masks, at their
//     defaults and at the parameters that push them furthest out;
//   - a review kit staged from a final-tier ring holds the texture byte for
//     byte, the lifted review fixture and the game's own PARTICLES.XML with
//     only the click marker's lifetime changed; a second staging is refused,
//     and --record writes the verdict.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, win32 } from "node:path";
import { fileURLToPath } from "node:url";
import { HFS } from "./houdini.mjs";
import { acquireLock } from "./contain.mjs";
import { sha256 } from "./cache.mjs";
import { runHoiiotool } from "./generate.mjs";
import { decodeDds, decodeTga } from "./images.mjs";
import { PAD, atlasLayout, encodeArgs, packArgs, validateDds, validateTexture } from "./profiles.mjs";
import { MARKER, MARKER_FRAMES, REVIEW_LIFT, focRoot } from "./review-kit.mjs";
import { readStockFile, resolveReplayGameRoots } from "../replay-proof.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const generateScript = join(here, "generate.mjs");
const validateScript = join(here, "validate.mjs");
const reviewScript = join(here, "review-kit.mjs");
const cellStepsScript = join(here, "cell-steps.mjs");
const system32 = win32.join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const GRID = 4, CELL = 32, FRAME = CELL - 2 * PAD;
const GENERATORS = ["static-smoke-puff", "static-energy-ring", "ground-fireball-4x4"], EDGE_MAX = 8;
// The generators whose frames end in a square edge mask, each with the
// parameters that push its effect furthest out. (The ring's window is round,
// so it can't show the frame's square.)
const MASKED = {
  "static-smoke-puff": { density: 3, billow: 1, brightness: 1 },
  "ground-fireball-4x4": { temperature: 2, turbulence: 1, palette: "orange" },
};
const failures = [];
const check = (ok, what) => {
  console.log(`[houdini-lane] ${ok ? "PASS" : "FAIL"} ${what}`);
  if (!ok) failures.push(what);
  return ok;
};
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const alive = (pid) => { try { process.kill(pid, 0); return true; } catch (error) { return error.code === "EPERM"; } };
async function until(condition, ms) {
  for (const started = Date.now(); Date.now() - started < ms; await sleep(100)) if (condition()) return true;
  return condition();
}

// Starts generate.mjs; `onLaunched(hythonPid, wrapper)` fires when hython runs.
function generate(root, seed, { generator = "debug-atlas", tier = "preview", params = null, onLaunched = null, minutes = 10 } = {}) {
  const wrapper = spawn(process.execPath, [generateScript, "--generator", generator, "--seed", String(seed),
    "--tier", tier, "--out", root, ...(params ? ["--params", params] : [])], { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "", stderr = "", hython = null;
  wrapper.stdout.on("data", (chunk) => { stdout += chunk; });
  wrapper.stderr.on("data", (chunk) => {
    stderr += chunk;
    const m = /\[houdini-gen\] launched (\d+)/.exec(stderr);
    if (m && hython === null) { hython = Number(m[1]); onLaunched?.(hython, wrapper); }
  });
  // A wrapper that never exits must not hang the gate: kill it, and say so.
  const timer = setTimeout(() => { stderr += "\n[houdini-lane] wrapper timed out; killed"; taskkill(wrapper.pid); }, minutes * 60000);
  return new Promise((done) => wrapper.once("close", (code) => {
    clearTimeout(timer);
    const line = stdout.split(/\r?\n/).find((v) => v.startsWith("[houdini-gen] {"));
    let record = null;
    try { if (line) record = JSON.parse(line.slice(14)); } catch { /* reported below */ }
    done({ code, record, hython, stderr });
  }));
}

const taskkill = (pid) => spawnSync(win32.join(system32, "taskkill.exe"), ["/F", "/PID", String(pid)], { windowsHide: true });
const staged = (root) => (existsSync(join(root, ".staging")) ? readdirSync(join(root, ".staging")) : []);
const cached = (root) => (existsSync(join(root, "cache")) ? readdirSync(join(root, "cache")) : []);

// Every texel of every cell's frame, in the published texture: red = i % grid
// and green = i / grid in thirds, blue 0.5, except the frame's top-left
// quarter-by-quarter corner, where blue is 1 (debug-atlas's orientation mark).
// So cell order, frame size and placement, and each frame's orientation are
// all checked, and the guard bands are black.
function decodeTexture(path) {
  const image = decodeTga(readFileSync(path)), layout = atlasLayout(GRID, CELL), mark = Math.floor(FRAME / 4);
  const errors = [];
  for (const cell of layout.cells) {
    const want = [Math.round((cell.index % GRID) * 85), Math.round(Math.floor(cell.index / GRID) * 85)];
    let wrong = 0, first = null;
    for (let v = -PAD; v < FRAME + PAD; v++) {
      for (let u = -PAD; u < FRAME + PAD; u++) {
        const o = ((cell.frameY + v) * image.width + cell.frameX + u) * 4;
        const inFrame = u >= 0 && v >= 0 && u < FRAME && v < FRAME;
        const expected = !inFrame ? [0, 0, 0] : [...want, u < mark && v < mark ? 255 : 128];
        const got = [...image.rgba.subarray(o, o + 3)];
        if (got.some((value, c) => Math.abs(value - expected[c]) > 1)) { wrong++; first ??= `(${u},${v}) is ${got} not ${expected}`; }
      }
    }
    if (wrong) errors.push(`cell ${cell.index}: ${wrong} texels wrong, first ${first}`);
  }
  return errors;
}

// The brightest texel (RGB if additive, alpha if transparent) on the square
// where a builder's edge mask reaches 1: ceil(frame / 20) texels in from each
// side of every frame. The mask fades what lies outside it, so the outermost
// texels are black by construction and prove nothing; an effect that reaches
// this square would have the mask cut it into the frame's shape.
function maskEdge(entry) {
  const { texture } = JSON.parse(readFileSync(join(entry, "provenance.json"), "utf8"));
  const image = decodeTga(readFileSync(join(entry, texture.name))), layout = atlasLayout(texture.grid, texture.cell);
  const last = layout.frame - 1, inset = Math.ceil(layout.frame / 20);
  let worst = 0;
  for (const cell of layout.cells) {
    for (let v = inset; v <= last - inset; v++) {
      for (let u = inset; u <= last - inset; u++) {
        if (Math.min(u, v, last - u, last - v) !== inset) continue;
        const o = ((cell.frameY + v) * image.width + cell.frameX + u) * 4;
        worst = Math.max(worst, texture.blend === "additive" ? Math.max(...image.rgba.subarray(o, o + 3)) : image.rgba[o + 3]);
      }
    }
  }
  return worst;
}

// validate.mjs as the editor-validation step runs it.
function validate(entry, out) {
  const r = spawnSync(process.execPath, [validateScript, "--entry", entry, "--out", out], { encoding: "utf8", windowsHide: true, timeout: 20 * 60000 });
  const line = (r.stdout ?? "").split(/\r?\n/).find((v) => v.startsWith("[houdini-validate] {"));
  try { return { code: r.status, record: line ? JSON.parse(line.slice(19)) : null }; } catch { return { code: r.status, record: null }; }
}

// review-kit.mjs, as the owner runs it.
function reviewKit(args) {
  const r = spawnSync(process.execPath, [reviewScript, ...args], { encoding: "utf8", windowsHide: true, timeout: 5 * 60000 });
  const line = (r.stdout ?? "").split(/\r?\n/).find((v) => v.startsWith("[houdini-review] {"));
  try { return { code: r.status, record: line ? JSON.parse(line.slice(17)) : null }; } catch { return { code: r.status, record: null }; }
}

// Each profile through the real hoiiotool on a synthetic 1x1 layout: a frame
// whose straight colour is (1, 0.5, 0) under an alpha ramp, premultiplied as a
// renderer writes it. The encode must pass its validators and store that
// colour wherever alpha is partial; a broken input must fail them.
function profileEncodes(dir) {
  mkdirSync(dir);
  const env = { SystemRoot: process.env.SystemRoot, PATH: `${system32};${win32.join(HFS, "bin")}` };
  const tool = (args) => {
    const r = runHoiiotool(args, { cwd: dir, env, timeoutMs: 120000 });
    if (r.code !== 0) throw new Error(`hoiiotool exit ${r.code}: ${r.stderr}`);
  };
  const layout = atlasLayout(1, 64), size = layout.frame;
  const errorsOf = (file, blend) => { const bytes = readFileSync(join(dir, file)); return validateTexture(decodeTga(bytes), bytes.length, { blend, layout }); };
  tool(["--evaloff", "--wildcardoff", "--pattern", `fill:left=1,0.5,0,1:right=1,0.5,0,0.03125`, `${size}x${size}`, "4", "--premult", "-d", "half", "-o", "ramp.exr"]);
  tool(packArgs(["ramp.exr"], layout, "packed.exr"));
  for (const blend of ["additive", "transparent"]) {
    tool(encodeArgs(blend, "packed.exr", `${blend}.tga`));
    const errors = errorsOf(`${blend}.tga`, blend);
    check(errors.length === 0, `the ${blend} encode passes its validators${errors.length ? `: ${errors.join("; ")}` : ""}`);
  }
  // Straight colour survives at every partial alpha: the writer divides once.
  // It divides the 8-bit premultiplied value, so colour is good to one
  // premultiplied step, 255 / alpha (alpha 21: 10/21 reads 121 for 128). The
  // plan's first recipe divided twice and read 255 for 128.
  const image = decodeTga(readFileSync(join(dir, "transparent.tga")));
  const drift = [];
  for (let x = PAD; x < PAD + size; x += 4) {
    const o = ((PAD + size / 2) * image.width + x) * 4, [r, g, b, a] = image.rgba.subarray(o, o + 4);
    const slack = 255 / a + 2;
    if (a >= 16 && (Math.abs(r - 255) > slack || Math.abs(g - 128) > slack || b > slack)) drift.push(`x ${x}: ${r},${g},${b} at alpha ${a}`);
  }
  check(drift.length === 0, `the transparent encode keeps (255,128,0) under partial alpha${drift.length ? `: ${drift.join("; ")}` : ""}`);
  // A soft edge fading through the 8-bit rounding boundary (alpha 0.01 down to
  // 0.00001): every texel the file holds at alpha 0 must carry the fill, not
  // its own near-black premultiplied colour.
  tool(["--evaloff", "--wildcardoff", "--pattern", `fill:left=1,0.5,0,0.01:right=1,0.5,0,0.00001`, `${size}x${size}`, "4", "--premult", "-d", "half", "-o", "fade.exr"]);
  tool(packArgs(["fade.exr"], layout, "fade-packed.exr"));
  tool(encodeArgs("transparent", "fade-packed.exr", "fade.tga"));
  const fade = decodeTga(readFileSync(join(dir, "fade.tga"))), dark = [];
  let clear = 0;
  for (let x = PAD; x < PAD + size; x++) {
    const o = ((PAD + size / 2) * fade.width + x) * 4, [r, g, b, a] = fade.rgba.subarray(o, o + 4);
    if (a === 0) { clear++; if (r < 250 || Math.abs(g - 128) > 6 || b > 6) dark.push(`x ${x}: ${r},${g},${b}`); }
  }
  const fadeErrors = errorsOf("fade.tga", "transparent");
  check(clear > 0 && dark.length === 0 && fadeErrors.length === 0,
    `a soft edge's alpha-0 texels carry the fill (${clear} of ${size} clear${dark.length ? `; dark: ${dark.join("; ")}` : ""}${fadeErrors.length ? `; ${fadeErrors.join("; ")}` : ""})`);
  // Broken inputs: no fill under the holes, and an additive band that isn't black.
  tool(["--evaloff", "--wildcardoff", "packed.exr", "--clamp:min=0:max=1", "-d", "uint8", "--compression", "none", "-o", "unfilled.tga"]);
  check(errorsOf("unfilled.tga", "transparent").some((v) => /dark fringe/.test(v)), "an unfilled transparent texture fails on its dark fringe");
  tool(["--evaloff", "--wildcardoff", "--pattern", "constant:color=0.2,0.2,0.2,1", "64x64", "4", "-d", "half", "-o", "grey.exr"]);
  tool(encodeArgs("additive", "grey.exr", "grey.tga"));
  check(errorsOf("grey.tga", "additive").some((v) => /guard-band texels are not the black/.test(v)), "an additive texture with a grey band fails");
}

const base = mkdtempSync(join(tmpdir(), "pe-houdini-lane-"));
const root = join(base, "generated");
try {
  // 1. A miss, end to end.
  const first = await generate(root, 7);
  if (check(first.code === 0 && first.record?.hit === false, `debug-atlas generates (exit ${first.code}${first.record?.error ? `: ${first.record.error}` : ""})`)) {
    const errors = decodeTexture(first.record.published);
    check(errors.length === 0, `the texture holds the oracle's 16 colours in order, each frame upright in its cell${errors.length ? `: ${errors.join("; ")}` : ""}`);
    check(staged(root).length === 0, "the stage was removed after promotion");
    check(!alive(first.hython), "hython has exited");
    const { texture, published, contentHash, profile } = first.record;
    check(profile === "eaw-flipbook" && /^pe_gen_debug-atlas_[0-9a-f]{8}\.tga$/.test(texture) && texture.slice(-12, -4) === contentHash.slice(0, 8),
      `the texture is named for its bytes (${texture}, ${profile})`);
    check(published === join(root, "Data", "Art", "Textures", texture) && existsSync(published) && sha256(readFileSync(published)) === contentHash,
      "the texture is published under Data\\Art\\Textures, byte for byte");

    // The editor: from the root alone, fmt=21, the control differs, cells 0..15 in order.
    const validated = validate(first.record.entry, join(base, "validate"));
    check(validated.code === 0 && validated.record?.ok === true,
      `the editor validates it (exit ${validated.code}${validated.record?.error ? `: ${validated.record.error}` : ""})`);
    check(JSON.stringify(validated.record?.sequence) === JSON.stringify([...Array(16).keys()]),
      `the fireball fixture plays the cells 0..15 in order (${JSON.stringify(validated.record?.sequence ?? null)})`);

    // The cell steps the oracle predicts, recorded in provenance; then the stock baseline.
    const along = Math.round(8500 / 3) / 100, wrap = Math.round(34000 / 3) / 100;
    const steps = JSON.parse(readFileSync(join(first.record.entry, "provenance.json"), "utf8")).cellSteps?.steps ?? [];
    check(JSON.stringify(steps) === JSON.stringify([...Array(15).keys()].map((k) => (k % 4 === 3 ? wrap : along))),
      `provenance records the oracle's cell steps (${JSON.stringify(steps)})`);
    const stock = spawnSync(process.execPath, [cellStepsScript, "--stock", "ART\\TEXTURES\\P_PARTICLE_EXPLOSION1.DDS", "--grid", "4",
      "--blend", "additive"], { encoding: "utf8", windowsHide: true, timeout: 60000 });
    let baseline = null;
    try { baseline = JSON.parse((stock.stdout ?? "").split(/\r?\n/).find((v) => v.startsWith("[houdini-cell-steps] {")).slice(21)); } catch { /* below */ }
    check(stock.status === 0 && baseline?.ok === true && baseline.steps.length === 15 && baseline.mean > 0,
      `cell-steps.mjs measures the stock fire sheet (mean ${baseline?.mean ?? "?"}, largest ${baseline?.max ?? "?"})`);

    // The same inputs elsewhere: the same bytes.
    const elsewhere = await generate(join(base, "elsewhere"), 7);
    check(elsewhere.code === 0 && elsewhere.record?.hit === false && elsewhere.record.contentHash === contentHash,
      `a new root reproduces the texture byte for byte (${elsewhere.record?.contentHash ?? `exit ${elsewhere.code}`})`);

    // The same frames as a DDS, held to the TGA the oracle just passed: within
    // DXT5's bounds, a flipped sheet, a moved cell or a turned orientation mark
    // would not be.
    const dds = await generate(root, 7, { generator: "debug-atlas-dds" });
    if (check(dds.code === 0 && dds.record?.hit === false && /^pe_gen_debug-atlas-dds_[0-9a-f]{8}\.dds$/.test(dds.record.texture ?? ""),
      `debug-atlas-dds generates a DDS (exit ${dds.code}${dds.record?.error ? `: ${dds.record.error}` : `, ${dds.record?.texture}`})`)) {
      const bytes = readFileSync(dds.record.published), image = decodeDds(bytes);
      const ddsErrors = validateDds(image, decodeTga(readFileSync(published)), bytes.length,
        { format: "dxt5", blend: "additive", layout: atlasLayout(GRID, CELL) });
      check(ddsErrors.length === 0 && image.mipCount === Math.log2(GRID * CELL) + 1,
        `the DDS is the oracle's texture in DXT5 with a full mip chain${ddsErrors.length ? `: ${ddsErrors.join("; ")}` : ""}`);
      const ddsValidated = validate(dds.record.entry, join(base, "validate-dds"));
      check(ddsValidated.code === 0 && ddsValidated.record?.ok === true &&
        JSON.stringify(ddsValidated.record.sequence) === JSON.stringify([...Array(16).keys()]),
        `the editor loads the DDS as DXT5 and plays its cells 0..15 in order (exit ${ddsValidated.code}${ddsValidated.record?.error ? `: ${ddsValidated.record.error}` : ""}, ${JSON.stringify(ddsValidated.record?.sequence ?? null)})`);
      const ddsElsewhere = await generate(join(base, "elsewhere"), 7, { generator: "debug-atlas-dds" });
      check(ddsElsewhere.code === 0 && ddsElsewhere.record?.hit === false && ddsElsewhere.record.contentHash === dds.record.contentHash,
        `texconv reproduces the DDS byte for byte in a new root (${ddsElsewhere.record?.contentHash ?? `exit ${ddsElsewhere.code}`})`);
    }
  }
  profileEncodes(join(base, "profiles"));

  // 2. The same inputs are a verified hit.
  const second = await generate(root, 7);
  check(second.code === 0 && second.record?.hit === true && second.record.cacheKey === first.record?.cacheKey &&
    second.hython === null, "an identical re-run is a cache hit and starts no hython");

  // 3. One Houdini job per machine.
  const lock = await acquireLock();
  if (check(lock !== null, "the lane can take the machine lock")) {
    const busy = await generate(root, 8);
    await lock.release();
    check(busy.code === 7 && busy.hython === null, `a run while the lock is held exits 7 (exit ${busy.code})`);
  }

  // 4. Kill hython mid-run.
  const before = cached(root).length;
  const killed = await generate(root, 9, { onLaunched: (pid) => taskkill(pid) });
  check(killed.code === 8, `killing hython exits 8 (exit ${killed.code}${killed.record?.error ? `: ${killed.record.error}` : ""})`);
  check(killed.hython !== null && !alive(killed.hython), "hython is gone");
  check(staged(root).length === 0 && cached(root).length === before, "no stage and no cache entry are left");

  // 5. Kill the wrapper alone: the launcher sees its stdin close and kills the job.
  const orphan = await generate(root, 10, { onLaunched: (_pid, wrapper) => taskkill(wrapper.pid) });
  check(orphan.record === null, "the killed wrapper printed no result");
  check(orphan.hython !== null && await until(() => !alive(orphan.hython), 20000), "hython died with its wrapper");
  check(staged(root).length === 1 && cached(root).length === before, "the dead run left only its stage");
  const after = await generate(root, 10);
  check(after.code === 0 && after.record?.hit === false, `the next run succeeds (exit ${after.code})`);
  check(staged(root).length === 0, "and sweeps the abandoned stage");

  // 6. The generators, four seeds each at the validate tier: each passes its
  // profile, re-runs as a hit, reproduces byte for byte in a new root and
  // passes editor validation, and the seeds give four different textures.
  for (const generator of GENERATORS) {
    const hashes = new Set();
    for (const seed of [1, 2, 3, 4]) {
      const run = (where) => generate(join(base, where), seed, { generator, tier: "validate", minutes: 60 });
      const what = `${generator} seed ${seed}`, made = await run("seeds");
      if (!check(made.code === 0 && made.record?.hit === false, `${what} generates (exit ${made.code}${made.record?.error ? `: ${made.record.error}` : ""})`)) continue;
      hashes.add(made.record.contentHash);
      if (MASKED[generator]) {
        const edge = maskEdge(made.record.entry);
        check(edge <= EDGE_MAX, `${what}: the effect stays inside its edge mask (${edge}; at most ${EDGE_MAX})`);
      }
      const again = await run("seeds");
      check(again.code === 0 && again.record?.hit === true && again.record.cacheKey === made.record.cacheKey && again.hython === null,
        `${what}: an identical re-run is a cache hit`);
      const elsewhere = await run("seeds-elsewhere");
      check(elsewhere.code === 0 && elsewhere.record?.hit === false && elsewhere.record.contentHash === made.record.contentHash,
        `${what}: a new root reproduces the texture byte for byte (${elsewhere.record?.contentHash ?? `exit ${elsewhere.code}`})`);
      const validated = validate(made.record.entry, join(base, "validate-seeds", `${generator}-${seed}`));
      check(validated.code === 0 && validated.record?.ok === true,
        `${what}: the editor validates it (exit ${validated.code}${validated.record?.error ? `: ${validated.record.error}` : ""})`);
    }
    check(hashes.size === 4, `${generator}: four seeds give four textures (${hashes.size})`);
  }

  // 7. The parameters that push a masked effect furthest out still keep it
  // inside its edge mask.
  for (const [generator, params] of Object.entries(MASKED)) {
    const file = join(base, `${generator}-extreme.json`);
    writeFileSync(file, JSON.stringify(params));
    const made = await generate(join(base, "extremes"), 1, { generator, params: file, minutes: 60 });
    if (check(made.code === 0, `${generator} at ${JSON.stringify(params)} generates (exit ${made.code}${made.record?.error ? `: ${made.record.error}` : ""})`)) {
      const edge = maskEdge(made.record.entry);
      check(edge <= EDGE_MAX, `${generator} at ${JSON.stringify(params)} stays inside its edge mask (${edge}; at most ${EDGE_MAX})`);
    }
  }

  // 8. The review kit, from a final-tier ring (the cheapest final tier).
  const kitRoot = join(base, "kit");
  const ring = await generate(kitRoot, 1, { generator: "static-energy-ring", tier: "final" });
  if (check(ring.code === 0, `the ring generates at the final tier (exit ${ring.code}${ring.record?.error ? `: ${ring.record.error}` : ""})`)) {
    const staged = reviewKit(["--entry", ring.record.entry]);
    const { kit, texture } = staged.record ?? {};
    if (check(staged.code === 0 && staged.record?.ok === true && staged.record.kind === "ring",
      `the review kit stages (exit ${staged.code}${staged.record?.error ? `: ${staged.record.error}` : ""})`)) {
      const file = (...parts) => join(kit, "Data", ...parts);
      check(kit === join(kitRoot, "review", `static-energy-ring-${texture.slice(-12, -4)}`), "the kit is staged under <root>\\review\\<id>-<hash8>");
      check(sha256(readFileSync(file("Art", "Textures", texture))) === ring.record.contentHash, "the kit holds the texture byte for byte");
      const review = JSON.parse(readFileSync(join(kit, "review.json"), "utf8"));
      check(review.fixture?.lift === REVIEW_LIFT && review.fixture?.kind === "ring" && existsSync(file("Art", "Models", MARKER)),
        `the kit's ${MARKER} is the ring fixture, lifted ${REVIEW_LIFT}`);
      const stock = readStockFile([focRoot(resolveReplayGameRoots({}))], "XML\\PARTICLES.XML").toString("latin1");
      const xml = readFileSync(file("XML", "PARTICLES.XML")).toString("latin1");
      const at = stock.indexOf('<Particle Name="GUI_Move_Command_Particle">');
      const frames = stock.indexOf("<Particle_Lifetime_Frames>", at);
      const close = stock.indexOf("</Particle_Lifetime_Frames>", frames);
      check(at >= 0 && xml === `${stock.slice(0, frames)}<Particle_Lifetime_Frames>${MARKER_FRAMES}${stock.slice(close)}`,
        `the kit's PARTICLES.XML is the game's own, with only the click marker living ${MARKER_FRAMES} frames`);
      const again = reviewKit(["--entry", ring.record.entry]);
      check(again.code === 2, `staging the same kit again is refused (exit ${again.code})`);
      const recorded = reviewKit(["--record", kit, "--status", "passed", "--notes", "houdini lane"]);
      check(recorded.code === 0 && JSON.parse(readFileSync(join(kit, "review.json"), "utf8")).gameVerification?.status === "passed",
        `--record writes the verdict into review.json (exit ${recorded.code})`);
    }
  }
} finally {
  rmSync(base, { recursive: true, force: true, maxRetries: 10, retryDelay: 250 });
}

console.log(`[houdini-lane] ${failures.length ? `${failures.length} check(s) failed` : "all checks passed"}`);
process.exitCode = failures.length ? 1 : 0;
