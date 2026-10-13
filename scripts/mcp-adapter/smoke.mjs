// [mcp-adapter] Smoke test against the real exe: starts the adapter as an MCP
// server over stdio, the way a client does, and checks each tool against the
// direct command on the guide's example effect. Run by the tool-smoke lane.
//
//   node scripts/mcp-adapter/smoke.mjs [--exe <ParticleEditor.exe>] [--observe --game-root <dir>...]
//
// --observe also compares particle_observe with scripts/observe.mjs; it needs
// the game data and a render desktop, so the lane leaves it out.
import { spawn, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { copyFileSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { compareRepeat } from "../observe.mjs";
import { FIXTURE, rootRequest, runTool } from "../tool-smoke.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, "..", "..");
const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");

// A minimal MCP client over the server's stdio.
export function startClient(args) {
  const child = spawn(process.execPath, [join(here, "server.mjs"), ...args], { stdio: ["pipe", "pipe", "pipe"], windowsHide: true });
  const waiting = new Map();
  let buffer = "", nextId = 1, stderr = "";
  child.stderr.on("data", (d) => { stderr += d; });
  child.stdout.on("data", (d) => {
    buffer += d;
    let n;
    while ((n = buffer.indexOf("\n")) >= 0) {
      const line = buffer.slice(0, n);
      buffer = buffer.slice(n + 1);
      if (!line.trim()) continue;
      const message = JSON.parse(line);
      waiting.get(message.id)?.(message);
      waiting.delete(message.id);
    }
  });
  const request = (method, params) => new Promise((resolveReply, reject) => {
    const id = nextId++;
    const timer = setTimeout(() => { waiting.delete(id); reject(new Error(`no reply to ${method}\n${stderr}`)); }, 40 * 60_000);
    waiting.set(id, (m) => { clearTimeout(timer); resolveReply(m); });
    child.stdin.write(JSON.stringify({ jsonrpc: "2.0", id, method, params }) + "\n");
  });
  const call = async (name, args) => (await request("tools/call", { name, arguments: args })).result;
  const close = () => new Promise((done) => { child.on("close", done); child.stdin.end(); });
  return { request, call, close, stderr: () => stderr };
}

export async function mcpSmoke(exe, dir, { observe = false, gameRoots = [], say = console.log } = {}) {
  const failures = [];
  const check = (ok, what) => { say(`  ${ok ? "ok:  " : "FAIL:"} ${what}`); if (!ok) failures.push(what); };
  const root = join(dir, "root"), direct = join(dir, "direct");
  mkdirSync(root);
  mkdirSync(direct);
  const source = join(root, "p_explosion_example.alo");
  copyFileSync(FIXTURE, source);
  const bytes = readFileSync(source), hash = sha256(bytes);
  const text = (r) => r?.content?.[0]?.text;

  const client = startClient(["--exe", resolve(exe), "--root", root, ...gameRoots.flatMap((g) => ["--game-root", g])]);
  try {
    say("mcp: initialize");
    const init = await client.request("initialize", { protocolVersion: "2025-06-18", capabilities: {}, clientInfo: { name: "mcp-smoke", version: "1" } });
    check(init.result?.serverInfo?.name === "particle-editor", "the server answers initialize");
    const list = await client.request("tools/list", {});
    check(list.result?.tools?.length === 5, "it lists five tools");

    say("mcp: inspect and validate match the direct --tool calls byte for byte");
    for (const verb of ["inspect", "validate"]) {
      const r = await client.call(`particle_${verb}`, { source: "p_explosion_example.alo" });
      check(runTool(exe, [verb, source, "--out", join(direct, `${verb}.json`)]).code === 0, `direct ${verb} exits 0`);
      check(r?.isError === false && text(r) === readFileSync(join(direct, `${verb}.json`), "utf8"), `particle_${verb} returns the exe's JSON unchanged`);
    }

    say("mcp: patch");
    const edit = [{ op: "setProperty", field: "lifetime", value: 0.5 }, { op: "rename", name: "Fire Core" }];
    const r = await client.call("particle_patch", { source: "p_explosion_example.alo", request: rootRequest(hash, "Fire", edit), candidate: "fire-core.alo" });
    const report = JSON.parse(text(r) ?? "null");
    check(r?.isError === false && report?.outcome === "ok", "particle_patch exits 0 with an ok report");
    writeFileSync(join(direct, "request.json"), JSON.stringify(rootRequest(hash, "Fire", edit)));
    runTool(exe, ["patch", source, join(direct, "request.json"), "--out", join(direct, "fire-core.alo"), "--report", join(direct, "report.json")]);
    const candidate = join(root, "fire-core.alo");
    check(existsSync(candidate) && readFileSync(candidate).equals(readFileSync(join(direct, "fire-core.alo"))),
      "the candidate is byte-identical to the direct patch's");
    check(report?.candidateHash === sha256(readFileSync(candidate)), "the report hashes the candidate written in the task root");
    const refused = await client.call("particle_patch", { source: "p_explosion_example.alo", request: rootRequest(hash, "Debris", edit.slice(0, 1)), candidate: "linked.alo" });
    check(refused?.isError === true && /^exit 5/.test(refused?.content?.[1]?.text ?? "") &&
      JSON.parse(text(refused) ?? "null")?.refusal?.category === "linked-target" && !existsSync(join(root, "linked.alo")),
      "a linked emitter is refused (exit 5) with its report, and nothing is written");
    const escape = await client.call("particle_patch", { source: "p_explosion_example.alo", request: rootRequest(hash, "Fire", edit), candidate: "../escape.alo" });
    check(escape?.isError === true && !existsSync(join(dir, "escape.alo")), "a candidate outside the task root is refused before the exe runs");
    // The exe strips a trailing dot and follows the junction; the adapter must refuse first.
    const outside = join(dir, "outside");
    mkdirSync(outside);
    symlinkSync(outside, join(root, "link"), "junction");
    for (const candidate of ["link/x.alo", "link./x.alo", "link /x.alo"]) {
      const r = await client.call("particle_patch", { source: "p_explosion_example.alo", request: rootRequest(hash, "Fire", edit), candidate });
      check(r?.isError === true && readdirSync(outside).length === 0, `a candidate through a junction out of the root is refused: ${candidate}`);
    }
    check(readFileSync(source).equals(bytes), "the source is unchanged");

    if (observe) {
      say("mcp: observe matches scripts/observe.mjs");
      const spec = { schemaVersion: 1, scenario: { schemaVersion: 1, seed: 305419896, algorithmVersion: 1, clockAnchor: 0,
        spawnPosition: [0, 0, 0], spawnVelocity: [0, 0, 0], maxFrames: 120, overloadGuardEnabled: true,
        overloadGuardLimit: 10000, estimatedParticlesPerInstance: 0 },
      lifecycle: { type: "oneshot", quietTailFrames: 15 }, output: { strideInterval: 30, playbackStride: 0, maxBytes: 209715200 } };
      const o = await client.call("particle_observe", { source: "p_explosion_example.alo", spec, outDir: "observed" });
      writeFileSync(join(direct, "observation.json"), JSON.stringify({ ...spec, scenario: { ...spec.scenario, aloPath: source } }));
      const d = spawnSync(process.execPath, [join(repoRoot, "scripts", "observe.mjs"), join(direct, "observation.json"), join(direct, "observed"),
        "--exe", exe, ...gameRoots.flatMap((g) => ["--game-root", g])], { encoding: "utf8", windowsHide: true });
      check(d.status === 0, `direct observe.mjs exits 0${d.status ? `: ${d.stderr}` : ""}`);
      check(o?.isError === false && text(o) === readFileSync(join(root, "observed", "manifest.json"), "utf8"),
        "particle_observe returns its bundle's manifest.json unchanged");
      // The rule observe.mjs --repeat applies: deterministic files byte for byte, and
      // the manifest without its `nondeterministic` block (log sizes name the folder).
      const differences = compareRepeat(join(root, "observed"), join(direct, "observed"));
      check(differences.length === 0, `the bundle matches observe.mjs's${differences.length ? `: ${differences.join("; ")}` : ""}`);
    }
  } finally {
    await client.close();
  }
  return failures;
}

async function main(argv) {
  const value = (flag) => { const i = argv.indexOf(flag); return i >= 0 ? argv[i + 1] : undefined; };
  const exe = resolve(value("--exe") ?? join(repoRoot, "x64", "Release", "ParticleEditor.exe"));
  if (!existsSync(exe)) { console.error(`[mcp-adapter] exe not found: ${exe}`); return 2; }
  const gameRoots = argv.flatMap((a, i) => a === "--game-root" ? [resolve(argv[i + 1])] : []);
  const dir = mkdtempSync(join(tmpdir(), "pe-mcp-smoke-"));
  try {
    const failures = await mcpSmoke(exe, dir, { observe: argv.includes("--observe"), gameRoots });
    console.log(failures.length === 0 ? "[mcp-adapter] smoke: all checks passed" : `[mcp-adapter] smoke: ${failures.length} check(s) failed`);
    return failures.length === 0 ? 0 : 1;
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) process.exitCode = await main(process.argv.slice(2));
