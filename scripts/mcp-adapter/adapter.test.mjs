// [mcp-adapter] Unit tests: confinement, the process runner, each tool's argv
// and passthrough, the protocol, stdio, and the thinness guard. No exe needed;
// scripts/mcp-adapter/smoke.mjs drives the real one. Run: node --test scripts/mcp-adapter/*.test.mjs
import { test } from "node:test";
import assert from "node:assert/strict";
import * as fs from "node:fs";
import { spawn } from "node:child_process";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { confine, taskRoot } from "./confine.mjs";
import { runCommand } from "./run.mjs";
import { DEFINITIONS, OBSERVE_SCRIPT, SEARCH_SCRIPT, checkArguments, createTools } from "./tools.mjs";
import { PROTOCOL_VERSIONS, createProtocol } from "./protocol.mjs";
import { parseArgs } from "./server.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const node = process.execPath;

function sandbox() {
  const base = fs.realpathSync.native(fs.mkdtempSync(join(tmpdir(), "pe-mcp-test-")));
  const root = join(base, "root"), outside = join(base, "outside");
  fs.mkdirSync(join(root, "fx"), { recursive: true });
  fs.mkdirSync(outside);
  fs.writeFileSync(join(root, "fx", "fire.alo"), "alo");
  fs.writeFileSync(join(outside, "secret.alo"), "alo");
  const exe = join(base, "ParticleEditor.exe");
  fs.writeFileSync(exe, "not run by these tests");
  return { base, root: taskRoot(root), outside, exe, done: () => fs.rmSync(base, { recursive: true, force: true }) };
}

// ---- confinement -------------------------------------------------------------

test("confine accepts paths inside the root and refuses every way out", () => {
  const s = sandbox();
  try {
    assert.equal(confine(s.root, "fx/fire.alo", "file"), join(s.root, "fx", "fire.alo"));
    assert.equal(confine(s.root, "fx\\fire.alo", "file"), join(s.root, "fx", "fire.alo"));
    assert.equal(confine(s.root, "fx/new.alo", "output"), join(s.root, "fx", "new.alo"));
    assert.equal(confine(s.root, "bundles/run1", "folder"), join(s.root, "bundles", "run1"));
    for (const bad of ["", ".", "..", "../outside/secret.alo", "fx/../../outside/secret.alo", "fx//fire.alo", "./fx/fire.alo",
      join(s.outside, "secret.alo"), "C:/Windows/win.ini", "/etc/passwd", "\\\\server\\share\\x.alo", "fx/fire.alo:stream", "a\0b"])
      assert.throws(() => confine(s.root, bad, "output"), /task-root-relative/, JSON.stringify(bad));
    assert.throws(() => confine(s.root, "fx/missing.alo", "file"), /no such file/);
    assert.throws(() => confine(s.root, "fx", "file"), /no such file/, "a folder is not a file");
    assert.throws(() => confine(s.root, "fx", "output"), /not a plain file/);
    assert.throws(() => confine(s.root, "fx/fire.alo", "folder"), /not a plain folder/);
    assert.throws(() => confine(s.root, 7, "file"), /task-root-relative/);
  } finally { s.done(); }
});

test("confine refuses a junction or link that leads out of the root", () => {
  const s = sandbox();
  try {
    fs.symlinkSync(s.outside, join(s.root, "escape"), "junction");
    assert.throws(() => confine(s.root, "escape/secret.alo", "file"), /leaves the task root/);
    assert.throws(() => confine(s.root, "escape/new.alo", "output"), /leaves the task root/, "a write through it too");
    assert.throws(() => confine(s.root, "escape/deeper/new.alo", "output"), /leaves the task root/);
    fs.symlinkSync(join(s.root, "fx"), join(s.root, "inner"), "junction");
    assert.equal(confine(s.root, "inner/fire.alo", "file"), join(s.root, "fx", "fire.alo"), "a link that stays inside resolves");
    assert.throws(() => confine(s.root, "inner", "folder"), /not a plain folder/, "an output folder is never a link");
    assert.throws(() => taskRoot("relative/root"), /absolute/);
    assert.throws(() => taskRoot(join(s.root, "fx", "fire.alo")), /not a folder/);
  } finally { s.done(); }
});

// Win32 strips a segment's trailing dots and spaces, so "escape." opens the
// junction "escape" while Node's fs sees a missing name: the cheat check wrote
// outside the root this way through the real exe.
test("confine refuses names Windows would reinterpret, which once led through a junction", () => {
  const s = sandbox();
  try {
    fs.symlinkSync(s.outside, join(s.root, "escape"), "junction");
    for (const name of ["escape./x.alo", "escape /x.alo", "escape. /x.alo", "escape../x.alo", "escape.", "escape ", "escape.\\secret.alo"])
      for (const want of ["output", "folder", "file"])
        assert.throws(() => confine(s.root, name, want), /task-root-relative/, `${JSON.stringify(name)} as ${want}`);
    for (const name of ["fx/CON", "nul.txt", "fx/COM1.alo", "LPT9", "COM¹", "lpt³.txt", "conin$", "a<b.alo", "a?b.alo", "a*b", "a|b", "a\"b", "tab\there"])
      assert.throws(() => confine(s.root, name, "output"), /task-root-relative/, JSON.stringify(name));
    assert.equal(confine(s.root, "fx/confetti/console.alo", "output"), join(s.root, "fx", "confetti", "console.alo"), "a name merely starting with a device word is fine");
    assert.ok(!fs.existsSync(join(s.outside, "x.alo")));
  } finally { s.done(); }
});

