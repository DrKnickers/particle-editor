import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { fileURLToPath } from "node:url";

import {
  GOLDEN_PROFILE_ARGS,
  GOLDEN_PROFILE_ATTESTATIONS,
  SCENES,
  buildGoldenCaptureArgs,
  buildGoldenCaptureEnv,
  missingGoldenProfileAttestations,
} from "./render-goldens.mjs";

test("golden capture env removes every inherited ALO hook", () => {
  const inherited = {
    PATH: "kept",
    ALO_LT7_TEST_OBJECT: "non-empty-object",
    alo_capture_subviewport: "non-empty-subviewport",
    AlO_PARTICLE_MIPFILTER: "LINEAR",
    ALO_SHADER_DIAG: "caller-controlled",
  };

  const env = buildGoldenCaptureEnv(inherited, false);

  assert.deepEqual(env, { PATH: "kept" });
  assert.equal(inherited.ALO_LT7_TEST_OBJECT, "non-empty-object");
  assert.equal(inherited.ALO_SHADER_DIAG, "caller-controlled");
});

test("texture-gated capture adds only the controlled shader diagnostic hook", () => {
  const env = buildGoldenCaptureEnv({
    PATH: "kept",
    ALO_SHADER_DIAG: "caller-controlled",
    ALO_CAPTURE_CAM_DIST_MULT: "9",
    alo_shadow_force: "1",
  }, true);

  assert.equal(env.PATH, "kept");
  assert.equal(env.ALO_SHADER_DIAG, "1");
  assert.deepEqual(
    Object.keys(env).filter((key) => /^ALO_/i.test(key)),
    ["ALO_SHADER_DIAG"],
  );
});

test("every golden scene refuses to bless missing particle textures", () => {
  const a11y = SCENES.find((scene) => scene.name === "a11y-base-state");
  const singleton = SCENES.find((scene) => scene.name === "nt-5-singleton");
  const bump = SCENES.find((scene) => scene.name === "bump-cutout");

  assert.deepEqual(
    a11y?.requireTexGate,
    ["P_PARTICLE_MASTER.TGA", "P_PARTICLE_DEPTH_MASTER.TGA"],
  );
  assert.deepEqual(singleton?.requireTexGate, a11y?.requireTexGate);
  assert.deepEqual(
    bump?.requireTexGate,
    [
      "TESTS\\FIXTURES\\BUMPTEST\\ZZ_BUMPGOLD_ROCK.TGA",
      "TESTS\\FIXTURES\\BUMPTEST\\ZZ_BUMPTEST_NM.TGA",
    ],
  );
});

test("golden capture argv always selects the isolated profile and skydome one", () => {
  assert.deepEqual(GOLDEN_PROFILE_ARGS, ["--golden-profile", "--skydome", "1"]);
  assert.deepEqual(
    buildGoldenCaptureArgs("fixture.alo", "capture.png", ["--frames", "45"]),
    [
      "--capture", "fixture.alo", "capture.png",
      "--golden-profile", "--skydome", "1",
      "--frames", "45",
    ],
  );
});

test("attestation check requires both exact stdout lines", () => {
  const complete = `noise\r\n${GOLDEN_PROFILE_ATTESTATIONS[0]}\r\n${GOLDEN_PROFILE_ATTESTATIONS[1]}\r\n`;
  assert.deepEqual(missingGoldenProfileAttestations(complete), []);

  assert.deepEqual(
    missingGoldenProfileAttestations(`${GOLDEN_PROFILE_ATTESTATIONS[0]}\n`),
    [GOLDEN_PROFILE_ATTESTATIONS[1]],
  );
  assert.deepEqual(
    missingGoldenProfileAttestations(`prefix ${GOLDEN_PROFILE_ATTESTATIONS[0]}\n${GOLDEN_PROFILE_ATTESTATIONS[1]}`),
    [GOLDEN_PROFILE_ATTESTATIONS[0]],
  );
});

test("production runner binds argv and attestations before texture, compare, or update", () => {
  const source = readFileSync(fileURLToPath(new URL("./render-goldens.mjs", import.meta.url)), "utf8");
  const envAt = source.indexOf("buildGoldenCaptureEnv(process.env, Boolean(scene.requireTexGate))");
  const spawnAt = source.indexOf("buildGoldenCaptureArgs(scene.fixture, out, scene.args)");
  const attestationAt = source.indexOf("missingGoldenProfileAttestations(cap.stdout)", spawnAt);
  const textureAt = source.indexOf("const unresolved =", spawnAt);
  const updateAt = source.indexOf("if (UPDATE)", spawnAt);
  const compareAt = source.indexOf("const s = ssim(golden, out)", spawnAt);

  assert.ok(envAt >= 0, "production spawn must use the sanitized golden environment");
  assert.ok(spawnAt >= 0, "production spawn must use buildGoldenCaptureArgs");
  assert.ok(spawnAt > envAt, "sanitized environment must be prepared before capture");
  assert.ok(attestationAt > spawnAt, "production runner must validate capture stdout");
  assert.ok(textureAt > attestationAt, "attestations must precede texture checks");
  assert.ok(updateAt > attestationAt, "attestations must precede --update copy");
  assert.ok(compareAt > attestationAt, "attestations must precede comparison");
  assert.match(
    source,
    /if \(process\.argv\[1\] && import\.meta\.url === pathToFileURL\(process\.argv\[1\]\)\.href\)/,
  );
});
