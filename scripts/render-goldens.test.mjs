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
  parseGoldenProvenance,
  provenanceDrift,
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

test("attestation check requires every exact stdout line", () => {
  const complete = `noise\r\n${GOLDEN_PROFILE_ATTESTATIONS.join("\r\n")}\r\n`;
  assert.deepEqual(missingGoldenProfileAttestations(complete), []);

  // Each attestation is individually load-bearing: dropping any one has to be
  // reported, or a capture could skip that isolation step and still be blessed.
  for (const attestation of GOLDEN_PROFILE_ATTESTATIONS) {
    assert.deepEqual(
      missingGoldenProfileAttestations(
        GOLDEN_PROFILE_ATTESTATIONS.filter((a) => a !== attestation).join("\n"),
      ),
      [attestation],
    );
  }
  // Exact full-line match, not substring: a prefixed line is not the line.
  assert.deepEqual(
    missingGoldenProfileAttestations(
      [`prefix ${GOLDEN_PROFILE_ATTESTATIONS[0]}`, ...GOLDEN_PROFILE_ATTESTATIONS.slice(1)].join("\n"),
    ),
    [GOLDEN_PROFILE_ATTESTATIONS[0]],
  );
});

test("the mod-layer attestation asserts an empty stack, not merely a skipped restore", () => {
  const modLayer = GOLDEN_PROFILE_ATTESTATIONS.find((a) => a.includes("persisted-mod-layer-restore"));
  assert.ok(modLayer, "golden profile must attest the persisted mod-layer restore was skipped");
  assert.match(modLayer, /layers=0$/);

  // The host prints the LIVE count. If the restore gate regresses it prints
  // layers=1, and that must not satisfy the gate.
  assert.deepEqual(
    missingGoldenProfileAttestations(
      GOLDEN_PROFILE_ATTESTATIONS.join("\n").replace("layers=0", "layers=1"),
    ),
    [modLayer],
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

test("provenance parses the adapter line and tolerates CRLF", () => {
  const line = "[capture-profile] golden adapter=NVIDIA GeForce RTX 3080 vendor=0x10DE device=0x2206";
  assert.deepEqual(parseGoldenProvenance(`noise\r\n${line}\r\n`), {
    adapter: "NVIDIA GeForce RTX 3080",
    vendorId: "0x10DE",
    deviceId: "0x2206",
  });
  assert.equal(parseGoldenProvenance("nothing here"), null);
  assert.equal(parseGoldenProvenance(`prefix ${line}`), null);
});

test("provenance drift reports differences and stays silent when they agree", () => {
  const recorded = { adapter: "GPU A", vendorId: "0x10DE", deviceId: "0x2206" };
  assert.equal(provenanceDrift(recorded, { ...recorded }), null);
  assert.equal(provenanceDrift(null, recorded), null);
  assert.equal(provenanceDrift(recorded, null), null);
  assert.match(
    provenanceDrift(recorded, { ...recorded, adapter: "GPU B" }),
    /adapter: golden=GPU A current=GPU B/,
  );
  assert.match(provenanceDrift(recorded, { ...recorded, deviceId: "0x1111" }), /deviceId/);
});

test("production records provenance on bless and never fails the lane on drift", () => {
  const source = readFileSync(fileURLToPath(new URL("./render-goldens.mjs", import.meta.url)), "utf8");
  const attestationAt = source.indexOf("missingGoldenProfileAttestations(cap.stdout)");
  const parseAt = source.indexOf("parseGoldenProvenance(cap.stdout)");
  const updateAt = source.indexOf("if (UPDATE)", parseAt);
  const writeAt = source.indexOf("writeFileSync(sidecar", parseAt);
  const driftAt = source.indexOf("provenanceDrift(recorded, provenance)", parseAt);
  const compareAt = source.indexOf("const s = ssim(golden, out)", driftAt);

  assert.ok(parseAt >= 0, "production must parse provenance from the capture's own stdout");
  assert.ok(parseAt > attestationAt, "attestations must still gate before provenance is recorded");
  assert.ok(writeAt > updateAt, "the sidecar may only be written inside the --update branch");
  // An absolute exe path would bake the blessing checkout's location into the
  // repo and churn on every other machine.
  assert.match(
    source.slice(updateAt, driftAt > updateAt ? driftAt : source.length),
    /relative\(repoRoot, exe\)/,
    "the recorded exe path must be repo-relative",
  );
  assert.ok(driftAt > parseAt, "production must compare recorded provenance against the live run");

  // Drift warns and nothing more. Failing the lane for a legitimate GPU swap
  // would train people to bless past the warning — the exact habit this
  // profile exists to prevent.
  assert.ok(compareAt > driftAt, "drift check must precede comparison");
  assert.ok(
    !/failed\+\+/.test(source.slice(driftAt, compareAt)),
    "provenance drift must never increment the failure counter",
  );
});
