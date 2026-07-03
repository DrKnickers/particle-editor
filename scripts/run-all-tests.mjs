// [gate] Unified test gate — runs every automated test layer, exits nonzero on
// any failure.
//
//   node scripts/run-all-tests.mjs [flags]
//
// Lanes, cheap/fast first (later lanes depend on earlier build lanes):
//   lint              tsc --noEmit                        (web/apps/editor)
//   vitest            vitest run — the web unit/component suite
//   web-build         tsc -b && vite build -> dist/ (proof: dist/index.html mtime
//                     must ADVANCE — vite exit 0 alone doesn't prove output)
//   scripts           node --test script libs — MUST follow web-build: the
//                     no-test-seam-in-prod guard silently self-skips without dist/
//   playwright-web    mock-browser Playwright lane (own Vite server)
//   cpp-unit          all standalone tests/test_*.cpp except needs-exe ones
//   msbuild-debug     x64 Debug ParticleEditor.sln (test:native launches this exe)
//   cpp-unit-exe      needs-exe native tests against the FRESH Debug exe (explicit
//                     path — bare test_resource_strings would prefer stale Release)
//   playwright-native 41 specs vs the real app over CDP (pnpm test:native)
//   msbuild-release   x64 Release (drive-smoke hard-requires the Release exe)
//   render-goldens    deterministic --capture scenes vs checked-in goldens
//                     (ffmpeg SSIM ≥ 0.9995 — see scripts/render-goldens.mjs)
//   drive-smoke       tasks/drive-smoke.ps1 — real-pixel non-black --drive smoke
//                     + the oracle-step scenarios (assert-state / nonblack /
//                     production-wire bridge-selftest)
//
// Missing prereqs are FAILURES with actionable messages by default; silent skips
// are the enemy of a gate. `--allow-missing <lane>` downgrades that lane's missing
// prereq to a visible SKIP. A lane failure SKIPs lanes that depend on it (visible,
// counted as blocked, overall run still fails).
//
// Deliberately NOT in the default gate: test:site (writes placeholder media, needs
// FFmpeg) and a11y:drift (mutates goldens; test:native already runs a11y specs).
//
// Flags:
//   --lane <a,b,…>        run only these lanes (deps NOT auto-added)
//   --allow-missing <a,b> downgrade missing-prereq FAIL to SKIP for these lanes
//   --skip-build          skip web-build/msbuild lanes + native unit rebuilds
//                         (UNSAFE: asserts current artifacts are fresh)
//   --list                print lanes and exit

import { spawnSync } from "node:child_process";
import { existsSync, statSync, writeFileSync, readFileSync, unlinkSync } from "node:fs";
import { join, resolve, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const editorDir = join(repoRoot, "web", "apps", "editor");
const sln = join(repoRoot, "ParticleEditor.sln");
const debugExe = join(repoRoot, "x64", "Debug", "ParticleEditor.exe");
const releaseExe = join(repoRoot, "x64", "Release", "ParticleEditor.exe");
const distIndex = join(editorDir, "dist", "index.html");
const smokeFixture = join(editorDir, "tests", "fixtures", "a11y-base-state.alo");
const lockPath = join(repoRoot, ".gate.lock");

const argv = process.argv.slice(2);
function argList(name) {
  // Collect EVERY occurrence (--lane a --lane b) plus comma lists (--lane a,b).
  const out = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === name && argv[i + 1]) {
      out.push(...argv[i + 1].split(",").map((s) => s.trim()).filter(Boolean));
    }
  }
  return out;
}
const ONLY = argList("--lane");
const ALLOW_MISSING = new Set(argList("--allow-missing"));
const SKIP_BUILD = argv.includes("--skip-build");
const LIST = argv.includes("--list");

function log(msg) {
  console.log(`[gate] ${msg}`);
}

// ---------------------------------------------------------------------------
// Spawn helpers. .bat/.CMD shims (pnpm) can't be launched with shell:false, so
// they go through cmd.exe /d /s /c with verbatim args — the only reliable form
// for a repo path containing spaces ("Particle Editor").
function runCmdLine(commandLine, cwd) {
  const r = spawnSync("cmd.exe", ["/d", "/s", "/c", `"${commandLine}"`], {
    cwd,
    stdio: "inherit",
    shell: false,
    windowsVerbatimArguments: true,
  });
  return r.error ? 1 : (r.status ?? 1);
}
function runExe(cmd, args, cwd = repoRoot) {
  const r = spawnSync(cmd, args, { cwd, stdio: "inherit", shell: false });
  return r.error ? 1 : (r.status ?? 1);
}
function psCapture(command) {
  const r = spawnSync(
    "powershell.exe",
    ["-NoProfile", "-NonInteractive", "-Command", command],
    { encoding: "utf8", shell: false },
  );
  return { code: r.status ?? 1, out: (r.stdout || "").trim() };
}

