// Exercises release staging and package validation with stubs; does not launch the editor.
import { createHash } from "node:crypto";
import { test } from "node:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  mkdtempSync,
  mkdirSync,
  writeFileSync,
  rmSync,
  existsSync,
  statSync,
  readdirSync,
  readFileSync,
} from "node:fs";
import { fileURLToPath } from "node:url";
import { tmpdir } from "node:os";
import path from "node:path";

// Smoke test for scripts/package-release.ps1: it must stage a
// COMPLETE bundle and FAIL LOUDLY on any missing piece, so a release zip can never be produced
// broken. We exercise the real PowerShell script against stub fixtures. CI's web job (Ubuntu) ships
// `pwsh`, so this runs there; locally it skips only if no PowerShell is found (the live Windows run
// is the local proof). It must NEVER skip under CI.
//
// The release contains the exe, vendored D3DX9 runtime and verified engine-host package.
const SCRIPT = fileURLToPath(new URL("../../../../../scripts/package-release.ps1", import.meta.url));
const EDITOR_SHA256 = "a".repeat(64);

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
/** @param {string} root @param {string[]} omit @returns {void} */
function buildFixture(root, omit = []) {
  /** @param {string} rel @param {string} content @returns {void} */
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
  if (!omit.includes("license")) file("LICENSE", "editor license");
  if (!omit.includes("notices")) file("THIRD-PARTY-NOTICES.md", "third-party notices");
  if (!omit.includes("licenses")) file("licenses/Expat.txt", "Expat license");
  /** @type {Record<string, string>} */
  const packageContents = {
    "d3d9.dll": "host stub",
    "alamo_mapped.txt": "mapped builds stub",
    "scene/PREVIEW_SOURCE.TED": "scene stub",
    "layer/PREVIEWTEST.XML": "layer stub",
    "README.txt": "readme stub",
    [`signatures/${EDITOR_SHA256}.txt`]: "signature stub",
  };
  /** @type {Record<string, string>} */
  const hashes = {};
  for (const [relative, content] of Object.entries(packageContents)) {
    file(`alamo-preview/${relative}`, content);
    hashes[relative] = createHash("sha256").update(content).digest("hex");
  }
  file(
    "alamo-preview/package.json",
    JSON.stringify({
      format: 1,
      host_version: "test",
      editors: [{ sha256: EDITOR_SHA256, label: "test editor" }],
      files: hashes,
    }),
  );
  file("alamo-preview/unlisted.txt", "must not ship");
}

/**
 * @typedef {{ tmp: string, repo: string }} FixturePaths
 * @typedef {Object} PackageOptions
 * @property {boolean} [withZip]
 * @property {boolean} [passRepoRoot]
 * @property {boolean} [useFixtureScript]
 * @property {(paths: FixturePaths) => string} [stagePath]
 * @property {"package" | "skip" | "neither" | "both"} [engineHostMode]
 * @property {(packageFolder: string) => void} [mutatePackage]
 */
/**
 * @param {import("node:test").TestContext} t
 * @param {string[]} omit
 * @param {PackageOptions} options
 * @returns {{res: import("node:child_process").SpawnSyncReturns<string>, repo: string, stage: string, zip: string}}
 */