test("confine refuses junctions to the root's parent or another drive, broken links, and hard links", (t) => {
  const s = sandbox();
  try {
    fs.symlinkSync(s.base, join(s.root, "up"), "junction");
    assert.throws(() => confine(s.root, "up/outside/secret.alo", "file"), /leaves the task root/);
    assert.throws(() => confine(s.root, "up", "folder"), /leaves the task root/, "a junction to the root's own parent");
    fs.symlinkSync(join(s.base, "gone"), join(s.root, "dangle"), "junction");
    assert.throws(() => confine(s.root, "dangle/x.alo", "output"), /broken link/);
    assert.throws(() => confine(s.root, "dangle", "output"), /broken link/, "a dangling link is not a missing name");
    fs.linkSync(join(s.outside, "secret.alo"), join(s.root, "hard.alo"));
    assert.throws(() => confine(s.root, "hard.alo", "file"), /hard link/);
    assert.throws(() => confine(s.root, "hard.alo", "output"), /hard link/);
    const otherDrive = ["D:\\", "E:\\", "C:\\"].find((d) => d[0].toLowerCase() !== s.root[0].toLowerCase() && fs.existsSync(d));
    if (!otherDrive) t.diagnostic("no second drive here: the cross-drive junction case was not exercised");
    else {
      fs.symlinkSync(otherDrive, join(s.root, "drive"), "junction");
      assert.throws(() => confine(s.root, "drive/x.alo", "output"), /leaves the task root/, `a junction to ${otherDrive}`);
    }
  } finally { s.done(); }
});

test("confine refuses an existing file symlink as an output", (t) => {
  const s = sandbox();
  try {
    try { fs.symlinkSync(join(s.root, "fx", "fire.alo"), join(s.root, "fx", "link.alo"), "file"); }
    catch (e) { if (e.code === "EPERM") return t.skip("file symlinks need Developer Mode or admin here"); throw e; }
    assert.throws(() => confine(s.root, "fx/link.alo", "output"), /not a plain file/);
  } finally { s.done(); }
});

// ---- the process runner --------------------------------------------------------

test("run returns exit code and output, and kills on timeout, output cap and cancel", async () => {
  const ok = await runCommand(node, ["-e", "process.stdout.write('hi'); process.stderr.write('warn'); process.exit(3)"], { timeoutMs: 20000 });
  assert.deepEqual(ok, { code: 3, stdout: "hi", stderr: "warn", stopped: null });
  const hang = await runCommand(node, ["-e", "setInterval(() => {}, 1000)"], { timeoutMs: 300 });
  assert.equal(hang.stopped, "timeout");
  const loud = await runCommand(node, ["-e", "setInterval(() => process.stdout.write('x'.repeat(65536)), 1)"], { timeoutMs: 20000, maxOutput: 100000 });
  assert.equal(loud.stopped, "output");
  assert.ok(loud.stdout.length <= 100000);
  const controller = new AbortController();
  const pending = runCommand(node, ["-e", "setInterval(() => {}, 1000)"], { timeoutMs: 20000, signal: controller.signal });
  setTimeout(() => controller.abort(), 200);
  assert.equal((await pending).stopped, "cancelled");
  const missing = await runCommand(join(tmpdir(), "no-such-program.exe"), [], { timeoutMs: 2000 });
  assert.equal(missing.code, -1);
  const gone = new AbortController();
  gone.abort();
  assert.equal((await runCommand(node, ["-e", "setInterval(() => {}, 1000)"], { timeoutMs: 20000, signal: gone.signal })).stopped, "cancelled",
    "a call cancelled before it starts is stopped at once");
  const big = await runCommand(node, ["-e", "process.stdout.write('x'.repeat((1 << 20) + 1)); setInterval(() => {}, 1000)"], { timeoutMs: 20000 });
  assert.equal(big.stopped, "output", "the default cap is 1 MiB");
  const fits = await runCommand(node, ["-e", "process.stdout.write('x'.repeat(1 << 20))"], { timeoutMs: 20000 });
  assert.equal(fits.stopped, null, "exactly the cap is allowed");
  assert.equal(fits.stdout.length, 1 << 20);
});

