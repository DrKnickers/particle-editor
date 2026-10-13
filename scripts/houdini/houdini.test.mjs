// [houdini] The child environment is exactly the allowlist, and the binaries
// are the pinned constants (plan §5 "Spawn environment").
import { test } from "node:test";
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, win32 } from "node:path";
import { HFS, HOUDINI_BUILD, PINNED, THREADS, childEnvironment, verifyPinned } from "./houdini.mjs";
import { sha256 } from "./cache.mjs";

const STAGE = "C:\\gen\\.staging\\k-1\\work";
// Everything a real shell carries that must not reach Houdini.
const AMBIENT = {
  SystemRoot: "C:\\Windows", SystemDrive: "C:", NUMBER_OF_PROCESSORS: "20",
  USERPROFILE: "C:\\Users\\me", USERNAME: "me", COMPUTERNAME: "BOX", APPDATA: "C:\\Users\\me\\AppData\\Roaming",
  HSITE: "C:\\site", HOUDINI_PATH: "C:\\evil", HOUDINI_USER_PREF_DIR: "C:\\Users\\me\\OneDrive\\houdini19.5",
  HOUDINI_DSO_PATH: "C:\\dso", PYTHONPATH: "C:\\py", PYTHONHOME: "C:\\pyhome", OCIO: "C:\\ocio.config", JOB: "C:\\job",
  PATH: "C:\\Windows\\System32;C:\\Python313;C:\\Users\\me\\bin", Path: "C:\\x", TEMP: "C:\\Users\\me\\Temp",
};

test("the child environment is exactly the allowlist", () => {
  const env = childEnvironment(STAGE, { inherit: AMBIENT });
  assert.deepEqual(env, {
    SystemRoot: "C:\\Windows", SystemDrive: "C:", NUMBER_OF_PROCESSORS: "20",
    TEMP: STAGE, TMP: STAGE, HOUDINI_TEMP_DIR: STAGE, HOME: STAGE,
    PATH: `C:\\Windows\\System32;${HFS}\\bin`,
    HOUDINI_USER_PREF_DIR: `${STAGE}\\prefs\\houdini__HVER__`,
    HOUDINI_NO_ENV_FILE: "1", HOUDINI_PACKAGE_SKIP: "1", HOUDINI_MAXTHREADS: String(THREADS),
    PYTHONNOUSERSITE: "1", PYTHONDONTWRITEBYTECODE: "1",
  });
  // Houdini ignores a prefs folder without __HVER__ and falls back to the owner's.
  assert.ok(env.HOUDINI_USER_PREF_DIR.startsWith(`${STAGE}\\`) && env.HOUDINI_USER_PREF_DIR.includes("__HVER__"));
  assert.throws(() => childEnvironment(STAGE, { inherit: {} }), /SystemRoot/);
});

test("the binaries are constants under the pinned build", () => {
  assert.equal(HOUDINI_BUILD, "19.5.435");
  assert.ok(HFS.endsWith(`Houdini ${HOUDINI_BUILD}`));
  for (const [name, pin] of Object.entries(PINNED)) {
    assert.equal(win32.dirname(pin.path), win32.join(HFS, "bin"), name);
    assert.match(pin.sha256, /^[0-9a-f]{64}$/, name);
  }
});

test("verifyPinned refuses a missing or altered binary", (t) => {
  const dir = mkdtempSync(join(tmpdir(), "pe-houdini-pins-"));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const path = join(dir, "hython.exe");
  writeFileSync(path, "pinned bytes");
  const pins = { hython: { path, sha256: sha256(Buffer.from("pinned bytes")) } };
  verifyPinned({ pins });
  writeFileSync(path, "swapped bytes");
  assert.throws(() => verifyPinned({ pins }), /differs from the pinned/);
  assert.throws(() => verifyPinned({ pins: { hython: { path: join(dir, "none.exe"), sha256: pins.hython.sha256 } } }), /missing/);
});
