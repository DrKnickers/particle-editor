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
//   node scripts/vfx-benchmark/canonicalise.mjs --from <store> <base> <source.alo> [--exe <path>]
//     the scripted alternative: a native save with no net edit (see fromSource).
//
// Saving under the stem "effect" keeps the system name (every save re-stamps
// it from the destination file name); a Save As under another stem changes
// the bytes and is refused here.
import fs from "node:fs";
import { fileURLToPath } from "node:url";
import { spawnWithDeadline } from "../observe.mjs";
import { rolePath } from "./recipe.mjs";
import { canonical } from "./semantic.mjs";
import { DEFAULT_EXE, sha256, store, tools } from "./store.mjs";

class CanonicaliseError extends Error {}

function openBase(root, base, io, { needAlo = true } = {}) {
  const s = store(root, io);
  const corpus = s.json("corpus.json");
  if (corpus.state === "locked") throw new CanonicaliseError("the corpus is locked");
  const index = corpus.bases.findIndex((b) => b.id === base);
  if (index < 0) throw new CanonicaliseError(`no base ${base} in corpus.json`);
  if (needAlo && !s.exists("bases", base, "effect.alo")) throw new CanonicaliseError(`bases/${base}/effect.alo is missing`);
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

// The scripted alternative to the owner's save: a native save through
// --tool patch with no net edit (one field moved and moved back), whose own
// check (3) proves a save of the result reproduces its bytes; --tool validate
// then confirms the file is canonical. The persisted state must equal the
// source's except the derived lifetime group (groups[1]), which the stock
// exporter writes on X and the editor's serialiser derives onto Y: absorbing
// that normalisation once is what a canonical base is for.
export async function fromSource({ root, base, source, exe = DEFAULT_EXE, run = spawnWithDeadline, io = fs }) {
  const { s, corpus, index } = openBase(root, base, io, { needAlo: false });
  if (s.exists("bases", base, "effect.alo")) throw new CanonicaliseError(`bases/${base}/effect.alo already exists`);
  const tool = tools({ exe, run, io });
  const work = (name) => s.at("bases", base, `effect.first.${name}`);
  io.mkdirSync(s.at("bases", base), { recursive: true });
  try {
    const before = await tool.inspect(source, work("source.inspect.json"));
    const at = before.emitters.findIndex((e) => e.linkGroup === 0 && before.emitters.filter((x) => x.name === e.name).length === 1);
    if (at < 0) throw new CanonicaliseError(`${source}: no unlinked, uniquely named emitter to carry the no-op save`);
    const e = before.emitters[at];
    const request = { schema: 1, selector: { sourceDocumentHash: before.sourceDocumentHash, rolePath: rolePath(before, at), expected: { name: e.name } },
      mutations: [{ op: "setProperty", field: "initialDelay", value: e.initialDelay + 0.5 }, { op: "setProperty", field: "initialDelay", value: e.initialDelay }] };
    s.writeJson(["bases", base, "effect.first.request.json"], request);
    await tool.patch(source, work("request.json"), s.at("bases", base, "effect.alo"), work("report.json"));
    const { code, report } = await tool.validate(s.at("bases", base, "effect.alo"), work("validate.json"));
    if (code !== 0 || report.canonical !== true) throw new CanonicaliseError(`bases/${base}: the saved file is not canonical (validate exit ${code})`);
    const after = await tool.inspect(s.at("bases", base, "effect.alo"), work("inspect.json"));
    const strip = (x) => ({ ...x, sourceDocumentHash: undefined, emitters: x.emitters.map((m) => ({ ...m,
      groups: m.groups.map((g, i) => (i === 1 ? { ...g, minX: 0, maxX: 0, minY: 0, maxY: 0 } : g)) })) });
    if (canonical(strip(before)) !== canonical(strip(after))) throw new CanonicaliseError(`bases/${base}: the save changed the persisted state`);
  } catch (error) {
    io.rmSync(s.at("bases", base, "effect.alo"), { force: true });
    throw error;
  } finally {
    for (const name of ["source.inspect.json", "request.json", "report.json", "validate.json", "inspect.json"]) io.rmSync(work(name), { force: true });
  }
  const hash = s.hash("bases", base, "effect.alo");
  corpus.bases[index].canonical = hash;
  s.writeJson(["corpus.json"], corpus);
  return hash;
}

export async function main(argv = process.argv.slice(2)) {
  const [mode, root, base, ...rest] = argv;
  const exeAt = rest.indexOf("--exe");
  try {
    if (mode === "--prepare" && root && base) console.log(`[bench] prepared ${base}: ${prepare({ root, base }).slice(0, 8)}; re-save effect.alo in place, then --verify`);
    else if (mode === "--verify" && root && base) console.log(`[bench] ${base} is canonical: ${(await verify({ root, base, exe: exeAt >= 0 ? rest[exeAt + 1] : DEFAULT_EXE })).slice(0, 8)}`);
    else if (mode === "--from" && root && base && rest[0]) console.log(`[bench] ${base} is canonical: ${(await fromSource({ root, base, source: rest[0], exe: exeAt >= 0 ? rest[exeAt + 1] : DEFAULT_EXE })).slice(0, 8)}`);
    else { console.error("usage: canonicalise.mjs --prepare <store> <base> | --verify <store> <base> | --from <store> <base> <source.alo> [--exe <path>]"); return 2; }
    return 0;
  } catch (error) {
    console.error(`[bench] ${error instanceof CanonicaliseError ? "refused" : "failed"}: ${error.message}`);
    return error instanceof CanonicaliseError ? 3 : 1;
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = await main();
