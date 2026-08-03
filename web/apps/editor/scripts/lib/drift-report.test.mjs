import { test } from "node:test";
import assert from "node:assert/strict";
import { parseChangedGoldens, stableDrift, verdict } from "./drift-report.mjs";

const G = "web/apps/editor/tests/a11y-goldens/";

test("parseChangedGoldens keeps only goldens paths, stripped of the porcelain status prefix", () => {
  const porcelain = [
    ` M ${G}toolbar.golden.json`,
    `?? ${G}new-surface.composition.golden.yaml`,
    " M web/apps/editor/src/components/Toolbar.tsx",
    "",
  ].join("\n");
  assert.deepEqual(parseChangedGoldens(porcelain), [
    `${G}new-surface.composition.golden.yaml`,
    `${G}toolbar.golden.json`,
  ]);
});

test("parseChangedGoldens tolerates CRLF line endings and blank input", () => {
  assert.deepEqual(parseChangedGoldens(` M ${G}x.golden.json\r\n\r\n`), [`${G}x.golden.json`]);
  assert.deepEqual(parseChangedGoldens(""), []);
});

test("stableDrift splits changed-in-both (stable) from changed-in-one (noisy)", () => {
  const r = stableDrift([`${G}a.golden.json`, `${G}b.golden.json`], [`${G}a.golden.json`]);
  assert.deepEqual(r.stable, [`${G}a.golden.json`]);
  assert.deepEqual(r.noisy, [`${G}b.golden.json`]);
});

test("verdict: stable drift -> exit 2", () => {
  const v = verdict({ stable: [`${G}a.golden.json`], noisy: [] });
  assert.equal(v.kind, "drift");
  assert.equal(v.exitCode, 2);
  assert.match(v.message, /A11Y-DRIFT: 1 file/);
});

test("verdict: only noise -> exit 0, normalizer-gap message", () => {
  const v = verdict({ stable: [], noisy: [`${G}a.golden.json`] });
  assert.equal(v.kind, "noise");
  assert.equal(v.exitCode, 0);
  assert.match(v.message, /nondeterministic/);
});

test("verdict: nothing changed -> exit 0, none", () => {
  const v = verdict({ stable: [], noisy: [] });
  assert.equal(v.kind, "none");
  assert.equal(v.exitCode, 0);
  assert.equal(v.message, "A11Y-DRIFT: none");
});

test("stableDrift: identical passes -> all stable, none noisy", () => {
  const r = stableDrift([`${G}a.golden.json`, `${G}b.golden.json`], [`${G}a.golden.json`, `${G}b.golden.json`]);
  assert.deepEqual(r.stable, [`${G}a.golden.json`, `${G}b.golden.json`]);
  assert.deepEqual(r.noisy, []);
});

test("verdict: stable + noisy -> drift (exit 2) with the noisy files noted in the message", () => {
  const v = verdict({ stable: [`${G}a.golden.json`], noisy: [`${G}b.golden.json`] });
  assert.equal(v.kind, "drift");
  assert.equal(v.exitCode, 2);
  assert.match(v.message, /also nondeterministic/);
  assert.ok(v.message.includes(`${G}b.golden.json`));
});