test("run finishes once the child exits, even if a descendant holds its pipes", async () => {
  const { PassThrough } = await import("node:stream");
  const { EventEmitter } = await import("node:events");
  const fakeSpawn = () => {
    const child = Object.assign(new EventEmitter(), { stdout: new PassThrough(), stderr: new PassThrough(), exitCode: null, signalCode: null, pid: 0 });
    setTimeout(() => { child.exitCode = 0; child.emit("exit", 0); }, 10); // and never "close"
    return child;
  };
  const started = Date.now();
  const r = await runCommand("x", [], { timeoutMs: 60000, spawnImpl: fakeSpawn });
  assert.equal(r.code, 0);
  assert.ok(Date.now() - started < 10000, "the grace period, not the deadline, ends it");
});

test("run kills the tree when the middle process is not Node", { skip: process.platform !== "win32" && "Windows only" }, async () => {
  const dir = fs.mkdtempSync(join(tmpdir(), "pe-mcp-tree-")), pidFile = join(dir, "pid"), grand = join(dir, "grand.js");
  try {
    fs.writeFileSync(grand, `require("fs").writeFileSync(${JSON.stringify(pidFile)}, String(process.pid)); setInterval(() => {}, 1000);`);
    // cmd.exe in the middle: killing it alone leaves the grandchild running.
    const viaCmd = (file, args, options) => spawn(file, args, { ...options, windowsVerbatimArguments: true });
    const r = await runCommand(process.env.ComSpec ?? "cmd.exe", ["/d", "/s", "/c", `""${node}" "${grand}""`],
      { timeoutMs: 2500, spawnImpl: viaCmd });
    assert.equal(r.stopped, "timeout");
    const pid = Number(fs.readFileSync(pidFile, "utf8"));
    const alive = () => { try { process.kill(pid, 0); return true; } catch { return false; } };
    for (let i = 0; i < 50 && alive(); i++) await new Promise((r) => setTimeout(r, 100));
    const survived = alive();
    if (survived) process.kill(pid);
    assert.equal(survived, false, "the grandchild under cmd.exe is gone too");
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

test("run kills the whole process tree", async () => {
  const dir = fs.mkdtempSync(join(tmpdir(), "pe-mcp-tree-")), pidFile = join(dir, "pid");
  try {
    const parent = `const c = require("child_process").spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], { stdio: "ignore" });
      require("fs").writeFileSync(${JSON.stringify(pidFile)}, String(c.pid)); setInterval(() => {}, 1000);`;
    const r = await runCommand(node, ["-e", parent], { timeoutMs: 1500 });
    assert.equal(r.stopped, "timeout");
    const pid = Number(fs.readFileSync(pidFile, "utf8"));
    const alive = () => { try { process.kill(pid, 0); return true; } catch { return false; } };
    for (let i = 0; i < 50 && alive(); i++) await new Promise((r) => setTimeout(r, 100));
    assert.equal(alive(), false, "the grandchild is gone too");
  } finally { fs.rmSync(dir, { recursive: true, force: true }); }
});

// ---- the tools -------------------------------------------------------------------

// A stand-in for the exe and the scripts: records each call and writes what the
// real command would, so passthrough can be checked byte for byte.
function fakeRun(behaviour = {}) {
  const calls = [];
  const run = async (file, args, options) => {
    calls.push({ file, args, options });
    const at = (flag) => args[args.indexOf(flag) + 1];
    const b = behaviour[args[0] === "--tool" ? args[1] : args[0] === OBSERVE_SCRIPT ? "observe" : "search"] ?? {};
    if (b.write) for (const [flag, text] of Object.entries(b.write)) fs.writeFileSync(flag === "outDir" ? join(args[2], "manifest.json") : at(flag), text);
    if (b.seeRequest) b.seeRequest(fs.readFileSync(args[3], "utf8"));
    if (b.seeSpec) b.seeSpec(JSON.parse(fs.readFileSync(args[1], "utf8")));
    return { code: b.code ?? 0, stdout: b.stdout ?? "", stderr: b.stderr ?? "", stopped: b.stopped ?? null };
  };
  return { run, calls };
}
const texts = (r) => r.content.map((c) => c.text);
const INSPECT = '{ "schema" :1,\n  "sourceDocumentHash": "ab" }\n';

test("inspect and validate run the exe on the confined source and return its JSON unchanged", async () => {
  const s = sandbox();
  try {
    const f = fakeRun({ inspect: { write: { "--out": INSPECT } }, validate: { code: 3, stderr: "repairs listed", write: { "--out": '{"valid":false}' } } });
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    const r = await tools.call("particle_inspect", { source: "fx/fire.alo" });
    assert.deepEqual(r, { content: [{ type: "text", text: INSPECT }], isError: false });
    assert.equal(f.calls[0].file, s.exe);
    assert.deepEqual(f.calls[0].args.slice(0, 4), ["--tool", "inspect", join(s.root, "fx", "fire.alo"), "--out"]);
    assert.equal(f.calls[0].options.cwd, s.root);
    const v = await tools.call("particle_validate", { source: "fx/fire.alo" });
    assert.equal(v.isError, false, "an invalid file is an answer, not a tool error");
    assert.deepEqual(texts(v), ['{"valid":false}', "exit 3: the source did not load exactly as written\nrepairs listed"]);
    assert.ok(!fs.existsSync(f.calls[0].args[4]), "the temporary output is removed");
  } finally { s.done(); }
});

test("a failing or stopped command is a tool error naming the exit code", async () => {
  const s = sandbox();
  try {
    const f = fakeRun({ inspect: { code: 3, stderr: "bad float at emitters[0].lifetime" }, validate: { stopped: "timeout", code: -1 } });
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    const r = await tools.call("particle_inspect", { source: "fx/fire.alo" });
    assert.equal(r.isError, true);
    assert.deepEqual(texts(r), ["exit 3: the source did not load exactly as written\nbad float at emitters[0].lifetime"]);
    assert.match(texts(await tools.call("particle_validate", { source: "fx/fire.alo" }))[0], /stopped after 120 s/);
  } finally { s.done(); }
});

test("deadlines, caps, cancellation and failed exits reach each tool's result", async () => {
  const s = sandbox();
  try {
    fs.writeFileSync(join(s.root, "catalog.json"), "{}");
    const f = fakeRun();
    const config = { root: s.root, exe: s.exe, gameRoots: [] };
    const tools = createTools(config, { run: f.run });
    const signal = new AbortController().signal;
    await tools.call("particle_inspect", { source: "fx/fire.alo" }, signal);
    await tools.call("particle_validate", { source: "fx/fire.alo" }, signal);
    await tools.call("particle_patch", { source: "fx/fire.alo", request: {}, candidate: "c.alo" }, signal);
    await tools.call("particle_asset_search", { catalog: "catalog.json", use: "fire" }, signal);
    await tools.call("particle_observe", { source: "fx/fire.alo", spec: { scenario: {} }, outDir: "b" }, signal);
    assert.deepEqual(f.calls.map((c) => c.options.timeoutMs), [120000, 120000, 120000, 120000, 1800000]);
    assert.ok(f.calls.every((c) => c.options.signal === signal), "the client's cancellation reaches every command");

    const loud = createTools(config, { run: fakeRun({ inspect: { stopped: "output", code: -1 } }).run });
    assert.match(texts(await loud.call("particle_inspect", { source: "fx/fire.alo" }))[0], /printed more than the output cap/);
    const cancelled = createTools(config, { run: fakeRun({ inspect: { stopped: "cancelled", code: -1 } }).run });
    assert.deepEqual(texts(await cancelled.call("particle_inspect", { source: "fx/fire.alo" })), ["cancelled"]);

    // A command that wrote its JSON and still failed is an error, and its JSON is not passed off as a result.
    const three = createTools(config, { run: fakeRun({ inspect: { code: 3, write: { "--out": INSPECT } } }).run });
    assert.equal((await three.call("particle_inspect", { source: "fx/fire.alo" })).isError, true, "only validate treats exit 3 as an answer");
    const half = createTools(config, { run: fakeRun({ inspect: { code: 13, write: { "--out": INSPECT } }, observe: { code: 1, write: { outDir: "{}" } } }).run });
    const r = await half.call("particle_inspect", { source: "fx/fire.alo" });
    assert.equal(r.isError, true);
    assert.deepEqual(texts(r), ["exit 13: a file could not be written"]);
    fs.mkdirSync(join(s.root, "b3"));
    const o = await half.call("particle_observe", { source: "fx/fire.alo", spec: { scenario: {} }, outDir: "b3" });
    assert.equal(o.isError, true);
    assert.deepEqual(texts(o), ["exit 1"]);

    const huge = createTools(config, { run: fakeRun({ inspect: { write: { "--out": "x".repeat((32 << 20) + 1) } } }).run });
    assert.match(texts(await huge.call("particle_inspect", { source: "fx/fire.alo" }))[0], /larger than 33554432 bytes/);

    for (const [args, message] of [[{ catalog: "catalog.json", limit: 101 }, /limit: expected integer/],
      [{ source: "fx/fire.alo", spec: { scenario: [] }, outDir: "b" }, /scenario: required/],
      [{ source: "fx/fire.alo", request: {}, candidate: "fx" }, /not a plain file/]]) {
      const name = "catalog" in args ? "particle_asset_search" : "spec" in args ? "particle_observe" : "particle_patch";
      assert.match(texts(await tools.call(name, args))[0], message, name);
    }
    assert.equal(f.calls.length, 5, "none of those started anything");
  } finally { s.done(); }
});

test("patch writes the request as given and the candidate inside the root; the report comes back unchanged", async () => {
  const s = sandbox();
  try {
    const request = { schema: 1, selector: { sourceDocumentHash: "ab", rolePath: [{ role: "root", name: "Fire" }] },
      mutations: [{ op: "setProperty", field: "lifetime", value: 0.5 }] };
    let seen;
    const report = '{"outcome":"ok","candidateHash":"cd"}';
    const f = fakeRun({ patch: { write: { "--report": report }, seeRequest: (t) => { seen = t; } } });
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    const r = await tools.call("particle_patch", { source: "fx/fire.alo", request, candidate: "fx/fire.candidate.alo" });
    assert.deepEqual(r, { content: [{ type: "text", text: report }, { type: "text", text: "candidate written: fx/fire.candidate.alo" }], isError: false });
    assert.deepEqual(JSON.parse(seen), request);
    const args = f.calls[0].args;
    assert.deepEqual([args[0], args[1], args[2], args[4], args[5]], ["--tool", "patch", join(s.root, "fx", "fire.alo"), "--out", join(s.root, "fx", "fire.candidate.alo")]);
    const refused = fakeRun({ patch: { code: 5, write: { "--report": '{"outcome":"refused"}' } } });
    const r2 = await createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: refused.run })
      .call("particle_patch", { source: "fx/fire.alo", request, candidate: "fx/c.alo" });
    assert.equal(r2.isError, true);
    assert.deepEqual(texts(r2), ['{"outcome":"refused"}', "exit 5: a rule refused an edit"]);
    assert.match(texts(await tools.call("particle_patch", { source: "fx/fire.alo", request, candidate: "../c.alo" }))[0], /task-root-relative/);
  } finally { s.done(); }
});

test("observe sets aloPath from the confined source, runs observe.mjs with the pinned exe, and returns the manifest", async () => {
  const s = sandbox();
  try {
    let spec;
    const manifest = '{"schemaVersion":1,"files":[]}';
    const f = fakeRun({ observe: { write: { outDir: manifest }, seeSpec: (v) => { spec = v; } } });
    const gameRoots = [s.outside];
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots }, { run: f.run });
    const want = { schemaVersion: 1, scenario: { schemaVersion: 1, seed: 7, maxFrames: 60 }, lifecycle: { type: "oneshot" } };
    fs.mkdirSync(join(s.root, "bundles", "run1"), { recursive: true });
    const r = await tools.call("particle_observe", { source: "fx/fire.alo", spec: want, outDir: "bundles/run1" });
    assert.deepEqual(texts(r), [manifest, "bundle written: bundles/run1"]);
    assert.deepEqual(spec, { ...want, scenario: { ...want.scenario, aloPath: join(s.root, "fx", "fire.alo") } });
    const { file, args, options } = f.calls[0];
    assert.equal(file, node);
    assert.deepEqual([args[0], args[2], ...args.slice(3)], [OBSERVE_SCRIPT, join(s.root, "bundles", "run1"), "--exe", s.exe, "--game-root", s.outside]);
    assert.equal(options.timeoutMs, 30 * 60_000);
    const bad = await tools.call("particle_observe", { source: "fx/fire.alo", spec: { scenario: { aloPath: "C:/elsewhere.alo" } }, outDir: "b2" });
    assert.match(texts(bad)[0], /aloPath: leave it out/);
    assert.match(texts(await tools.call("particle_observe", { source: "fx/fire.alo", spec: {}, outDir: "b2" }))[0], /scenario: required/);
    assert.equal(f.calls.length, 1, "a refused call starts nothing");
  } finally { s.done(); }
});