function runCase(
  t,
  omit = [],
  {
    withZip = true,
    passRepoRoot = true,
    useFixtureScript = false,
    stagePath,
    engineHostMode = "package",
    mutatePackage,
  } = {},
) {
  assert.ok(shell, "PowerShell (pwsh/powershell) is required under CI but was not found");
  const tmp = mkdtempSync(path.join(tmpdir(), "pkgrel-"));
  t.after(() => rmSync(tmp, { recursive: true, force: true }));
  const repo = path.join(tmp, "fixtures");
  const stage = stagePath ? stagePath({ tmp, repo }) : path.join(tmp, "stage");
  const zip = path.join(tmp, "out.zip");
  buildFixture(repo, omit);
  /** @type {string} */
  const packageFolder = path.join(repo, "alamo-preview");
  if (mutatePackage) mutatePackage(packageFolder);
  const script = useFixtureScript ? path.join(repo, "scripts", "package-release.ps1") : SCRIPT;
  if (useFixtureScript) {
    mkdirSync(path.dirname(script), { recursive: true });
    copyFileSync(SCRIPT, script);
  }
  const args = ["-NoProfile", "-File", script, "-Stage", stage];
  if (engineHostMode === "package" || engineHostMode === "both") args.push("-EngineHostPackage", packageFolder);
  if (engineHostMode === "skip" || engineHostMode === "both") args.push("-SkipEngineHost");
  if (passRepoRoot) args.push("-RepoRoot", repo);
  if (withZip) args.push("-OutZip", zip);
  const res = spawnSync(shell, args, { encoding: "utf8" });
  assert.equal(res.error, undefined, `spawn error: ${res.error}`);
  return { res, repo, stage, zip };
}

const staged = (stage, ...parts) => existsSync(path.join(stage, ...parts));

test("case 1: full fixture stages the complete bundle and a verified zip", skipOpt, (t) => {
  const { res, stage, zip } = runCase(t, []);
  assert.equal(res.status, 0, `expected success; stderr:\n${res.stderr}`);
  assert.ok(staged(stage, "x64", "Release", "ParticleEditor.exe"), "exe staged");
  assert.ok(staged(stage, "x64", "Release", "d3dx9_43.dll"), "d3dx9 staged");
  assert.equal(readFileSync(path.join(stage, "LICENSE"), "utf8"), "editor license");
  assert.equal(readFileSync(path.join(stage, "THIRD-PARTY-NOTICES.md"), "utf8"), "third-party notices");
  assert.equal(readFileSync(path.join(stage, "licenses", "Expat.txt"), "utf8"), "Expat license");
  // The WebView2 loader is statically linked into the exe (no WebView2Loader.dll)
  // and the runtime bootstrapper is no longer bundled, so NEITHER may be staged.
  assert.ok(
    !staged(stage, "x64", "Release", "WebView2Loader.dll"),
    "WebView2Loader.dll NOT staged (statically linked)",
  );
  assert.ok(!staged(stage, "x64", "Release", "MicrosoftEdgeWebview2Setup.exe"), "WebView2 bootstrapper NOT staged");
  // The React UI is embedded in the exe as RCDATA — no separate web/ folder ships.
  assert.ok(!staged(stage, "web"), "no web/ folder staged (UI embedded in exe)");
  assert.ok(staged(stage, "x64", "Release", "engine-host", "package.json"), "manifest staged");
  assert.ok(staged(stage, "x64", "Release", "engine-host", "scene", "PREVIEW_SOURCE.TED"), "nested file staged");
  assert.ok(!staged(stage, "x64", "Release", "engine-host", "unlisted.txt"), "unlisted file excluded");
  // Exit 0 with -OutZip means the script's own zip-entry assertions (including engine-host) all
  // passed; confirm the archive exists and is non-empty.
  assert.ok(existsSync(zip) && statSync(zip).size > 0, "release zip created + verified");
});

test("case 1b: stages the complete bundle WITHOUT -OutZip (post-stage asserts run)", skipOpt, (t) => {
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
  { name: "missing editor license", omit: ["license"], rx: /Missing required source.*LICENSE/s },
  { name: "missing third-party notices", omit: ["notices"], rx: /Missing required source.*THIRD-PARTY-NOTICES/s },
  { name: "missing licenses folder", omit: ["licenses"], rx: /Missing required source.*licenses/s },
  // The reviewer's scenario: an exe that staged fine but carries no embedded bundle
  // (a stale folder-mapping build) — must be rejected, not shipped as an unusable release.
  {
    name: "case 11: exe without an embedded bundle is rejected",
    omit: ["noembed"],
    rx: /does not embed the web bundle/,
  },
]) {
  test(c.name, skipOpt, (t) => {
    const { res } = runCase(t, c.omit);
    assert.notEqual(res.status, 0, `expected non-zero exit; stderr:\n${res.stderr}`);
    assert.match(res.stderr, c.rx, `stderr did not match ${c.rx}:\n${res.stderr}`);
  });
}

