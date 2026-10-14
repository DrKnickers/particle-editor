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
//   - each profile's encode, through the real hoiiotool, passes its
//     validators, keeps colour where alpha is partial, and a broken input
//     fails them;
//   - an identical re-run is a verified cache hit;
//   - a run while the machine lock is held exits 7;
//   - killing hython mid-run exits 8, with no stage and no cache entry left;
//   - killing the wrapper alone (no tree kill) still ends hython, and the next
//     run sweeps the abandoned stage.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, win32 } from "node:path";
import { fileURLToPath } from "node:url";
import { HFS } from "./houdini.mjs";
import { acquireLock } from "./contain.mjs";
import { sha256 } from "./cache.mjs";
import { runHoiiotool } from "./generate.mjs";
import { decodeTga } from "./images.mjs";
import { PAD, atlasLayout, encodeArgs, packArgs, validateTexture } from "./profiles.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const generateScript = join(here, "generate.mjs");
const validateScript = join(here, "validate.mjs");
const system32 = win32.join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const GRID = 4, CELL = 32, FRAME = CELL - 2 * PAD;
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
function generate(root, seed, onLaunched = null) {
  const wrapper = spawn(process.execPath, [generateScript, "--generator", "debug-atlas", "--seed", String(seed),
    "--tier", "preview", "--out", root], { windowsHide: true, stdio: ["ignore", "pipe", "pipe"] });
  let stdout = "", stderr = "", hython = null;
  wrapper.stdout.on("data", (chunk) => { stdout += chunk; });
  wrapper.stderr.on("data", (chunk) => {
    stderr += chunk;
    const m = /\[houdini-gen\] launched (\d+)/.exec(stderr);
    if (m && hython === null) { hython = Number(m[1]); onLaunched?.(hython, wrapper); }
  });
  // A wrapper that never exits must not hang the gate: kill it, and say so.
  const timer = setTimeout(() => { stderr += "\n[houdini-lane] wrapper timed out; killed"; taskkill(wrapper.pid); }, 10 * 60000);
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

// validate.mjs as the editor-validation step runs it.
function validate(entry, out) {
  const r = spawnSync(process.execPath, [validateScript, "--entry", entry, "--out", out], { encoding: "utf8", windowsHide: true, timeout: 20 * 60000 });
  const line = (r.stdout ?? "").split(/\r?\n/).find((v) => v.startsWith("[houdini-validate] {"));
  try { return { code: r.status, record: line ? JSON.parse(line.slice(19)) : null }; } catch { return { code: r.status, record: null }; }
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

    // The same inputs elsewhere: the same bytes.
    const elsewhere = await generate(join(base, "elsewhere"), 7);
    check(elsewhere.code === 0 && elsewhere.record?.hit === false && elsewhere.record.contentHash === contentHash,
      `a new root reproduces the texture byte for byte (${elsewhere.record?.contentHash ?? `exit ${elsewhere.code}`})`);
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
  const killed = await generate(root, 9, (pid) => taskkill(pid));
  check(killed.code === 8, `killing hython exits 8 (exit ${killed.code}${killed.record?.error ? `: ${killed.record.error}` : ""})`);
  check(killed.hython !== null && !alive(killed.hython), "hython is gone");
  check(staged(root).length === 0 && cached(root).length === before, "no stage and no cache entry are left");

  // 5. Kill the wrapper alone: the launcher sees its stdin close and kills the job.
  const orphan = await generate(root, 10, (_pid, wrapper) => taskkill(wrapper.pid));
  check(orphan.record === null, "the killed wrapper printed no result");
  check(orphan.hython !== null && await until(() => !alive(orphan.hython), 20000), "hython died with its wrapper");
  check(staged(root).length === 1 && cached(root).length === before, "the dead run left only its stage");
  const after = await generate(root, 10);
  check(after.code === 0 && after.record?.hit === false, `the next run succeeds (exit ${after.code})`);
  check(staged(root).length === 0, "and sweeps the abandoned stage");
} finally {
  rmSync(base, { recursive: true, force: true, maxRetries: 10, retryDelay: 250 });
}

console.log(`[houdini-lane] ${failures.length ? `${failures.length} check(s) failed` : "all checks passed"}`);
process.exitCode = failures.length ? 1 : 0;