test("asset search passes each facet to search.mjs --json and returns its stdout", async () => {
  const s = sandbox();
  try {
    fs.writeFileSync(join(s.root, "catalog.json"), "{}");
    fs.writeFileSync(join(s.root, "recipes.json"), "{}");
    const f = fakeRun({ search: { stdout: '[{"kind":"asset"}]\n' } });
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    const r = await tools.call("particle_asset_search", { catalog: "catalog.json", recipes: "recipes.json", use: "smoke", blend: "additive", slot: "any", limit: 3 });
    assert.deepEqual(texts(r), ['[{"kind":"asset"}]\n'], "stdout unchanged, newline and all");
    assert.deepEqual(f.calls[0].args, [SEARCH_SCRIPT, "--catalog", join(s.root, "catalog.json"), "--json", "--recipes", join(s.root, "recipes.json"),
      "--blend", "additive", "--use", "smoke", "--slot", "any", "--limit", "3"]);
    const bad = fakeRun({ search: { code: 2, stderr: "[vfx-index] give at least one facet" } });
    const r2 = await createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: bad.run }).call("particle_asset_search", { catalog: "catalog.json" });
    assert.deepEqual(texts(r2), ["exit 2: a bad query\n[vfx-index] give at least one facet"]);
  } finally { s.done(); }
});

