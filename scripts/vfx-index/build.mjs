// [vfx-index] Build the catalog for one content stack.
//
//   node scripts/vfx-index/build.mjs --records <records.json> --out <catalog.json>
//   node scripts/vfx-index/build.mjs --exe <ParticleEditor.exe> --game-root <dir>... [--layer <dir>]...
//        --out <catalog.json> [--records <records.json>]
//
// With --exe it first runs `--tool index` (roots and layers as the tool takes
// them; layers highest first) and keeps its records beside the catalog. Local
// only: it reads the records and writes the catalog, nothing else.
import * as fs from "node:fs";
import { spawnSync } from "node:child_process";
import { pathToFileURL } from "node:url";
import { buildCatalog } from "./catalog.mjs";

const fail = (message, exitCode = 2) => Object.assign(new Error(message), { exitCode });

function parseArgs(argv) {
  const opts = { gameRoots: [], layers: [] };
  for (let i = 0; i < argv.length; i++) {
    const flag = argv[i], value = argv[++i];
    if (value == null) throw fail(`${flag} needs a value`);
    if (flag === "--records") opts.records = value;
    else if (flag === "--out") opts.out = value;
    else if (flag === "--exe") opts.exe = value;
    else if (flag === "--game-root") opts.gameRoots.push(value);
    else if (flag === "--layer") opts.layers.push(value);
    else throw fail(`unknown argument ${flag}`);
  }
  if (!opts.out) throw fail("--out is required");
  if (opts.exe ? !opts.gameRoots.length : !opts.records) throw fail("give --records, or --exe with at least one --game-root");
  if (!opts.exe && (opts.gameRoots.length || opts.layers.length)) throw fail("--game-root and --layer need --exe");
  return opts;
}

const spawn = (bin, args) => spawnSync(bin, args, { encoding: "utf8", windowsHide: true, timeout: 30 * 60 * 1000 });

export function main(argv = process.argv.slice(2), { io = fs, run = spawn, log = console.log } = {}) {
  const opts = parseArgs(argv);
  const records = opts.records ?? `${opts.out.replace(/\.json$/i, "")}.records.json`;
  if (opts.exe) {
    const args = ["--tool", "index", "--out", records, ...opts.layers.flatMap((l) => ["--layer", l]), ...opts.gameRoots];
    const r = run(opts.exe, args);
    if (r.error) throw fail(`could not run ${opts.exe}: ${r.error.message}`, 1);
    if (r.status !== 0) throw fail(`--tool index exited ${r.status}: ${String(r.stderr ?? "").trim().split("\n").slice(-3).join(" | ")}`, r.status ?? 1);
    for (const line of String(r.stderr ?? "").split("\n")) if (line.startsWith("[vfx-index]")) log(line.trim());
  }
  const catalog = buildCatalog(JSON.parse(io.readFileSync(records, "utf8")));
  const temp = `${opts.out}.tmp-${process.pid}`;
  io.writeFileSync(temp, JSON.stringify(catalog, null, 2) + "\n");
  io.renameSync(temp, opts.out);
  const s = catalog.summary;
  log(`[vfx-index] catalog: ${s.assets} assets, ${s.unresolved} unresolved, ${s.effects} effects, ${s.atlasConflicts} atlas conflicts -> ${opts.out}`);
  return catalog;
}

if (import.meta.url === pathToFileURL(process.argv[1] ?? "").href) {
  try { main(); } catch (error) { console.error(`[vfx-index] ${error.message}`); process.exit(error.exitCode ?? 1); }
}
