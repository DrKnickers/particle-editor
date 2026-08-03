// production-inventory.md is a hand-maintained mirror of manifest.json (its recording queue),
// so its Output column can silently drift from the real manifest outputs. This guard fails if
// any inventory row's `Output` cell (or the "Release tag" line) disagrees with the manifest —
// keeping the human doc honest without a generator step.
import { test } from "node:test";
import assert from "node:assert/strict";
import { existsSync, readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const wikiMedia = join(__dirname, "..", "..", "..", "tasks", "wiki-media");

// This guard mirrors two PRIVATE files (tasks/wiki-media) against each other;
// the public mirror ships neither. Skip visibly there — budgeted by
// run-all-tests.mjs SKIP_BUDGET only when the data is absent.
const needsClipData = {
  skip: existsSync(join(wikiMedia, "manifest.json"))
    ? false
    : "maintainer-only clip-source data (tasks/wiki-media) not in this checkout",
};

test("production-inventory Output cells and release tag match the manifest", needsClipData, () => {
  const manifest = JSON.parse(readFileSync(join(wikiMedia, "manifest.json"), "utf8"));
  const outputById = new Map(manifest.items.map((it) => [it.id, it.output]));
  const md = readFileSync(join(wikiMedia, "production-inventory.md"), "utf8");

  const mismatches = [];
  let rows = 0;
  for (const line of md.split(/\r?\n/)) {
    // Table row: | `id` | kind | `output` | focus | purpose |
    const m = line.match(/^\|\s*`([\w-]+)`\s*\|\s*[^|]+?\s*\|\s*`([^`]+)`\s*\|/);
    if (!m) continue;
    const [, id, output] = m;
    if (!outputById.has(id)) { mismatches.push(`row "${id}" is not in the manifest`); continue; }
    rows++;
    if (output !== outputById.get(id))
      mismatches.push(`row "${id}": inventory "${output}" != manifest "${outputById.get(id)}"`);
  }

  assert.equal(rows, manifest.items.length, `expected one inventory row per manifest item (${manifest.items.length})`);

  const tagLine = md.match(/^- Release tag: `([^`]+)`/m);
  assert.ok(tagLine, "production-inventory must state the release tag");
  if (tagLine[1] !== manifest.releaseTag)
    mismatches.push(`release tag: inventory "${tagLine[1]}" != manifest "${manifest.releaseTag}"`);

  assert.deepEqual(mismatches, [], "production-inventory drifted from manifest.json:\n  " + mismatches.join("\n  "));
});