test("asset search end to end: the real search.mjs, through the adapter, returns what search() ranks", async () => {
  const s = sandbox();
  try {
    const { buildCatalog } = await import("../vfx-index/catalog.mjs");
    const { search } = await import("../vfx-index/search.mjs");
    const { recordsDoc, stockRecords } = await import("../vfx-index/testing.mjs");
    const catalog = buildCatalog(recordsDoc(stockRecords()));
    fs.writeFileSync(join(s.root, "catalog.json"), JSON.stringify(catalog));
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] });
    const r = await tools.call("particle_asset_search", { catalog: "catalog.json", use: "smoke", limit: 3 });
    assert.equal(r.isError, false, texts(r).join("\n"));
    assert.deepEqual(JSON.parse(texts(r)[0]), search(catalog, { use: "smoke" }, { limit: 3 }));
    const none = await tools.call("particle_asset_search", { catalog: "catalog.json" });
    assert.equal(none.isError, true);
    assert.match(texts(none)[0], /^exit 2: a bad query/);
  } finally { s.done(); }
});

test("arguments are checked before anything runs, and no argument picks what runs", async () => {
  const s = sandbox();
  try {
    const f = fakeRun();
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    for (const [name, args, message] of [
      ["particle_inspect", {}, /source: required/],
      ["particle_inspect", { source: "fx/fire.alo", exe: "C:/evil.exe" }, /exe: not an argument/],
      ["particle_inspect", { source: "" }, /source: expected string/],
      ["particle_patch", { source: "fx/fire.alo", request: [], candidate: "c.alo" }, /request: expected object/],
      ["particle_asset_search", { catalog: "c.json", limit: 0 }, /limit: expected integer/],
      ["particle_asset_search", { catalog: "c.json", slot: "diffuse" }, /slot: expected one of/],
      ["particle_inspect", { source: "fx/missing.alo" }, /no such file/],
    ]) {
      const r = await tools.call(name, args);
      assert.equal(r.isError, true, name);
      assert.match(texts(r)[0], message);
    }
    assert.equal(f.calls.length, 0);
    assert.deepEqual(checkArguments(DEFINITIONS[0].inputSchema, null), ["arguments must be an object"]);
    assert.equal(await tools.call("particle_render", {}), null);
  } finally { s.done(); }
});

