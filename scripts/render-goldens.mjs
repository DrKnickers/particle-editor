// [gate] Render-golden lane — deterministic `--capture` scenes vs checked-in
// golden PNGs, compared with ffmpeg SSIM.
//
//   node scripts/render-goldens.mjs [--update] [--exe <path>]
//
// Each scene renders a fixture .alo through the REAL engine (x64 Release,
// no CDP) and must match tests/goldens/render/<name>.png with SSIM >= 0.9995.
// Optional per-scene fields: `args` (extra --capture CLI args, e.g. --frames /
// --ambient), and `requireTexGate` (texture names that MUST resolve for real —
// verified via the ALO_SHADER_DIAG [tex-gate] log — so a scene whose textures
// regress to the checkerboard placeholder fails loudly instead of being
// compared, or worse blessed, as placeholder pixels).
// Sim determinism: --capture seeds the particle PRNG and steps a frozen
// preview clock exactly 1/60 s per counted frame (HostWindow.cpp, #481), so
// consecutive captures are bit-identical even for RNG-using fixtures.
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
import { SSIM_MIN, ssim } from "./lib/ssim.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const goldenDir = join(repoRoot, "tests", "goldens", "render");
const fixturesDir = join(repoRoot, "web", "apps", "editor", "tests", "fixtures");

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
  // #481 regression guard: bump-mode (blend 11) emitter with hard alpha-cutout
  // sprites at small on-screen size. Guards the particle-bracket MIPFILTER=NONE
  // default end-to-end: a regression to trilinear re-smears the cutouts into
  // dark blobs and drops SSIM to ~0.998 (measured; gate is 0.9995). The
  // fixture's textures are REPO-COMMITTED (tests/fixtures/bumptest/*, resolved
  // CWD-relative from the repo root) so this scene needs no game install;
  // requireTexGate fails the scene loudly if resolution ever regresses to the
  // checkerboard placeholder, which could otherwise get silently blessed.
  {
    name: "bump-cutout",
    fixture: join(fixturesDir, "bump-asteroids.alo"),
    args: ["--frames", "45", "--ambient", "0.6,0.6,0.6", "--sun", "1,1,1"],
    requireTexGate: ["TESTS\\FIXTURES\\BUMPTEST\\ZZ_BUMPGOLD_ROCK.TGA",
                     "TESTS\\FIXTURES\\BUMPTEST\\ZZ_BUMPTEST_NM.TGA"],
  },
];

// A requireTexGate texture resolved for real iff its [tex-gate] getTexture(NAME)
// line is immediately followed by a "loaded WxH" tex-gate line — the placeholder
// fallback (D3DXCreateTextureFromResource) never logs "loaded". Order-sensitive
// on purpose: a later "loaded" for a DIFFERENT texture must not vouch for this one.
function texGateResolved(stdout, name) {
  const lines = (stdout || "").split(/\r?\n/).filter((l) => l.includes("[tex-gate]"));
  for (let i = 0; i < lines.length; i++) {
    if (lines[i].includes(`getTexture(${name})`)) {
      return i + 1 < lines.length && lines[i + 1].includes("loaded ");
    }
  }
  return false;
}

function log(msg) {
  console.log(`[gate] ${msg}`);
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
    // Two attempts per scene, but ONLY for ssim-exec failures (dimension
    // mismatch): the WebView2 layout can reflow between the gate opening and
    // the frame target under system load, yielding a wrong-SIZE capture with
    // no gate-timeout marker (the race the layout gate reduces but cannot
    // fully close). A low SSIM (right size, wrong pixels) is NEVER retried —
    // that's a real rendering change. The failed artifact is kept for diagnosis.
    let attempt = 0;
    retry: while (true) {
    attempt++;
    // Quiet by default: the engine logs shader diagnostics on stdout; surface
    // them only when the capture actually fails. Scenes with requireTexGate run
    // with ALO_SHADER_DIAG=1 so the [tex-gate] resolution log is available.
    // ALO_PARTICLE_MIPFILTER is stripped unconditionally: a caller's shell
    // override would otherwise make every capture exercise the override path —
    // and `--update` would silently BLESS those pixels as the golden.
    const env = { ...process.env, ALO_SHADER_DIAG: scene.requireTexGate ? "1" : undefined };
    delete env.ALO_PARTICLE_MIPFILTER;
    if (!scene.requireTexGate) delete env.ALO_SHADER_DIAG;
    const cap = spawnSync(exe, ["--capture", scene.fixture, out, ...(scene.args ?? [])], {
      cwd: repoRoot, encoding: "utf8", shell: false, timeout: 120000, env,
    });
    if (cap.error || cap.status !== 0 || !existsSync(out)) {
      log(`${scene.name}: FAIL — capture exit ${cap.error ? "spawn-error" : cap.status}`);
      if (cap.stdout) process.stdout.write(cap.stdout);
      if (cap.stderr) process.stderr.write(cap.stderr);
      failed++;
      break retry;
    }
    const unresolved = (scene.requireTexGate ?? []).filter((t) => !texGateResolved(cap.stdout, t));
    if (unresolved.length > 0) {
      // A missing texture renders the checkerboard placeholder — comparing (or
      // worse, blessing) that would silently gut the scene. Fail loudly instead.
      log(`${scene.name}: FAIL — required texture(s) not resolved: ${unresolved.join(", ")}`);
      failed++;
      break retry;
    }
    if ((cap.stdout || "").includes("layout-gate-timeout")) {
      // The host degraded to an ungated (racy-sized) capture — comparing that
      // against a fixed-size golden would flake; fail the scene deterministically.
      log(`${scene.name}: FAIL — capture ran with the layout gate timed out (racy size)`);
      failed++;
      break retry;
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
        const kept = join(tmpdir(), `render-golden-${scene.name}-failed-${process.pid}-a${attempt}.png`);
        copyFileSync(out, kept);
        log(`${scene.name}: ssim exec failed (${s.why}) — failed capture kept at ${kept}`);
        if (attempt < 2) {
          log(`${scene.name}: RETRY (transient capture-size race)`);
          rmSync(out, { force: true });
          continue retry;
        }
        log(`${scene.name}: FAIL — ${s.why} (persisted across retry)`);
        failed++;
      } else if (s.all < SSIM_MIN) {
        log(`${scene.name}: FAIL — SSIM ${s.all} < ${SSIM_MIN} (rendering changed; if intentional, --update and diff-review)`);
        failed++;
      } else {
        log(`${scene.name}: PASS — SSIM ${s.all}`);
      }
    }
    rmSync(out, { force: true });
    break;
    }  // retry loop
  }
  log(`render-goldens: ${SCENES.length - failed}/${SCENES.length} scenes ok`);
  return failed > 0 ? 1 : 0;
}

process.exit(main());
