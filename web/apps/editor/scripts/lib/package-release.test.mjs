import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, existsSync, statSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import path from "node:path";

// Smoke test for scripts/package-release.ps1 (release-audit findings #1/#14): it must stage a
// COMPLETE bundle and FAIL LOUDLY on any missing piece, so a release zip can never be produced
// broken. We exercise the real PowerShell script against stub fixtures. CI's web job (Ubuntu) ships
// `pwsh`, so this runs there; locally it skips only if no PowerShell is found (the live Windows run
// is the local proof). It must NEVER skip under CI.
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
  if (!omit.includes("exe")) file("x64/Release/ParticleEditor.exe");
  if (!omit.includes("loader")) file("x64/Release/WebView2Loader.dll");
  if (!omit.includes("d3dx9")) file("libs/redist/d3dx9_43.dll");
  if (!omit.includes("dist")) {
    if (!omit.includes("index")) file("web/apps/editor/dist/index.html", "<html></html>");
    if (omit.includes("emptyassets")) mkdirSync(path.join(root, "web/apps/editor/dist/assets"), { recursive: true });
    else file("web/apps/editor/dist/assets/index-stub.js");
    if (omit.includes("emptyfonts")) mkdirSync(path.join(root, "web/apps/editor/dist/fonts"), { recursive: true });
    else file("web/apps/editor/dist/fonts/stub.woff2");
  }
}

// Run the packager against a fresh temp fixture; returns { res, stage, zip }.
function runCase(t, omit = [], { withZip = true } = {}) {
  assert.ok(shell, "PowerShell (pwsh/powershell) is required under CI but was not found");
  const tmp = mkdtempSync(path.join(tmpdir(), "pkgrel-"));
  t.after(() => rmSync(tmp, { recursive: true, force: true }));
  const repo = path.join(tmp, "fixtures");
  const stage = path.join(tmp, "stage");
  const zip = path.join(tmp, "out.zip");
  buildFixture(repo, omit);
  const args = ["-NoProfile", "-File", SCRIPT, "-Stage", stage, "-RepoRoot", repo];
  if (withZip) args.push("-OutZip", zip);
  const res = spawnSync(shell, args, { encoding: "utf8" });
  assert.equal(res.error, undefined, `spawn error: ${res.error}`);
  return { res, stage, zip };
}

const staged = (stage, ...parts) => existsSync(path.join(stage, ...parts));

test("case 1: full fixture stages a complete bundle and a verified zip", skipOpt, (t) => {
  const { res, stage, zip } = runCase(t, []);
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "x64", "Release", "ParticleEditor.exe"), "exe staged");
  assert.ok(staged(stage, "x64", "Release", "WebView2Loader.dll"), "WebView2Loader staged");
  assert.ok(staged(stage, "x64", "Release", "d3dx9_43.dll"), "d3dx9 staged");
  assert.ok(staged(stage, "web", "apps", "editor", "dist", "index.html"), "index.html staged");
  assert.ok(staged(stage, "web", "apps", "editor", "dist", "assets", "index-stub.js"), "assets staged");
  assert.ok(staged(stage, "web", "apps", "editor", "dist", "fonts", "stub.woff2"), "fonts staged");
  // Exit 0 with -OutZip means the script's own zip-entry assertions (exe, loader, d3dx9, index,
  // assets/*, fonts/*) all passed; confirm the archive exists and is non-empty.
  assert.ok(existsSync(zip) && statSync(zip).size > 0, "release zip created + verified");
});

test("case 1b: stages a complete bundle WITHOUT -OutZip (post-stage asserts run)", skipOpt, (t) => {
  const { res, stage } = runCase(t, [], { withZip: false });
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "web", "apps", "editor", "dist", "assets", "index-stub.js"), "assets staged");
  assert.ok(staged(stage, "web", "apps", "editor", "dist", "fonts", "stub.woff2"), "fonts staged");
});

// Negative cases: each must exit non-zero with the specific error substring (not just any failure).
for (const c of [
  { name: "case 2: missing exe", omit: ["exe"], rx: /Missing required source.*ParticleEditor\.exe/s },
  { name: "case 3: missing WebView2Loader", omit: ["loader"], rx: /Missing required source.*WebView2Loader\.dll/s },
  { name: "case 4: missing vendored d3dx9", omit: ["d3dx9"], rx: /Missing required source.*d3dx9_43\.dll/s },
  { name: "case 5: missing entire dist", omit: ["dist"], rx: /Missing required source.*dist/s },
  { name: "case 6: dist present but assets empty", omit: ["emptyassets"], rx: /no assets/ },
  { name: "case 7: dist present but index.html missing", omit: ["index"], rx: /Missing required source.*index\.html/s },
  { name: "case 8: dist present but fonts empty", omit: ["emptyfonts"], rx: /no fonts/ },
]) {
  test(c.name, skipOpt, (t) => {
    const { res } = runCase(t, c.omit);
    assert.notEqual(res.status, 0, `expected non-zero exit; stderr:\n${res.stderr}`);
    assert.match(res.stderr, c.rx, `stderr did not match ${c.rx}:\n${res.stderr}`);
  });
}