// ---- the protocol ------------------------------------------------------------------

const rpc = (id, method, params) => ({ jsonrpc: "2.0", id, method, ...(params ? { params } : {}) });

test("initialize negotiates the version; tools/list lists the five tools; errors use JSON-RPC codes", async () => {
  const handle = createProtocol({ definitions: DEFINITIONS, call: async () => ({ content: [], isError: false }) });
  const init = await handle(rpc(1, "initialize", { protocolVersion: "2025-03-26", capabilities: {}, clientInfo: { name: "t", version: "1" } }));
  assert.equal(init.result.protocolVersion, "2025-03-26");
  assert.deepEqual(init.result.capabilities, { tools: { listChanged: false } });
  assert.equal((await handle(rpc(2, "initialize", { protocolVersion: "1999-01-01" }))).result.protocolVersion, PROTOCOL_VERSIONS[0]);
  assert.deepEqual((await handle(rpc(3, "tools/list"))).result.tools.map((t) => t.name),
    ["particle_inspect", "particle_validate", "particle_patch", "particle_observe", "particle_asset_search"]);
  assert.deepEqual(await handle(rpc(4, "ping")), { jsonrpc: "2.0", id: 4, result: {} });
  assert.equal((await handle(rpc(5, "resources/list"))).error.code, -32601);
  assert.equal((await handle(rpc(6, "tools/call", { name: "particle_render" }))).error.code, -32602);
  assert.equal(await handle({ jsonrpc: "2.0", method: "notifications/initialized" }), undefined);
  assert.equal(await handle({ jsonrpc: "2.0", id: 9, result: {} }), undefined, "a stray response is ignored");
  assert.equal((await handle(5)).error.code, -32600);
  assert.deepEqual(await handle({ id: 7, method: "ping" }), { jsonrpc: "2.0", id: 7, error: { code: -32600, message: "invalid request" } });
  const batch = await handle([rpc(10, "ping"), { jsonrpc: "2.0", method: "notifications/initialized" }, rpc(11, "ping")]);
  assert.deepEqual(batch.map((r) => r.id), [10, 11]);
  assert.equal((await handle([])).error.code, -32600);
  assert.equal(await handle([{ jsonrpc: "2.0", method: "notifications/initialized" }]), undefined, "a batch of notifications owes nothing");
});