for (const [name, engineHostMode] of [
  ["neither", "neither"],
  ["both", "both"],
]) {
  test(`${name} package switches fail`, skipOpt, (t) => {
    const { res } = runCase(t, [], { engineHostMode });
    assert.notEqual(res.status, 0);
    assert.match(res.stderr, /exactly one of -EngineHostPackage or -SkipEngineHost/);
  });
}

test("hash mismatch fails naming the package file", skipOpt, (t) => {
  const { res } = runCase(t, [], {
    mutatePackage: (packageFolder) => writeFileSync(path.join(packageFolder, "d3d9.dll"), "damaged"),
  });
  assert.notEqual(res.status, 0);
  assert.match(res.stderr, /SHA-256 mismatch.*d3d9\.dll/s);
});

test("parent traversal in package files is refused", skipOpt, (t) => {
  const { res } = runCase(t, [], {
    mutatePackage: (packageFolder) => {
      /** @type {Record<string, unknown> & { files: Record<string, string> }} */
      const manifest = JSON.parse(readFileSync(path.join(packageFolder, "package.json"), "utf8"));
      manifest.files["../outside.dll"] = EDITOR_SHA256;
      writeFileSync(path.join(packageFolder, "package.json"), JSON.stringify(manifest));
    },
  });
  assert.notEqual(res.status, 0);
  assert.match(res.stderr, /Unsafe engine host package file path.*outside\.dll/s);
});

for (const field of ["host_version", "editors", "layer/PREVIEWTEST.XML", "files", `signatures/${EDITOR_SHA256}.txt`]) {
  test(`package missing ${field} is rejected before clearing the stage`, skipOpt, (t) => {
    const { res, stage } = runCase(t, [], {
      stagePath: ({ tmp }) => {
        /** @type {string} */
        const stage = path.join(tmp, "stage");
        mkdirSync(stage);
        writeFileSync(path.join(stage, "preserved.txt"), "previous stage");
        return stage;
      },
      mutatePackage: (packageFolder) => {
        /** @type {Record<string, unknown> & { files: Record<string, string> }} */
        const manifest = JSON.parse(readFileSync(path.join(packageFolder, "package.json"), "utf8"));
        if (field.includes("/")) delete manifest.files[field];
        else if (field === "files") manifest.files = {};
        else delete manifest[field];
        writeFileSync(path.join(packageFolder, "package.json"), JSON.stringify(manifest));
      },
    });
    assert.notEqual(res.status, 0);
    assert.match(
      res.stderr,
      /host_version|supported editors|missing (layer\/PREVIEWTEST\.XML|d3d9\.dll|signatures\/)/s,
    );
    assert.ok(existsSync(path.join(stage, "preserved.txt")), "invalid package leaves previous stage intact");
  });
}

test("package source overlap is refused before deletion", skipOpt, (t) => {
  const { res, repo } = runCase(t, [], { stagePath: ({ repo }) => path.join(repo, "alamo-preview") });
  assert.notEqual(res.status, 0);
  assert.match(res.stderr, /Refusing -Stage.*Alamo Preview package/s);
  assert.ok(existsSync(path.join(repo, "alamo-preview", "package.json")));
});

test("SkipEngineHost stages exactly two files with a smoke-test warning", skipOpt, (t) => {
  const { res, stage } = runCase(t, [], { engineHostMode: "skip" });
  assert.equal(res.status, 0, res.stderr);
  assert.deepEqual(readdirSync(path.join(stage, "x64", "Release")).sort(), ["ParticleEditor.exe", "d3dx9_43.dll"]);
  assert.match(res.stdout, /smoke-test layout, not a releasable zip/);
});
