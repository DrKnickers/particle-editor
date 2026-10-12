// check-corpus (plan 3.4): re-derives every hash of a locked corpus and
// refuses on any mismatch. Runs before every benchmark run.
//
//   node scripts/vfx-benchmark/check-corpus.mjs <store>
import fs from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { checkCorpus } from "./corpus.mjs";
import { allowlist, verifyPack } from "./pack.mjs";
import { rulesMarkdown } from "./rules.mjs";
import { THREE } from "./context.mjs";
import { pinFiles, serialise, sha256, store } from "./store.mjs";

const here = dirname(fileURLToPath(import.meta.url));

// Links and junctions anywhere in the store: a store carries no registered mount.
export function reparsePoints(root, io = fs) {
  const found = [];
  const walk = (abs, rel) => {
    for (const entry of io.readdirSync(abs, { withFileTypes: true })) {
      const path = join(abs, entry.name), r = rel ? `${rel}/${entry.name}` : entry.name;
      if (io.lstatSync(path).isSymbolicLink()) { found.push(r); continue; }
      if (entry.isDirectory()) walk(path, r);
    }
  };
  walk(root, "");
  return found;
}

function packExpected(dir, inspect, io) {
  const three = THREE.every((l) => io.existsSync(join(dir, l, "selected.json")));
  const bundles = three ? THREE : ["bundle"];
  const frames = {};
  for (const b of bundles) frames[b] = JSON.parse(io.readFileSync(join(dir, three ? b : "", "selected.json"), "utf8")).tiles.map((t) => t.frame);
  return allowlist({ bundles, frames, inspect });
}

export function checkStore(root, { io = fs, requireLocked = true, streams = () => [] } = {}) {
  const s = store(root, io);
  const errors = [];
  const corpus = s.json("corpus.json");
  if (requireLocked && corpus.state !== "locked") errors.push("the corpus is not locked");
  errors.push(...checkCorpus(corpus));
  for (const r of reparsePoints(root, io)) errors.push(`reparse point in the store: ${r}`);

  const hashes = {
    rubric: sha256(fs.readFileSync(join(here, "rubric.json"))), prompt: sha256(fs.readFileSync(join(here, "prompt.md"))),
    schema: sha256(fs.readFileSync(join(here, "findings.schema.json"))), rules: sha256(rulesMarkdown()),
  };
  for (const [k, v] of Object.entries(hashes)) if (corpus.hashes?.[k] !== v) errors.push(`${k} hash differs from the one recorded at lock`);
  if (corpus.state === "locked") {
    const truths = Object.fromEntries(corpus.cases.map((c) => [c.id, s.exists("cases", c.id, "truth.json") ? s.json("cases", c.id, "truth.json") : null]).sort(([a], [b]) => (a < b ? -1 : 1)));
    if (sha256(serialise(truths)) !== corpus.hashes.truth) errors.push("truth hash differs from the one recorded at lock");
  }

  const pins = pinFiles(root, io), recorded = corpus.pins ?? {};
  for (const [path, hash] of Object.entries(recorded)) {
    if (!(path in pins)) errors.push(`${path} is missing`);
    else if (pins[path] !== hash) errors.push(`${path} changed since it was pinned`);
  }
  for (const path of Object.keys(pins)) if (!(path in recorded)) errors.push(`${path} is not pinned`);

  for (const c of corpus.cases) {
    for (const [folder, inspect] of [["packs", true], ["packs-bundle-only", false]]) {
      if (!s.exists(folder, `${c.id}.manifest.json`)) continue;
      const dir = s.at(folder, c.id);
      for (const e of verifyPack(dir, s.json(folder, `${c.id}.manifest.json`), packExpected(dir, inspect, io), { io, streams })) errors.push(`${folder}/${c.id}: ${e}`);
    }
  }
  return errors;
}

export function main(argv = process.argv.slice(2)) {
  if (argv.length !== 1) { console.error("usage: check-corpus.mjs <store>"); return 2; }
  const errors = checkStore(argv[0]);
  for (const e of errors) console.error(`[bench] check-corpus: ${e}`);
  if (!errors.length) console.log("[bench] check-corpus: ok");
  return errors.length ? 3 : 0;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) process.exitCode = main();