test("tool calls run one at a time, and a cancelled call is stopped and gets no response", async () => {
  const events = [];
  let release;
  const tools = { definitions: DEFINITIONS, call: async (name, args, signal) => {
    events.push(`start ${args.tag}`);
    if (args.tag === "slow") await new Promise((r) => { release = r; signal.addEventListener("abort", () => r()); });
    events.push(`end ${args.tag}${signal.aborted ? " aborted" : ""}`);
    return { content: [{ type: "text", text: args.tag }], isError: false };
  } };
  const handle = createProtocol(tools);
  const slow = handle(rpc(1, "tools/call", { name: "particle_inspect", arguments: { tag: "slow" } }));
  const queued = handle(rpc(2, "tools/call", { name: "particle_inspect", arguments: { tag: "queued" } }));
  const after = handle(rpc(3, "tools/call", { name: "particle_inspect", arguments: { tag: "after" } }));
  await new Promise((r) => setTimeout(r, 20));
  assert.deepEqual(events, ["start slow"], "the others wait");
  await handle({ jsonrpc: "2.0", method: "notifications/cancelled", params: { requestId: 2 } });
  await handle({ jsonrpc: "2.0", method: "notifications/cancelled", params: { requestId: 1 } });
  assert.equal(await slow, undefined);
  assert.equal(await queued, undefined);
  assert.equal((await after).result.content[0].text, "after");
  assert.deepEqual(events, ["start slow", "end slow aborted", "start after", "end after"], "a cancelled queued call never starts");
  assert.ok(release);
});

test("when the client goes away, abortAll stops the running call and the queued ones never start", async () => {
  const started = [];
  const tools = { definitions: DEFINITIONS, call: async (name, args, signal) => {
    started.push(args.tag);
    await new Promise((r) => signal.addEventListener("abort", r));
    return { content: [], isError: true };
  } };
  const handle = createProtocol(tools);
  const running = handle(rpc(1, "tools/call", { name: "particle_observe", arguments: { tag: "a" } }));
  const queued = handle(rpc(2, "tools/call", { name: "particle_observe", arguments: { tag: "b" } }));
  await new Promise((r) => setTimeout(r, 20));
  handle.abortAll();
  assert.equal(await running, undefined);
  assert.equal(await queued, undefined);
  assert.deepEqual(started, ["a"]);
});

test("a tool that throws is an internal error, and the queue goes on", async () => {
  let n = 0;
  const handle = createProtocol({ definitions: DEFINITIONS, call: async () => { if (n++ === 0) throw new Error("boom"); return { content: [], isError: false }; } });
  assert.equal((await handle(rpc(1, "tools/call", { name: "particle_inspect", arguments: {} }))).error.code, -32603);
  assert.deepEqual((await handle(rpc(2, "tools/call", { name: "particle_inspect", arguments: {} }))).result, { content: [], isError: false });
});

// ---- the server over stdio ---------------------------------------------------------

test("the server checks its launch arguments", () => {
  const s = sandbox();
  try {
    const config = parseArgs(["--exe", s.exe, "--root", s.root, "--game-root", s.outside]);
    assert.deepEqual(config, { exe: s.exe, root: s.root, gameRoots: [s.outside] });
    const other = join(s.base, "notepad.exe");
    fs.writeFileSync(other, "");
    const exeMessage = /--exe must be the absolute path of an existing ParticleEditor\.exe/;
    for (const [argv, message] of [[[], /usage/], [["--exe", s.exe], /usage/], [["--root", s.root], /usage/],
      [["--exe", "ParticleEditor.exe", "--root", s.root], exeMessage], [["--exe", other, "--root", s.root], exeMessage],
      [["--exe", join(s.base, "x", "ParticleEditor.exe"), "--root", s.root], exeMessage],
      [["--exe", join(s.root, "fx"), "--root", s.root], exeMessage],
      [["--exe", s.exe, "--root", s.root, "--root", s.root], /given twice/], [["--exe", s.exe, "--root", s.root, "--tool", "x"], /bad argument: --tool/],
      [["--exe", s.exe, "--root", join(s.root, "fx", "fire.alo")], /not a folder/],
      [["--exe", s.exe, "--root", s.root, "--game-root", "rel"], /--game-root is not a folder/],
      [["--exe", s.exe, "--root", s.root, "--game-root", join(s.base, "nowhere")], /--game-root is not a folder/]])
      assert.throws(() => parseArgs(argv), message, JSON.stringify(argv));
  } finally { s.done(); }
});

