// [houdini] The opt-in `houdini` gate lane (plan §5 "Houdini lane"):
//   node scripts/run-all-tests.mjs --lane houdini
//
// Runs debug-atlas through the real wrapper, launcher, Job Object and Houdini
// in a throwaway root, then checks:
//   - the 16 frames decode to the oracle's colours in order, at the tier's size;
//   - an identical re-run is a verified cache hit;
//   - a run while the machine lock is held exits 7;
//   - killing hython mid-run exits 8, with no stage and no cache entry left;
//   - killing the wrapper alone (no tree kill) still ends hython, and the next
//     run sweeps the abandoned stage.
import { spawn, spawnSync } from "node:child_process";
import { mkdtempSync, readdirSync, rmSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, win32 } from "node:path";
import { fileURLToPath } from "node:url";
import { HFS, PINNED } from "./houdini.mjs";
import { acquireLock } from "./contain.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const generateScript = join(here, "generate.mjs");
const system32 = win32.join(process.env.SystemRoot ?? "C:\\Windows", "System32");
const GRID = 4, CELL = 32;
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

// Each frame must be one flat colour: red = i % grid, green = i / grid, blue 0.5.
function decodeFrames(entry) {
  const frames = Array.from({ length: GRID * GRID }, (_, i) => join(entry, "frames", `frame_${String(i + 1).padStart(4, "0")}.exr`));
  const r = spawnSync(PINNED.hoiiotool.path, ["--evaloff", "--wildcardoff", "--stats", ...frames], {
    encoding: "utf8", windowsHide: true, timeout: 120000,
    env: { SystemRoot: process.env.SystemRoot, PATH: `${system32};${win32.join(HFS, "bin")}` },
  });
  if (r.status !== 0) return [`hoiiotool exit ${r.status}: ${r.stderr}`];
  const blocks = r.stdout.split(/\r?\n(?=\S)/).filter((block) => / : +\d+ x +\d+,/.test(block));
  if (blocks.length !== frames.length) return [`hoiiotool reported ${blocks.length} frames`];
  const span = GRID - 1;
  return blocks.flatMap((block, i) => {
    const size = / : +(\d+) x +(\d+),/.exec(block);
    const colour = /Constant Color: ([\d.]+) ([\d.]+) ([\d.]+) ([\d.]+)/.exec(block)?.slice(1).map(Number);
    const want = [(i % GRID) / span, Math.floor(i / GRID) / span, 0.5, 1];
    const errors = [];
    if (Number(size[1]) !== CELL || Number(size[2]) !== CELL) errors.push(`frame ${i} is ${size[1]}x${size[2]}`);
    // Half floats hold 1/3 as 0.333252.
    if (!colour || colour.some((v, c) => Math.abs(v - want[c]) > 1e-3))
      errors.push(`frame ${i} is ${colour ? colour.join(",") : "not flat"}, not ${want.map((v) => v.toFixed(3)).join(",")}`);
    return errors;
  });
}

const base = mkdtempSync(join(tmpdir(), "pe-houdini-lane-"));
const root = join(base, "generated");
try {
  // 1. A miss, end to end.
  const first = await generate(root, 7);
  if (check(first.code === 0 && first.record?.hit === false, `debug-atlas generates (exit ${first.code}${first.record?.error ? `: ${first.record.error}` : ""})`)) {
    const errors = decodeFrames(first.record.entry);
    check(errors.length === 0, `the 16 frames decode to the oracle's colours in order${errors.length ? `: ${errors.join("; ")}` : ""}`);
    check(staged(root).length === 0, "the stage was removed after promotion");
    check(!alive(first.hython), "hython has exited");
  }

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
