import { test } from "node:test";
import assert from "node:assert/strict";
import { acceptScalar, nextFloat32 } from "./persisted.mjs";
import { rulesMarkdown, workedExamples } from "./rules.mjs";

test("every worked example is the scorer's own interval: both ends pass, the values just outside fail", () => {
  for (const { field, contract } of workedExamples()) {
    const [lo, hi] = contract.interval;
    const step = (v, dir) => (field.startsWith("n") ? v + (dir === "increase" ? 1 : -1) : nextFloat32(v, dir));
    assert.equal(acceptScalar(contract, lo).ok, true, `${field} ${lo}`);
    assert.equal(acceptScalar(contract, hi).ok, true, `${field} ${hi}`);
    assert.equal(acceptScalar(contract, step(lo, "decrease")).ok, false, `${field} below ${lo}`);
    assert.equal(acceptScalar(contract, step(hi, "increase")).ok, false, `${field} above ${hi}`);
  }
});

test("rules.md prints the generated examples and is byte-stable", () => {
  const text = rulesMarkdown();
  assert.equal(rulesMarkdown(), text);
  assert.match(text, /fraction of 0\.6 decreased \(seed rule\): from 0\.100000024 to 0\.47500002\./);
  assert.match(text, /fraction of 0\.1 decreased \(seed rule\): from 0 to 0\.075\./);
  assert.match(text, /stated target is 0\.2, decreased \(intervention\): from 0\.1 to 0\.18\./);
  assert.match(text, /no defect/);
});

test("rules.md never uses the benchmark's private vocabulary", () => {
  const text = rulesMarkdown().toLowerCase();
  for (const word of ["planted", "recipe", "truth", "variant", "control", "case id"]) assert.ok(!text.includes(word), word);
});
