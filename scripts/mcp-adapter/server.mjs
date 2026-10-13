// [mcp-adapter] Particle Editor as a local MCP server over stdio (design
// §4.6, Phase 11). A thin shell over ParticleEditor.exe and the repo's own
// scripts: it confines paths to one task root and passes their JSON through.
//
//   node scripts/mcp-adapter/server.mjs --exe <ParticleEditor.exe> --root <task folder>
//        [--game-root <dir>]...
//
// The exe and the root are fixed at launch, never by a tool call (§24.2).
// --game-root is passed to observe, as `scripts/observe.mjs` takes it. stdout
// carries protocol messages only; the log goes to stderr.
import * as fs from "node:fs";
import { basename, isAbsolute } from "node:path";
import { createInterface } from "node:readline";
import { pathToFileURL } from "node:url";
import { taskRoot } from "./confine.mjs";
import { createProtocol } from "./protocol.mjs";
import { createTools } from "./tools.mjs";

const usage = "usage: server.mjs --exe <ParticleEditor.exe> --root <task folder> [--game-root <dir>]...";

export function parseArgs(argv, io = fs) {
  const config = { gameRoots: [] };
  for (let i = 0; i < argv.length; i += 2) {
    const [flag, value] = [argv[i], argv[i + 1]];
    if (!["--exe", "--root", "--game-root"].includes(flag) || !value) throw new Error(`${usage}\nbad argument: ${flag}`);
    if (flag === "--game-root") config.gameRoots.push(value);
    else if (config[flag.slice(2)] !== undefined) throw new Error(`${flag} given twice`);
    else config[flag.slice(2)] = value;
  }
  if (!config.exe || !config.root) throw new Error(usage);
  if (!isAbsolute(config.exe) || basename(config.exe).toLowerCase() !== "particleeditor.exe" ||
      !io.existsSync(config.exe) || !io.statSync(config.exe).isFile())
    throw new Error(`--exe must be the absolute path of an existing ParticleEditor.exe: ${config.exe}`);
  for (const g of config.gameRoots)
    if (!isAbsolute(g) || !io.existsSync(g) || !io.statSync(g).isDirectory()) throw new Error(`--game-root is not a folder: ${g}`);
  return { exe: io.realpathSync.native(config.exe), root: taskRoot(config.root, io), gameRoots: config.gameRoots };
}

export function serve(config, { input = process.stdin, output = process.stdout,
  log = (line) => process.stderr.write(`[mcp-adapter] ${line}\n`), tools = createTools(config, { log }) } = {}) {
  const handle = createProtocol(tools, { log });
  const send = (message) => { if (message !== undefined) output.write(JSON.stringify(message) + "\n"); };
  const lines = createInterface({ input, crlfDelay: Infinity });
  log(`serving exe=${config.exe} root=${config.root}`);
  lines.on("line", (line) => {
    if (!line.trim()) return;
    let message;
    try { message = JSON.parse(line); } catch { return send({ jsonrpc: "2.0", id: null, error: { code: -32700, message: "parse error" } }); }
    handle(message).then(send, (e) => log(`unhandled: ${e.stack ?? e.message}`));
  });
  // MCP shutdown over stdio: the client closes our stdin. Nothing is left running.
  lines.on("close", () => { log("stdin closed; stopping"); handle.abortAll(); });
  return lines;
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  try { serve(parseArgs(process.argv.slice(2))); }
  catch (e) { process.stderr.write(`[mcp-adapter] ${e.message}\n`); process.exitCode = 2; }
}
