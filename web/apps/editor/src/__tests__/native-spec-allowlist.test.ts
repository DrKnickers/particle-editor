// Native-spec allowlist guard.
//
// scripts/run-native-tests.mjs hands Playwright an explicit list of
// spec files instead of globbing tests/. The CHANGELOG records this as
// a known footgun: a new tests/*.spec.ts file is silently skipped by
// CI until someone remembers to append it to the harness array.
//
// This guard diffs the spec files on disk against the harness allowlist
// and fails if any spec is missing without an explicit waiver in
// INTENTIONALLY_EXCLUDED below. To add a new native spec: add it both
// to the harness array AND to tests/. To intentionally keep a spec out
// of the harness: add the basename + a reason to INTENTIONALLY_EXCLUDED.
import { describe, it, expect } from "vitest";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";

const __dirname = dirname(fileURLToPath(import.meta.url));
const TESTS_DIR = resolve(__dirname, "../../tests");
const HARNESS_PATH = resolve(__dirname, "../../scripts/run-native-tests.mjs");

// Specs deliberately kept out of the native harness. Keys are spec
// basenames (e.g. "foo.spec.ts"); values document why. Empty by
// design: the first run of this guard surfaces every unwaived spec.
const INTENTIONALLY_EXCLUDED: Record<string, string> = {
  // Example:
  // "legacy-thing.spec.ts": "Pinned for manual debugging, see ROADMAP NT-X",
};

function specsOnDisk(): string[] {
  return readdirSync(TESTS_DIR)
    .filter((name) => name.endsWith(".spec.ts"))
    .sort();
}

function specsInHarness(): string[] {
  const src = readFileSync(HARNESS_PATH, "utf8");
  // Match "tests/<name>.spec.ts" string literals. The harness writes
  // these as single string args to playwright; we don't try to parse
  // JS — a regex over the file is enough and robust to formatting.
  const matches = src.matchAll(/["']tests\/([\w.-]+\.spec\.ts)["']/g);
  return [...new Set([...matches].map((m) => m[1]))].sort();
}

describe("native-test harness allowlist", () => {
  it("includes every tests/*.spec.ts that isn't explicitly excluded", () => {
    const disk = specsOnDisk();
    const harness = new Set(specsInHarness());
    const excluded = new Set(Object.keys(INTENTIONALLY_EXCLUDED));

    const missing = disk.filter(
      (name) => !harness.has(name) && !excluded.has(name),
    );

    expect(
      missing,
      `These specs exist under web/apps/editor/tests/ but are not in ` +
        `scripts/run-native-tests.mjs. Either add them to the harness ` +
        `array OR add them to INTENTIONALLY_EXCLUDED with a reason:\n` +
        missing.map((n) => `  - ${n}`).join("\n"),
    ).toEqual([]);
  });

  it("does not allowlist specs that no longer exist on disk", () => {
    const disk = new Set(specsOnDisk());
    const stale = specsInHarness().filter((name) => !disk.has(name));

    expect(
      stale,
      `These specs are listed in scripts/run-native-tests.mjs but no ` +
        `longer exist under web/apps/editor/tests/. Remove them from ` +
        `the harness array:\n` +
        stale.map((n) => `  - ${n}`).join("\n"),
    ).toEqual([]);
  });

  it("does not waive specs that aren't on disk", () => {
    const disk = new Set(specsOnDisk());
    const phantoms = Object.keys(INTENTIONALLY_EXCLUDED).filter(
      (name) => !disk.has(name),
    );

    expect(
      phantoms,
      `INTENTIONALLY_EXCLUDED references spec files that don't exist. ` +
        `Remove the stale entries:\n` +
        phantoms.map((n) => `  - ${n}`).join("\n"),
    ).toEqual([]);
  });
});