// Resolve MSBuild via vswhere (same locator as a11y-drift-check.mjs — do not
// invent a third one).
function findMsbuild() {
  const vswhere = join(
    process.env["ProgramFiles(x86)"] || "C:/Program Files (x86)",
    "Microsoft Visual Studio", "Installer", "vswhere.exe",
  );
  const r = spawnSync(vswhere, ["-latest", "-find", "MSBuild\\**\\Bin\\MSBuild.exe"], {
    encoding: "utf8", shell: false,
  });
  const path = (r.stdout || "").split(/\r?\n/).find((l) => l.trim().endsWith("MSBuild.exe"));
  return path ? path.trim() : null;
}

let msbuildPath = null;
let restored = false;
function msbuild(config) {
  if (!msbuildPath) msbuildPath = findMsbuild();
  if (!msbuildPath) {
    log("MSBuild not found via vswhere — install VS with the C++ workload.");
    return 1;
  }
  if (!restored) {
    if (runExe(msbuildPath, [sln, "/t:Restore", "/p:RestorePackagesConfig=true", "/v:minimal"]) !== 0) return 1;
    restored = true;
  }
  return runExe(msbuildPath, [sln, `/p:Configuration=${config}`, "/p:Platform=x64", "/m", "/v:minimal"]);
}

// Build a single vcxproj for one x64 config, reusing the same MSBuild locator as
// msbuild() above. Used to materialize a prerequisite static lib without paying
// for a full-solution build. No /t:Restore: this is only used for package-free
// projects (e.g. expatw_static). Returns the process exit code.
function msbuildProject(projectPath, config) {
  if (!msbuildPath) msbuildPath = findMsbuild();
  if (!msbuildPath) {
    log("MSBuild not found via vswhere — install VS with the C++ workload.");
    return 1;
  }
  return runExe(msbuildPath, [projectPath, `/p:Configuration=${config}`, "/p:Platform=x64", "/m", "/v:minimal"]);
}

// ---------------------------------------------------------------------------
// Lane bodies. Each returns { status: "PASS"|"FAIL"|"SKIP", note? }.
const pass = { status: "PASS" };
const fail = (note) => ({ status: "FAIL", note });
const skip = (note) => ({ status: "SKIP", note });

// Missing prereq -> FAIL unless the user explicitly allowed it for this lane.
function prereq(lane, ok, what, hint) {
  if (ok) return null;
  const msg = `${what} — ${hint}`;
  if (ALLOW_MISSING.has(lane)) return skip(msg);
  log(`${lane}: missing prereq: ${msg} (or pass --allow-missing ${lane})`);
  return fail(`missing prereq: ${what}`);
}

