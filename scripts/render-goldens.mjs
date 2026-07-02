// [gate] Render-golden lane — deterministic `--capture` scenes vs checked-in
// golden PNGs, compared with ffmpeg SSIM.
//
//   node scripts/render-goldens.mjs [--update] [--exe <path>]
//
// Each scene renders a fixture .alo through the REAL engine (x64 Release,
// no CDP) and must match tests/goldens/render/<name>.png with SSIM >= 0.9995.
// Threshold calibrated 2026-07-02 from two measurements: consecutive captures
// of the same scene are BIT-IDENTICAL (SSIM exactly 1.0, equal hashes — the
// capture path steps a fixed frame count, so the sim is deterministic), while
// two DIFFERENT scenes still score 0.9973 (dark, mostly-uniform viewports
// inflate SSIM). A loose 0.98 threshold therefore passes wrong content — the
// gate must sit between 0.9973 and 1.0. If cross-driver noise ever appears,
// loosen deliberately with new measurements, never below ~0.999.
//
// Goldens are produced on the machine that blesses them (GPU/driver
// specific). On a new machine, a wholesale mismatch on the FIRST run means
// "re-bless with --update and diff-review the images", not "rendering broke".
//
// Exit nonzero on: missing ffmpeg/exe/fixture, capture failure, dimension
// mismatch, SSIM below threshold, or missing golden (hint: --update).

import { spawnSync } from "node:child_process";
import { existsSync, mkdirSync, copyFileSync, rmSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const goldenDir = join(repoRoot, "tests", "goldens", "render");
const fixturesDir = join(repoRoot, "web", "apps", "editor", "tests", "fixtures");
const SSIM_MIN = 0.9995;

const argv = process.argv.slice(2);
const UPDATE = argv.includes("--update");
const exeArg = argv.indexOf("--exe");
const exe = resolve(
  repoRoot,
  exeArg >= 0 && argv[exeArg + 1] ? argv[exeArg + 1] : join("x64", "Release", "ParticleEditor.exe"),
);

const SCENES = [
  { name: "a11y-base-state", fixture: join(fixturesDir, "a11y-base-state.alo") },
  { name: "nt-5-singleton", fixture: join(fixturesDir, "nt-5-singleton.alo") },
];

function log(msg) {
  console.log(`[gate] ${msg}`);
}

function ssim(a, b) {
  // ffmpeg prints "SSIM R:… G:… B:… All:<x> (…)" on stderr.
  const r = spawnSync(
    "ffmpeg",
    ["-hide_banner", "-i", a, "-i", b, "-filter_complex", "ssim", "-f", "null", "-"],
    { encoding: "utf8", shell: false },
  );
  if (r.error) return { ok: false, why: `ffmpeg spawn failed (${r.error.code}) — is ffmpeg on PATH?` };
  const text = `${r.stderr || ""}${r.stdout || ""}`;
  // Full float syntax incl. scientific notation: a badly mismatched image can
  // emit "All:1.23e-05", which a bare [\d.]+ would truncate to a PASSING 1.23.
  const m = text.match(/All:([0-9.]+(?:[eE][+-]?[0-9]+)?)/);
  if (r.status !== 0 || !m || !Number.isFinite(Number(m[1]))) {
    // Dimension mismatches surface here: ffmpeg's ssim filter refuses
    // differently-sized inputs and exits nonzero.
    const dims = text.match(/Input link.*parameters.*do not match|width|height/i);
    return { ok: false, why: `ffmpeg ssim failed (exit ${r.status})${dims ? " — likely dimension mismatch" : ""}` };
  }
  return { ok: true, all: Number(m[1]) };
}

function main() {
  if (!existsSync(exe)) {
    log(`Release exe missing (${exe}) — build x64 Release first.`);
    return 1;
  }
  const probe = spawnSync("ffmpeg", ["-version"], { encoding: "utf8", shell: false });
  if (probe.error) {
    log("ffmpeg not found on PATH — required for SSIM comparison.");
    return 1;
  }
  mkdirSync(goldenDir, { recursive: true });

  let failed = 0;
  for (const scene of SCENES) {
    const golden = join(goldenDir, `${scene.name}.png`);
    const out = join(tmpdir(), `render-golden-${scene.name}-${process.pid}.png`);
    if (!existsSync(scene.fixture)) {
      log(`${scene.name}: FAIL — fixture missing (${scene.fixture})`);
      failed++;
      continue;
    }
    // Quiet by default: the engine logs shader diagnostics on stdout; surface
    // them only when the capture actually fails.
    const cap = spawnSync(exe, ["--capture", scene.fixture, out], {
      cwd: repoRoot, encoding: "utf8", shell: false, timeout: 120000,
    });
    if (cap.error || cap.status !== 0 || !existsSync(out)) {
      log(`${scene.name}: FAIL — capture exit ${cap.error ? "spawn-error" : cap.status}`);
      if (cap.stdout) process.stdout.write(cap.stdout);
      if (cap.stderr) process.stderr.write(cap.stderr);
      failed++;
      continue;
    }
    if ((cap.stdout || "").includes("layout-gate-timeout")) {
      // The host degraded to an ungated (racy-sized) capture — comparing that
      // against a fixed-size golden would flake; fail the scene deterministically.
      log(`${scene.name}: FAIL — capture ran with the layout gate timed out (racy size)`);
      failed++;
      continue;
    }
    if (UPDATE) {
      copyFileSync(out, golden);
      log(`${scene.name}: golden updated (${golden})`);
    } else if (!existsSync(golden)) {
      log(`${scene.name}: FAIL — no golden at ${golden} (bless with --update, then diff-review)`);
      failed++;
    } else {
      const s = ssim(golden, out);
      if (!s.ok) {
        log(`${scene.name}: FAIL — ${s.why}`);
        failed++;
      } else if (s.all < SSIM_MIN) {
        log(`${scene.name}: FAIL — SSIM ${s.all} < ${SSIM_MIN} (rendering changed; if intentional, --update and diff-review)`);
        failed++;
      } else {
        log(`${scene.name}: PASS — SSIM ${s.all}`);
      }
    }
    rmSync(out, { force: true });
  }
  log(`render-goldens: ${SCENES.length - failed}/${SCENES.length} scenes ok`);
  return failed > 0 ? 1 : 0;
}

process.exit(main());
