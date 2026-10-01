// Generic builder for the standalone native unit tests and diagnostic tools.
//
//   node tests/build-native.mjs <name>... [--release] [--list]
//
// Reads tests/native-tests.json (see its _readme) and compiles each target with
// one shared command line: /W3 /WX like the production vcxproj, UNICODE, /Z7, a
// private obj dir per target and config (tests/obj/<name>/<config>/), so targets
// build in parallel. Debug = /MDd /Od /D_DEBUG -> tests/<name>.exe. A target with
// "release": true also gets a Release pass, /MD /O2 /DNDEBUG ->
// tests/<name>.release.exe, which runs the shipped (assert-free) code paths.
// Tools land in tests/tools/bin/<name>.exe, debug only unless --release.
//
// The toolchain comes from tests/_env.bat (vswhere -> vcvars64, DirectX SDK),
// captured once per process and handed to cl.exe / link.exe directly.
//
// scripts/run-native-unit-tests.mjs imports buildTargets() and validateManifest();
// the CLI is for building one test or tool by hand.

import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, readFileSync, readdirSync } from "node:fs";
import { availableParallelism } from "node:os";
import { basename, dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

export const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const testsDir = join(repoRoot, "tests");
const toolsDir = join(testsDir, "tools");
export const manifestPath = join(testsDir, "native-tests.json");

const FIELDS = new Set(["sources", "includes", "defines", "uses", "libs", "release", "wxExempt"]);
const USES = new Set(["dxsdk", "expat", "expat-lib", "webview2"]);
const WEBVIEW2_INCLUDE = "packages/Microsoft.Web.WebView2.1.0.3967.48/build/native/include";

export function loadManifest() {
  return JSON.parse(readFileSync(manifestPath, "utf8"));
}

// Every target in one flat map: name -> { kind, source, ...entry }.
export function targetsOf(manifest) {
  const out = new Map();
  for (const [kind, dir] of [["tests", testsDir], ["tools", toolsDir]]) {
    for (const [name, entry] of Object.entries(manifest[kind] ?? {})) {
      out.set(name, { name, kind, source: join(dir, `${name}.cpp`), ...entry });
    }
  }
  return out;
}

// Problems that make the manifest disagree with the tree. Empty array = clean.
// The lane runner refuses to start on any, and scripts/native-test-manifest.test.mjs
// runs it in the scripts lane so a new test_*.cpp without an entry fails early.
export function validateManifest(manifest = loadManifest()) {
  const problems = [];
  const targets = targetsOf(manifest);
  const onDisk = (dir) => (existsSync(dir) ? readdirSync(dir).filter((f) => /\.cpp$/i.test(f)) : []);
  for (const f of onDisk(testsDir).filter((f) => /^test_/i.test(f))) {
    const n = f.replace(/\.cpp$/i, "");
    if (!manifest.tests?.[n]) problems.push(`tests/${f} has no entry in tests/native-tests.json "tests"`);
  }
  for (const f of onDisk(toolsDir)) {
    const n = f.replace(/\.cpp$/i, "");
    if (!manifest.tools?.[n]) problems.push(`tests/tools/${f} has no entry in tests/native-tests.json "tools"`);
  }
  for (const f of onDisk(testsDir).filter((f) => !/^test_/i.test(f))) {
    problems.push(`tests/${f} is neither a test_*.cpp nor under tests/tools/`);
  }
  for (const t of targets.values()) {
    if (t.kind === "tests" && !t.name.startsWith("test_")) problems.push(`${t.name}: test names must start with test_`);
    if (!existsSync(t.source)) problems.push(`${t.name}: ${t.source} does not exist`);
    for (const k of Object.keys(t)) {
      if (!["name", "kind", "source"].includes(k) && !FIELDS.has(k)) problems.push(`${t.name}: unknown field "${k}"`);
    }
    for (const u of t.uses ?? []) if (!USES.has(u)) problems.push(`${t.name}: unknown use "${u}"`);
    const objNames = new Set([basename(t.source)]);
    for (const s of t.sources ?? []) {
      if (!existsSync(join(repoRoot, s))) problems.push(`${t.name}: source ${s} does not exist`);
      if (objNames.has(basename(s))) problems.push(`${t.name}: two sources named ${basename(s)} would share an .obj`);
      objNames.add(basename(s));
    }
    if (t.wxExempt !== undefined && (typeof t.wxExempt !== "string" || !/:\d+/.test(t.wxExempt))) {
      problems.push(`${t.name}: wxExempt must name the production file:line that warns`);
    }
  }
  return problems;
}

export function exePath(target, config) {
  if (target.kind === "tools") return join(toolsDir, "bin", `${target.name}${config === "release" ? ".release" : ""}.exe`);
  return join(testsDir, `${target.name}${config === "release" ? ".release" : ""}.exe`);
}

// Run tests/_env.bat once and capture the environment it leaves behind.
let toolchainEnv = null;
function captureToolchainEnv() {
  if (toolchainEnv) return toolchainEnv;
  const bat = join(testsDir, "_env.bat");
  const r = spawnSync("cmd.exe", ["/d", "/s", "/c", `""${bat}" >nul && set"`], {
    cwd: repoRoot,
    encoding: "utf8",
    shell: false,
    windowsVerbatimArguments: true,
    maxBuffer: 16 * 1024 * 1024,
  });
  if (r.status !== 0) {
    throw new Error(`tests/_env.bat failed (exit ${r.status}):\n${r.stderr || r.stdout}`);
  }
  const env = {};
  for (const line of r.stdout.split(/\r?\n/)) {
    const i = line.indexOf("=", 1);
    if (i > 0) env[line.slice(0, i)] = line.slice(i + 1);
  }
  if (!env.VCToolsInstallDir || !env.DXSDK_INC) throw new Error("tests/_env.bat did not set VCToolsInstallDir / DXSDK_INC");
  toolchainEnv = env;
  return env;
}

function compileFlags(t, config, env) {
  const uses = new Set(t.uses ?? []);
  if (uses.has("expat-lib")) uses.add("expat");
  const f = ["/nologo", "/c", "/EHsc", "/std:c++17", "/W3", "/Z7", "/FC",
             "/DUNICODE", "/D_UNICODE", "/D_WINDOWS", "/D_CRT_SECURE_NO_WARNINGS"];
  // wxExempt names a production warning the test can't fix (see the manifest).
  if (t.wxExempt === undefined) f.push("/WX");
  f.push(...(config === "release" ? ["/MD", "/O2", "/DNDEBUG"] : ["/MDd", "/Od", "/D_DEBUG"]));
  if (uses.has("expat")) f.push("/DXML_STATIC", "/DXML_UNICODE_WCHAR_T");
  for (const d of t.defines ?? []) f.push(`/D${d}`);
  // Same include order as the vcxproj: expat, DXSDK, src, WebView2.
  if (uses.has("expat")) f.push(`/I${join(repoRoot, "libs", "expat-2.8.5", "include")}`);
  if (uses.has("dxsdk")) f.push(`/I${env.DXSDK_INC}`);
  f.push(`/I${join(repoRoot, "src")}`);
  for (const inc of t.includes ?? []) f.push(`/I${join(repoRoot, inc)}`);
  if (uses.has("webview2")) f.push(`/I${join(repoRoot, WEBVIEW2_INCLUDE)}`);
  return f;
}

function linkFlags(t, config, env, objDir) {
  const uses = new Set(t.uses ?? []);
  const exe = exePath(t, config);
  const f = ["/nologo", "/DEBUG", "/INCREMENTAL:NO", `/OUT:${exe}`, `/PDB:${join(objDir, `${t.name}.pdb`)}`];
  if (uses.has("dxsdk")) f.push(`/LIBPATH:${env.DXSDK_LIB}`);
  if (uses.has("expat-lib")) {
    // expatw_static.lib follows the editor's CRT: Debug is /MDd (same as this
    // exe), Release is the static CRT (/MT). Only the Release pass therefore has
    // to drop the lib's libcmt default-lib request to link this exe's /MD CRT.
    f.push(`/LIBPATH:${join(repoRoot, "libs", "expat-2.8.5", "x64", config === "release" ? "Release" : "Debug")}`);
    if (config === "release") f.push("/NODEFAULTLIB:libcmt.lib");
    f.push("expatw_static.lib");
  }
  f.push(...(t.libs ?? []));
  return f;
}

function runTool(exe, args, env) {
  return new Promise((done) => {
    let out = "";
    const p = spawn(exe, args, { cwd: repoRoot, env, shell: false, windowsHide: true });
    p.stdout.on("data", (d) => { out += d; });
    p.stderr.on("data", (d) => { out += d; });
    p.on("error", (e) => done({ code: -1, out: `${out}${e}` }));
    p.on("close", (code) => done({ code, out }));
  });
}

function limiter(n) {
  let active = 0;
  const queue = [];
  const pump = () => {
    while (active < n && queue.length) {
      const { fn, ok, ko } = queue.shift();
      active++;
      fn().then(ok, ko).finally(() => { active--; pump(); });
    }
  };
  return (fn) => new Promise((ok, ko) => { queue.push({ fn, ok, ko }); pump(); });
}

// configs for one target: "debug" always, plus "release" for release:true
// targets (or every target when forceRelease is set, e.g. a tool by hand).
export function configsOf(target, forceRelease = false) {
  return target.release || forceRelease ? ["debug", "release"] : ["debug"];
}

// Build every (target, config) pair in parallel. Returns a Map keyed
// `${name}|${config}` -> { ok, secs, log }. Compiler output is returned, not
// printed, so parallel jobs don't interleave; callers print failures.
export async function buildTargets(names, { forceRelease = false, jobs = availableParallelism() } = {}) {
  const env = captureToolchainEnv();
  const binDir = join(env.VCToolsInstallDir, "bin", "Hostx64", "x64");
  const cl = join(binDir, "cl.exe");
  const link = join(binDir, "link.exe");
  const targets = targetsOf(loadManifest());
  const limit = limiter(Math.max(1, jobs));
  const results = new Map();

  const buildOne = async (t, config) => {
    const started = Date.now();
    const objDir = join(testsDir, "obj", t.name, config);
    mkdirSync(objDir, { recursive: true });
    mkdirSync(dirname(exePath(t, config)), { recursive: true });
    const flags = compileFlags(t, config, env);
    const sources = [t.source, ...(t.sources ?? []).map((s) => join(repoRoot, s))];
    const objs = sources.map((s) => join(objDir, basename(s).replace(/\.cpp$/i, ".obj")));
    const compiled = await Promise.all(sources.map((s, i) =>
      limit(() => runTool(cl, [...flags, `/Fo${objs[i]}`, s], env))));
    let log = compiled.map((c) => c.out).join("");
    let ok = compiled.every((c) => c.code === 0);
    if (ok) {
      const l = await limit(() => runTool(link, [...linkFlags(t, config, env, objDir), ...objs], env));
      log += l.out;
      ok = l.code === 0;
    }
    results.set(`${t.name}|${config}`, { ok, secs: (Date.now() - started) / 1000, log });
  };

  const work = [];
  for (const name of names) {
    const t = targets.get(name);
    if (!t) {
      results.set(`${name}|debug`, { ok: false, secs: 0, log: `${name}: not in tests/native-tests.json\n` });
      continue;
    }
    for (const config of configsOf(t, forceRelease)) work.push(buildOne(t, config));
  }
  await Promise.all(work);
  return results;
}

async function main() {
  const argv = process.argv.slice(2);
  const manifest = loadManifest();
  if (argv.includes("--list")) {
    for (const t of targetsOf(manifest).values()) console.log(`${t.kind === "tools" ? "tool " : "test "} ${t.name}${t.release ? "  (+release)" : ""}`);
    return 0;
  }
  const names = argv.filter((a) => !a.startsWith("--"));
  if (names.length === 0) {
    console.error("usage: node tests/build-native.mjs <name>... [--release] [--list]");
    return 2;
  }
  const results = await buildTargets(names, { forceRelease: argv.includes("--release") });
  let failed = 0;
  for (const [key, r] of results) {
    const [name, config] = key.split("|");
    if (!r.ok) { failed++; process.stdout.write(r.log); }
    const t = targetsOf(manifest).get(name);
    console.log(`${r.ok ? "built " : "FAILED"} ${name} [${config}]${r.ok && t ? ` -> ${exePath(t, config)}` : ""}`);
  }
  return failed ? 1 : 0;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  main().then((code) => process.exit(code), (e) => { console.error(String(e?.stack ?? e)); process.exit(1); });
}