const LANES = [
  {
    name: "lint",
    run: () => (runCmdLine("pnpm run lint", editorDir) === 0 ? pass : fail("tsc --noEmit")),
  },
  {
    name: "vitest",
    run: () => (runCmdLine("pnpm run test", editorDir) === 0 ? pass : fail("vitest run")),
  },
  {
    name: "web-build",
    run: () => {
      if (SKIP_BUILD) return skip("--skip-build");
      let before = -1;
      try { before = statSync(distIndex).mtimeMs; } catch { /* no prior dist */ }
      if (runCmdLine("pnpm run build", editorDir) !== 0) return fail("tsc -b && vite build");
      let after = -1;
      try { after = statSync(distIndex).mtimeMs; } catch { /* still missing */ }
      if (after <= before) return fail("build exited 0 but dist/index.html was not (re)produced");
      return pass;
    },
  },
  {
    name: "scripts",
    deps: ["web-build"],
    run: () => {
      // The no-test-seam-in-prod guard self-skips without dist/; require it.
      const p = prereq("scripts", existsSync(distIndex), "web dist/ missing", "run the web-build lane first");
      if (p) return p;
      return runCmdLine("pnpm run test:scripts", editorDir) === 0 ? pass : fail("node --test");
    },
  },
  {
    name: "playwright-web",
    run: () => (runCmdLine("pnpm run test:web", editorDir) === 0 ? pass : fail("mock-browser Playwright")),
  },
  {
    name: "cpp-unit",
    run: () => {
      // Four expat-linking tests (test_xml_billion_laughs -> Release, the other
      // three -> Debug) link a prebuilt expatw_static.lib via /LIBPATH but never
      // build it. On a fresh worktree that lib doesn't exist yet — the msbuild
      // lanes that produce it run AFTER this one — so the link fails with
      // LNK1181. Materialize it here (both configs) as a prereq. Building just
      // this one small static-lib project keeps the lane early and fast; a dev
      // machine with a prior full build simply rebuilds it as a no-op. (#483)
      if (!SKIP_BUILD) {
        const expatProj = join(repoRoot, "libs", "expat-2.2.0", "expatw_static.vcxproj");
        for (const cfg of ["Debug", "Release"]) {
          if (msbuildProject(expatProj, cfg) !== 0) {
            return fail(`expatw_static ${cfg} build (prereq for expat-linking unit tests)`);
          }
        }
      }
      const args = [join(repoRoot, "scripts", "run-native-unit-tests.mjs"), "--exclude-needs-exe"];
      if (SKIP_BUILD) args.push("--skip-build");
      return runExe(process.execPath, args) === 0 ? pass : fail("native unit lane");
    },
  },
  {
    name: "msbuild-debug",
    run: () => (SKIP_BUILD ? skip("--skip-build") : msbuild("Debug") === 0 ? pass : fail("x64 Debug build")),
  },
  {
    name: "cpp-unit-exe",
    deps: ["msbuild-debug"],
    run: () => {
      const p = prereq("cpp-unit-exe", existsSync(debugExe), "x64/Debug/ParticleEditor.exe missing", "run the msbuild-debug lane first");
      if (p) return p;
      const args = [
        join(repoRoot, "scripts", "run-native-unit-tests.mjs"),
        "--only-needs-exe", "--exe", debugExe,
      ];
      if (SKIP_BUILD) args.push("--skip-build");
      return runExe(process.execPath, args) === 0 ? pass : fail("needs-exe native tests");
    },
  },
  {
    name: "playwright-native",
    deps: ["web-build", "msbuild-debug"],
    run: () => {
      let p = prereq("playwright-native", existsSync(debugExe), "x64/Debug/ParticleEditor.exe missing", "run the msbuild-debug lane first");
      if (!p) p = prereq("playwright-native", existsSync(distIndex), "web dist/ missing", "run the web-build lane first");
      if (p) return p;
      // Preflight CDP port 9222. A stale --test-host is fine (run-native-tests
      // kills those itself); anything ELSE owning the port is an environment
      // error, distinct from a test failure. A daily-driver editor without
      // --test-host doesn't listen on 9222 and must never be flagged or killed.
      const probe = psCapture(
        "$c = Get-NetTCPConnection -LocalPort 9222 -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1; " +
        "if (-not $c) { 'FREE' } else { " +
        "$p = Get-CimInstance Win32_Process -Filter \"ProcessId=$($c.OwningProcess)\"; " +
        "if ($p.Name -eq 'ParticleEditor.exe' -and $p.CommandLine -like '*--test-host*') { 'TESTHOST' } " +
        "else { \"BUSY $($p.Name) pid $($p.ProcessId)\" } }",
      );
      if (probe.out.startsWith("BUSY")) {
        return fail(`CDP port 9222 owned by another process (${probe.out.slice(5)}) — close it and re-run`);
      }
      if (probe.code !== 0 || probe.out === "") {
        // Inconclusive probe (powershell/cmdlet failure): warn and proceed — a
        // genuinely busy port still fails loudly inside the lane itself.
        log(`playwright-native: port preflight inconclusive (probe exit ${probe.code}); proceeding.`);
      }
      return runCmdLine("pnpm run test:native", editorDir) === 0 ? pass : fail("native Playwright");
    },
  },
  {
    name: "msbuild-release",
    run: () => (SKIP_BUILD ? skip("--skip-build") : msbuild("Release") === 0 ? pass : fail("x64 Release build")),
  },
  {
    name: "render-goldens",
    deps: ["msbuild-release"],
    run: () => {
      const p = prereq("render-goldens", existsSync(releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first");
      if (p) return p;
      return runExe(process.execPath, [join(repoRoot, "scripts", "render-goldens.mjs")]) === 0
        ? pass
        : fail("render goldens (bless intentional changes with scripts/render-goldens.mjs --update)");
    },
  },
  {
    name: "drive-smoke",
    deps: ["msbuild-release", "web-build"],
    run: () => {
      // drive-smoke.ps1 only self-checks the exe; the gate owns the full prereq
      // trio + the fixture its A3 scenario silently skips without.
      const reg = spawnSync("reg.exe", ["query", "HKCU\\Software\\AloParticleEditor", "/v", "GameDataPath"], {
        encoding: "utf8", shell: false,
      });
      const checks = [
        [existsSync(releaseExe), "x64/Release/ParticleEditor.exe missing", "run the msbuild-release lane first"],
        [existsSync(distIndex), "web dist/ missing", "run the web-build lane first"],
        [reg.status === 0, "HKCU GameDataPath not set", "launch the editor once and pick the game data folder"],
        [existsSync(smokeFixture), `smoke fixture missing (${smokeFixture})`, "restore it from git"],
      ];
      for (const [ok, what, hint] of checks) {
        const p = prereq("drive-smoke", ok, what, hint);
        if (p) return p;
      }
      const code = runExe("powershell.exe", [
        "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
        "-File", join(repoRoot, "tasks", "drive-smoke.ps1"),
      ]);
      return code === 0 ? pass : fail(`drive smoke exit ${code}`);
    },
  },
];

// ---------------------------------------------------------------------------
// Single-instance lock: several native tests use fixed temp paths and the CDP
// port; two concurrent gates would corrupt each other. Stale locks (dead PID)
// are cleared automatically.
function acquireLock() {
  try {
    writeFileSync(lockPath, String(process.pid), { flag: "wx" });
    return true;
  } catch {
    let pid = NaN;
    try { pid = Number(readFileSync(lockPath, "utf8")); } catch { /* unreadable */ }
    if (Number.isFinite(pid) && pid > 0) {
      try {
        process.kill(pid, 0); // throws ESRCH if dead
        log(`another gate run is active (pid ${pid}, ${lockPath}) — refusing to run concurrently.`);
        return false;
      } catch (err) {
        // Only ESRCH proves the holder is dead. EPERM (etc.) means alive but
        // unsignalable from this context — that is NOT a stale lock.
        if (err.code !== "ESRCH") {
          log(`gate lock held by pid ${pid} (probe: ${err.code}) — refusing to run concurrently.`);
          return false;
        }
      }
    }
    log("clearing stale gate lock.");
    try { unlinkSync(lockPath); } catch { /* raced */ }
    try { writeFileSync(lockPath, String(process.pid), { flag: "wx" }); return true; } catch { return false; }
  }
}

function main() {
  const lanes = ONLY.length > 0 ? LANES.filter((l) => ONLY.includes(l.name)) : LANES;
  if (LIST) {
    for (const l of lanes) console.log(l.name);
    return 0;
  }
  const unknown = ONLY.filter((n) => !LANES.some((l) => l.name === n));
  if (unknown.length > 0) {
    log(`unknown lane(s): ${unknown.join(", ")} (use --list)`);
    return 1;
  }
  if (!acquireLock()) return 1;

  const results = [];
  try {
    for (const lane of lanes) {
      const blockedBy = (lane.deps || []).find(
        (d) => results.some((r) => r.name === d && r.status === "FAIL"),
      );
      const started = Date.now();
      let res;
      if (blockedBy) {
        res = skip(`blocked: ${blockedBy} failed`);
        log(`${lane.name}: SKIP (${res.note})`);
      } else {
        log(`=== ${lane.name} ===`);
        res = lane.run();
      }
      results.push({ name: lane.name, ...res, secs: (Date.now() - started) / 1000 });
    }
  } finally {
    try { unlinkSync(lockPath); } catch { /* already gone */ }
  }

  const width = Math.max(...results.map((r) => r.name.length));
  console.log("\n[gate] ================= summary =================");
  for (const r of results) {
    const note = r.note ? `  — ${r.note}` : "";
    console.log(`  ${r.name.padEnd(width)}  ${r.status.padEnd(4)}  ${r.secs.toFixed(1)}s${note}`);
  }
  if (SKIP_BUILD) {
    console.log("[gate] WARNING: --skip-build run — build lanes skipped; results may reflect STALE artifacts.");
  }
  const failed = results.filter((r) => r.status === "FAIL");
  const blocked = results.filter((r) => r.status === "SKIP" && (r.note || "").startsWith("blocked"));
  console.log(
    `[gate] ${results.filter((r) => r.status === "PASS").length} passed, ` +
      `${failed.length} failed, ${blocked.length} blocked, ` +
      `${results.filter((r) => r.status === "SKIP").length - blocked.length} skipped`,
  );
  return failed.length > 0 ? 1 : 0;
}

process.exit(main());
