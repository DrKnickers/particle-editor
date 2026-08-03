import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdtempSync, mkdirSync, writeFileSync, rmSync, existsSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import path from "node:path";

// Smoke test for scripts/package-release.ps1 (release-audit findings #1/#14): it must stage a
// COMPLETE bundle and FAIL LOUDLY on any missing piece, so a release zip can never be produced
// broken. We exercise the real PowerShell script against stub fixtures. CI's web job (Ubuntu) ships
// `pwsh`, so this runs there; locally it skips only if no PowerShell is found (the live Windows run
// is the local proof). It must NEVER skip under CI.
//
// The release is TWO FILES: ParticleEditor.exe (React UI embedded as RCDATA + statically-linked
// WebView2 loader) and the vendored d3dx9_43.dll beside it. No web/ folder, no WebView2Loader.dll,
// no bootstrapper — so the fixture and assertions model exactly that shape.
const SCRIPT = fileURLToPath(new URL("../../../../../scripts/package-release.ps1", import.meta.url));

let RESOLVED;
function resolveShell() {
  if (RESOLVED !== undefined) return RESOLVED;
  for (const cand of ["pwsh", "powershell"]) {
    const r = spawnSync(cand, ["-NoProfile", "-Command", "exit 0"], { encoding: "utf8" });
    if (!r.error && r.status === 0) return (RESOLVED = cand);
  }
  return (RESOLVED = null);
}
const shell = resolveShell();
const skipOpt = { skip: !shell && !process.env.CI ? "no pwsh/powershell on PATH" : false };

// Build a stub fixture repo. `omit` controls which pieces are missing (for the negative cases).
function buildFixture(root, omit = []) {
  const file = (rel, content = "stub") => {
    const p = path.join(root, rel);
    mkdirSync(path.dirname(p), { recursive: true });
    writeFileSync(p, content);
  };
  if (!omit.includes("exe")) {
    // The packager greps the exe for the embedded index.html's React mount sentinel
    // (id="root"). A real exe embeds it as RCDATA; the fixture bakes it into the stub so
    // the positive cases pass — omit "noembed" to model a stale pre-embed exe.
    const exeStub = omit.includes("noembed") ? "MZ stub no bundle" : 'MZ stub <div id="root"></div>';
    file("x64/Release/ParticleEditor.exe", exeStub);
  }
  if (!omit.includes("d3dx9")) file("libs/redist/d3dx9_43.dll");
  // NOTE: there is no stray-file negative control. Staging copies exactly the two named files
  // into a freshly-cleared dir — a stray in the source can never reach the stage, so "extra
  // artifact" pollution is impossible by construction (see package-release.ps1's post-stage note).
}

// Run the packager against a fresh temp fixture; returns { res, stage, zip }.
function runCase(
  t,
  omit = [],
  { withZip = true, passRepoRoot = true, useFixtureScript = false, stagePath } = {},
) {
  assert.ok(shell, "PowerShell (pwsh/powershell) is required under CI but was not found");
  const tmp = mkdtempSync(path.join(tmpdir(), "pkgrel-"));
  t.after(() => rmSync(tmp, { recursive: true, force: true }));
  const repo = path.join(tmp, "fixtures");
  const stage = stagePath ? stagePath({ tmp, repo }) : path.join(tmp, "stage");
  const zip = path.join(tmp, "out.zip");
  buildFixture(repo, omit);
  const script = useFixtureScript ? path.join(repo, "scripts", "package-release.ps1") : SCRIPT;
  if (useFixtureScript) {
    mkdirSync(path.dirname(script), { recursive: true });
    copyFileSync(SCRIPT, script);
  }
  const args = ["-NoProfile", "-File", script, "-Stage", stage];
  if (passRepoRoot) args.push("-RepoRoot", repo);
  if (withZip) args.push("-OutZip", zip);
  const res = spawnSync(shell, args, { encoding: "utf8" });
  assert.equal(res.error, undefined, `spawn error: ${res.error}`);
  return { res, repo, stage, zip };
}

const staged = (stage, ...parts) => existsSync(path.join(stage, ...parts));

test("case 1: full fixture stages the two-file bundle and a verified zip", skipOpt, (t) => {
  const { res, stage, zip } = runCase(t, []);
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "x64", "Release", "ParticleEditor.exe"), "exe staged");
  assert.ok(staged(stage, "x64", "Release", "d3dx9_43.dll"), "d3dx9 staged");
  // The WebView2 loader is statically linked into the exe (no WebView2Loader.dll)
  // and the runtime bootstrapper is no longer bundled, so NEITHER may be staged.
  assert.ok(!staged(stage, "x64", "Release", "WebView2Loader.dll"), "WebView2Loader.dll NOT staged (statically linked)");
  assert.ok(!staged(stage, "x64", "Release", "MicrosoftEdgeWebview2Setup.exe"), "WebView2 bootstrapper NOT staged");
  // The React UI is embedded in the exe as RCDATA — no separate web/ folder ships.
  assert.ok(!staged(stage, "web"), "no web/ folder staged (UI embedded in exe)");
  // Exit 0 with -OutZip means the script's own zip-entry assertions (exe, d3dx9) all
  // passed; confirm the archive exists and is non-empty.
  assert.ok(existsSync(zip) && statSync(zip).size > 0, "release zip created + verified");
});

test("case 1b: stages the two-file bundle WITHOUT -OutZip (post-stage asserts run)", skipOpt, (t) => {
  const { res, stage } = runCase(t, [], { withZip: false });
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "x64", "Release", "ParticleEditor.exe"), "exe staged");
  assert.ok(staged(stage, "x64", "Release", "d3dx9_43.dll"), "d3dx9 staged");
});

test("case 1c: -File default RepoRoot resolves from the script path", skipOpt, (t) => {
  const { res, stage, zip } = runCase(t, [], { passRepoRoot: false, useFixtureScript: true });
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "x64", "Release", "ParticleEditor.exe"), "default RepoRoot found fixture exe");
  assert.ok(existsSync(zip) && statSync(zip).size > 0, "release zip created + verified");
});

test("case 1d: refuses -Stage paths that overlap required sources before deleting them", skipOpt, (t) => {
  const { res, repo } = runCase(t, [], {
    withZip: false,
    stagePath: ({ repo }) => path.join(repo, "x64"),
  });
  assert.notEqual(res.status, 0, "expected non-zero exit");
  assert.match(
    res.stderr,
    /Refusing -Stage.*overlaps required source.*native release output/s,
    `stderr did not match:\n${res.stderr}`,
  );
  assert.ok(existsSync(path.join(repo, "x64", "Release", "ParticleEditor.exe")), "source fixture preserved");
});

// Negative cases: each must exit non-zero with the specific error substring (not just any failure).
for (const c of [
  { name: "case 2: missing exe", omit: ["exe"], rx: /Missing required source.*ParticleEditor\.exe/s },
  { name: "case 4: missing vendored d3dx9", omit: ["d3dx9"], rx: /Missing required source.*d3dx9_43\.dll/s },
  // The reviewer's scenario: an exe that staged fine but carries no embedded bundle
  // (a stale folder-mapping build) — must be rejected, not shipped as an unusable release.
  { name: "case 11: exe without an embedded bundle is rejected", omit: ["noembed"], rx: /does not embed the web bundle/ },
]) {
  test(c.name, skipOpt, (t) => {
    const { res } = runCase(t, c.omit);
    assert.notEqual(res.status, 0, `expected non-zero exit; stderr:\n${res.stderr}`);
    assert.match(res.stderr, c.rx, `stderr did not match ${c.rx}:\n${res.stderr}`);
  });
}
