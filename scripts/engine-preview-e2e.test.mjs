// Checks prerequisite reporting and source-hash verdicts without a GUI, registry or game.
// Gate integration uses fake probes and never launches the editor.
import assert from "node:assert/strict";
import { test } from "node:test";
import { compareFileHashes, detectPrerequisites } from "./engine-preview-e2e.mjs";
import { prereq, runEnginePreviewE2eLane } from "./run-all-tests.mjs";

/** @type {Parameters<typeof detectPrerequisites>[0]} */
const configuration = {
  platform: "win32", gameDataPath: "game/corruption", editorExePath: "tools/EAW Terrain Editor.exe",
  releaseExe: "x64/Release/ParticleEditor.exe",
};

test("prerequisite result names missing game, editor and desktop with actionable hints", () => {
  assert.deepEqual(detectPrerequisites(configuration, { exists: () => true, desktop: () => true }), { ok: true, missing: [] });
  /** @type {ReturnType<typeof detectPrerequisites>} */
  const result = detectPrerequisites(configuration, {
    exists: (path) => path !== configuration.gameDataPath && path !== configuration.editorExePath,
    desktop: () => false,
  });
  assert.deepEqual(result, {
    ok: false,
    missing: [
      { what: "Windows GUI desktop unavailable", hint: "run on an unlocked interactive Windows desktop" },
      { what: "configured game install missing", hint: "choose the installed game data folder in Particle Editor (HKCU GameDataPath)" },
      { what: "EAW Terrain Editor executable missing", hint: "set PE_ENGINE_EDITOR_EXE to the supported source EAW Terrain Editor.exe" },
    ],
  });
});

test("missing configuration and non-Windows hosts cannot pass prerequisites", () => {
  /** @type {ReturnType<typeof detectPrerequisites>} */
  const result = detectPrerequisites({ ...configuration, platform: "linux", gameDataPath: "", editorExePath: "" }, {
    exists: () => true,
    desktop: () => assert.fail("do not probe Windows APIs on another platform"),
  });
  const expectedMissingCount = 3;
  assert.equal(result.missing.length, expectedMissingCount);
  assert.equal(result.ok, false);
});

test("hash verdict detects edits, additions and removals, independent of key order", () => {
  assert.deepEqual(compareFileHashes({ a: "same", b: "same" }, { b: "same", a: "same" }), { ok: true, changed: [] });
  assert.deepEqual(compareFileHashes({ edited: "old", removed: "old", kept: "same" },
    { edited: "new", added: "new", kept: "same" }), { ok: false, changed: ["added", "edited", "removed"] });
});

test("gate fails missing prerequisites by default and visibly skips only when allowed", () => {
  for (const allowMissing of [false, true]) {
    /** @type {ReturnType<typeof runEnginePreviewE2eLane>} */
    const result = runEnginePreviewE2eLane({
      probe: () => ({ ok: false, missing: [{ what: "game install missing", hint: "configure GameDataPath" }] }),
      checkPrereq: (lane, ok, what, hint) => prereq(lane, ok, what, hint, new Set(allowMissing ? [lane] : [])),
      runLane: () => assert.fail("a missing prerequisite must prevent launch"),
    });
    assert.equal(result.status, allowMissing ? "SKIP" : "FAIL");
    assert.match(result.note, /game install missing/);
  }
});

test("gate reports runtime failures even with prerequisites present", () => {
  for (const code of [0, 1]) {
    assert.equal(runEnginePreviewE2eLane({ probe: () => ({ ok: true, missing: [] }), runLane: () => code }).status,
      code === 0 ? "PASS" : "FAIL");
  }
});