test("over stdio, stdout carries only protocol lines and the log goes to stderr", async () => {
  const s = sandbox();
  try {
    const child = spawn(node, [join(here, "server.mjs"), "--exe", s.exe, "--root", s.root], { stdio: ["pipe", "pipe", "pipe"] });
    let stdout = "", stderr = "";
    child.stdout.on("data", (d) => { stdout += d; });
    child.stderr.on("data", (d) => { stderr += d; });
    const lines = () => stdout.split("\n").filter(Boolean);
    child.stdin.write(JSON.stringify(rpc(1, "initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "t", version: "1" } })) + "\n");
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", method: "notifications/initialized" }) + "\n");
    child.stdin.write("not json\n");
    child.stdin.write(JSON.stringify(rpc(2, "tools/list")) + "\n");
    child.stdin.write(JSON.stringify(rpc(3, "tools/call", { name: "particle_inspect", arguments: { source: "../outside/secret.alo" } })) + "\n");
    for (let i = 0; i < 100 && lines().length < 4; i++) await new Promise((r) => setTimeout(r, 50));
    child.stdin.end();
    await new Promise((r) => child.on("close", r));
    // JSON-RPC does not order replies: match them by id.
    const replies = new Map(lines().map((l) => JSON.parse(l)).map((r) => [r.id, r]));
    assert.deepEqual([...replies.keys()].sort(), [1, 2, 3, null].sort());
    assert.equal(replies.get(1).result.serverInfo.name, "particle-editor");
    assert.equal(replies.get(null).error.code, -32700);
    assert.equal(replies.get(2).result.tools.length, 5);
    assert.equal(replies.get(3).result.isError, true);
    assert.match(replies.get(3).result.content[0].text, /task-root-relative/);
    assert.match(stderr, /^\[mcp-adapter\] serving/m);
  } finally { s.done(); }
});

test("serve: closing stdin stops the call that is running", async () => {
  const { PassThrough } = await import("node:stream");
  const { serve } = await import("./server.mjs");
  const input = new PassThrough(), output = new PassThrough();
  let aborted;
  const stopped = new Promise((r) => { aborted = r; });
  const tools = { definitions: DEFINITIONS, call: (name, args, signal) => new Promise((r) => {
    signal.addEventListener("abort", () => { aborted(); r({ content: [], isError: true }); });
  }) };
  serve({ exe: "x", root: "y" }, { input, output, log: () => {}, tools });
  input.write(JSON.stringify(rpc(1, "tools/call", { name: "particle_observe", arguments: {} })) + "\n");
  await new Promise((r) => setTimeout(r, 20));
  input.end();
  const outcome = await Promise.race([stopped.then(() => "stopped"), new Promise((r) => setTimeout(() => r("still running"), 2000))]);
  assert.equal(outcome, "stopped");
});

// ---- thinness --------------------------------------------------------

test("the adapter imports only Node built-ins and its own modules, and starts processes only in run.mjs", () => {
  const sources = fs.readdirSync(here).filter((f) => f.endsWith(".mjs") && !f.endsWith(".test.mjs") && f !== "smoke.mjs");
  assert.deepEqual(sources.sort(), ["confine.mjs", "protocol.mjs", "run.mjs", "server.mjs", "tools.mjs"]);
  for (const file of sources) {
    const text = fs.readFileSync(join(here, file), "utf8");
    // Every line that mentions import, or re-exports from a module, must be one
    // plain import of an allowed module: no comment, no second statement.
    const allowed = new Set(["node:fs", "node:path", "node:url", "node:os", "node:readline",
      ...(file === "run.mjs" ? ["node:child_process"] : []), "./confine.mjs", "./protocol.mjs", "./run.mjs", "./tools.mjs"]);
    const lines = text.split(/\r?\n/).filter((line) => /\bimport\b(?!\.meta\b)|\bfrom\s*["']/.test(line));
    for (const line of lines) {
      const m = line.match(/^import (?:\* as [A-Za-z]+|\{ [A-Za-z0-9, ]+ \}) from "([^"]+)";$/);
      assert.ok(m && allowed.has(m[1]), `${file}: not a plain import of an allowed module: ${line}`);
    }
    if (file === "tools.mjs") assert.ok(lines.length >= 5, "the check sees tools.mjs's imports");
    assert.doesNotMatch(text, /\brequire\b|createRequire|getBuiltinModule|process\.binding|process\.dlopen|\bexecve\b|worker_threads|\bWorker\b|node:vm|\beval\b|new Function/,
      `${file} loads no code another way`);
    if (file !== "run.mjs") assert.doesNotMatch(text, /child_process|\bspawn|\bexecFile|\bfork\b/, `${file} starts no process`);
  }
});

test("every tool runs only the pinned exe or a fixed repo script", async () => {
  const s = sandbox();
  try {
    fs.writeFileSync(join(s.root, "catalog.json"), "{}");
    const f = fakeRun();
    const tools = createTools({ root: s.root, exe: s.exe, gameRoots: [] }, { run: f.run });
    await tools.call("particle_inspect", { source: "fx/fire.alo" });
    await tools.call("particle_validate", { source: "fx/fire.alo" });
    await tools.call("particle_patch", { source: "fx/fire.alo", request: {}, candidate: "c.alo" });
    await tools.call("particle_observe", { source: "fx/fire.alo", spec: { scenario: {} }, outDir: "b" });
    await tools.call("particle_asset_search", { catalog: "catalog.json", use: "fire" });
    assert.equal(f.calls.length, 5);
    for (const { file, args } of f.calls)
      assert.ok(file === s.exe || (file === node && [OBSERVE_SCRIPT, SEARCH_SCRIPT].includes(args[0])), `${file} ${args[0]}`);
    assert.ok(fs.existsSync(OBSERVE_SCRIPT) && fs.existsSync(SEARCH_SCRIPT));
  } finally { s.done(); }
});
