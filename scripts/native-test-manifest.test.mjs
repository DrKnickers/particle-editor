// tests/native-tests.json must agree with the tree: every tests/test_*.cpp has
// a build entry (an unbuildable assertion is a silent coverage hole), every
// tests/tools/*.cpp has one, nothing else sits loose in tests/, and every listed
// source exists. The cpp-unit lane refuses to start on the same check; this
// catches it in the scripts lane, without a compiler.

import { test } from "node:test";
import assert from "node:assert/strict";

import { loadManifest, targetsOf, validateManifest } from "../tests/build-native.mjs";

test("tests/native-tests.json matches tests/ and tests/tools/", () => {
  assert.deepEqual(validateManifest(), []);
});

test("the fuzz / malformed-input tests keep their Release pass", () => {
  const targets = targetsOf(loadManifest());
  for (const name of ["test_alo_fuzz", "test_meg_fuzz", "test_xml_billion_laughs", "test_alo_roundtrip"]) {
    assert.equal(targets.get(name)?.release, true, `${name} must build and run /O2 /DNDEBUG too`);
  }
});

test("validateManifest reports a test with no entry and a missing source", () => {
  const manifest = loadManifest();
  delete manifest.tests.test_alo_fuzz;
  manifest.tests.test_meg_fuzz = { ...manifest.tests.test_meg_fuzz, sources: ["src/NoSuchFile.cpp"] };
  const problems = validateManifest(manifest).join("\n");
  assert.match(problems, /test_alo_fuzz\.cpp has no entry/);
  assert.match(problems, /src\/NoSuchFile\.cpp does not exist/);
});
