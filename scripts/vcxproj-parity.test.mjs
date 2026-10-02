// Project-file parity for src/ParticleEditor.vcxproj and its .filters.
//
// The two drifted apart unguarded: ~25 items had no
// filter entry, and ~40 on-disk headers — the header-only policy headers,
// version.h — were missing from the project, so Visual Studio's solution search
// could not see them. These guards keep:
//   1. every C/C++ source and header under src/ listed in the vcxproj,
//   2. every vcxproj source/header present on disk,
//   3. the .filters listing exactly the vcxproj's items, each under a declared filter.
//
// Excluded from (1), deliberately:
//   - src/generated/          build output (EmbeddedWebAssets.{rc,h}, regenerated
//                             by scripts/embed-web-dist.mjs before every compile)
//   - src/host/spike/         standalone spikes with their own .vcxproj files
//   - src/host/third_party/   vendored code (json.hpp is listed, but whatever
//                             else a vendor drop brings is not ours to curate)

import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync, readdirSync } from "node:fs";
import { dirname, join, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const srcDir = resolve(dirname(fileURLToPath(import.meta.url)), "..", "src");
const EXCLUDED_DIRS = ["generated", join("host", "spike"), join("host", "third_party")];
const EXCLUDED_FILES = [];
const ITEM_TYPES = ["ClCompile", "ClInclude", "ResourceCompile", "Image", "FxCompile", "None"];

// Map "Type|path" -> filter name (or null) for every project item in a file.
function items(file) {
  const xml = readFileSync(join(srcDir, file), "utf8");
  const out = new Map();
  const re = new RegExp(`<(${ITEM_TYPES.join("|")}) Include="([^"]+)"\\s*(/>|>([\\s\\S]*?)</\\1>)`, "g");
  for (const m of xml.matchAll(re)) {
    const filter = /<Filter>([^<]+)<\/Filter>/.exec(m[4] ?? "");
    out.set(`${m[1]}|${m[2]}`, filter ? filter[1] : null);
  }
  return out;
}

function sourcesOnDisk(dir = srcDir) {
  const out = [];
  for (const e of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, e.name);
    const rel = relative(srcDir, full);
    if (e.isDirectory()) {
      if (!EXCLUDED_DIRS.includes(rel)) out.push(...sourcesOnDisk(full));
    } else if (/\.(c|cpp|h|hpp)$/i.test(e.name) && !EXCLUDED_FILES.includes(rel)) {
      out.push(rel.split(sep).join("\\"));
    }
  }
  return out;
}

const project = items("ParticleEditor.vcxproj");
const filters = items("ParticleEditor.vcxproj.filters");
const compiled = new Set(
  [...project.keys()].filter((k) => /^Cl(Compile|Include)\|/.test(k)).map((k) => k.split("|")[1]),
);

test("every source and header under src/ is in ParticleEditor.vcxproj", () => {
  const missing = sourcesOnDisk().filter((p) => !compiled.has(p));
  assert.deepEqual(missing, [], "add these as ClCompile (.cpp) / ClInclude (headers) items");
});

test("every vcxproj source and header exists on disk", () => {
  const gone = [...compiled].filter((p) => !existsSync(join(srcDir, ...p.split("\\"))));
  assert.deepEqual(gone, []);
});

test("the .filters file lists exactly the vcxproj's items", () => {
  const notFiltered = [...project.keys()].filter((k) => !filters.has(k));
  const stale = [...filters.keys()].filter((k) => !project.has(k));
  assert.deepEqual(notFiltered, [], "in the vcxproj but missing from the .filters");
  assert.deepEqual(stale, [], "in the .filters but not in the vcxproj");
});

test("every filter an item names is declared in the .filters file", () => {
  const xml = readFileSync(join(srcDir, "ParticleEditor.vcxproj.filters"), "utf8");
  const declared = new Set([...xml.matchAll(/<Filter Include="([^"]+)"/g)].map((m) => m[1]));
  const undeclared = [...filters.entries()]
    .filter(([, f]) => f !== null && !declared.has(f))
    .map(([k, f]) => `${k} -> ${f}`);
  assert.deepEqual(undeclared, []);
});
