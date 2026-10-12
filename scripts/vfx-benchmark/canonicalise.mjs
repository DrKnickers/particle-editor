// canonicalise (plan 3.4, 3.5): the stability proof of a canonical base. No
// script can save, so the owner does the save and this script verifies it:
//
//   node scripts/vfx-benchmark/canonicalise.mjs --prepare <store> <base>
//     copies bases/<base>/effect.alo to effect.first.alo and records its hash;
//   (the owner opens effect.alo in the editor and saves it in place, no edit)
//   node scripts/vfx-benchmark/canonicalise.mjs --verify <store> <base> [--exe <path>]
//     requires the re-save to be byte-identical and inspect-identical, deletes
//     the copy and records the proof as bases[i].canonical in corpus.json.
//
// Saving under the stem "effect" keeps the system name (every save re-stamps
// it from the destination file name); a Save As under another stem changes
// the bytes and is refused here.
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { spawnWithDeadline } from "../observe.mjs";
import { canonical } from "./semantic.mjs";
import { DEFAULT_EXE, sha256, store, tools } from "./store.mjs";

class CanonicaliseError extends Error {}

function openBase(root, base, io) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  if (corpus.state === "locked") throw new CanonicaliseError("the corpus is locked");
  const index = corpus.bases.findIndex((b) => b.id === base);
  if (index < 0) throw new CanonicaliseError(`no base ${base} in corpus.json`);
  if (!s.exists("bases", base, "effect.alo")) throw new CanonicaliseError(`bases/${base}/effect.alo is missing`);
  return { s, corpus, index };
}

export function prepare({ root, base, io = fs }) {
  const { s } = openBase(root, base, io);
  const bytes = s.read("bases", base, "effect.alo");
  s.write(["bases", base, "effect.first.alo"], bytes);
  s.writeJson(["bases", base, "effect.first.json"], { sha256: sha256(bytes) });
  return sha256(bytes);
}

export async function verify({ root, base, exe = DEFAULT_EXE, run = spawnWithDeadline, io = fs }) {
  const { s, corpus, index } = openBase(root, base, io);
  if (!s.exists("bases", base, "effect.first.alo") || !s.exists("bases", base, "effect.first.json")) {
    throw new CanonicaliseError(`bases/${base}: no prepared copy (run --prepare first)`);
  }
  const recorded = s.json("bases", base, "effect.first.json").sha256;
  if (s.hash("bases", base, "effect.first.alo") !== recorded) throw new CanonicaliseError(`bases/${base}: effect.first.alo changed since --prepare`);
  const now = s.hash("bases", base, "effect.alo");
  if (now !== recorded) throw new CanonicaliseError(`bases/${base}: the re-save is not byte-identical (${now.slice(0, 8)} != ${recorded.slice(0, 8)})`);
  const tool = tools({ exe, run, io });
  const first = await tool.inspect(s.at("bases", base, "effect.first.alo"), s.at("bases", base, "effect.first.inspect.json"));
  const second = await tool.inspect(s.at("bases", base, "effect.alo"), s.at("bases", base, "effect.second.inspect.json"));
  for (const name of ["effect.first.inspect.json", "effect.second.inspect.json"]) io.rmSync(s.at("bases", base, name), { force: true });
  if (canonical(first) !== canonical(second)) throw new CanonicaliseError(`bases/${base}: the re-save is not inspect-identical`);
  io.rmSync(s.at("bases", base, "effect.first.alo"));
  io.rmSync(s.at("bases", base, "effect.first.json"));
  corpus.bases[index].canonical = now;
  s.writeJson(["corpus.json"], corpus);
  return now;
}

export async function main(argv = process.argv.slice(2)) {
  const [mode, root, base, ...rest] = argv;
  const exeAt = rest.indexOf("--exe");
  try {
    if (mode === "--prepare" && root && base) console.log(`[bench] prepared ${base}: ${prepare({ root, base }).slice(0, 8)}; re-save effect.alo in place, then --verify`);
    else if (mode === "--verify" && root && base) console.log(`[bench] ${base} is canonical: ${(await verify({ root, base, exe: exeAt >= 0 ? rest[exeAt + 1] : DEFAULT_EXE })).slice(0, 8)}`);
    else { console.error("usage: canonicalise.mjs --prepare <store> <base> | --verify <store> <base> [--exe <path>]"); return 2; }
    return 0;
  } catch (error) {
    console.error(`[bench] ${error instanceof CanonicaliseError ? "refused" : "failed"}: ${error.message}`);
    return error instanceof CanonicaliseError ? 3 : 1;
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = await main();
