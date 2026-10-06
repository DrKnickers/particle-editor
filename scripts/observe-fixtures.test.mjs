import { test } from "node:test";
import assert from "node:assert/strict";
import { OBSERVE_FIXTURE_KINDS, validateObserveFixture } from "./observe-fixtures.mjs";

function emitter(name, nParticlesPerBurst, lifetime, extra = {}) {
  return {
    name, parent: -1, spawnDuringLife: -1, spawnOnDeath: -1,
    colorTexture: "tests\\fixtures\\bumptest\\zz_bumpgold_rock.tga",
    normalTexture: "tests\\fixtures\\bumptest\\zz_bumptest_nm.tga",
    blendMode: 11, textureSize: 1, useBursts: true, nBursts: 1,
    nParticlesPerBurst, nParticlesPerSecond: 0, lifetime,
    initialDelay: 0, skipTime: 0, burstDelay: 10000, gravity: 0, freezeTime: 0,
    randomLifetimePerc: 0, randomScalePerc: 0, isWeatherParticle: false,
    positionGroup: 1, positionMin: [-8, -8, -8], positionMax: [8, 8, 8],
    speedGroup: 0, speed: [0, 0, 0], scale: 8, red: 1, green: 1, blue: 1, alpha: 1,
    ...extra,
  };
}

const SOFT = "tests\\fixtures\\observe\\zz_observe_soft.tga";

const fixtures = {
  "late-flash": [
    emitter("core", 64, 1, { colorTexture: SOFT, blendMode: 2,
      positionMin: [-40, -20, -40], positionMax: [40, 20, 40],
      speed: [0, 0, 1], scale: 24, red: 0.49803921580314636, green: 0.49803921580314636,
      blue: 0.49803921580314636, alpha: 0.24705882370471954 }),
    emitter("flash", 8, 0.5, { colorTexture: SOFT, blendMode: 1, initialDelay: 0.75, scale: 40,
      green: 0.49803921580314636, blue: 0.12156862765550613 }),
  ],
  "guard-short": [emitter("burst", 2048, 0.5)],
  "death-child": [emitter("parent", 8, 0.25, { spawnOnDeath: 1 }), emitter("death-child", 1, 0.75, { parent: 0 })],
  "child-starve": [
    emitter("parent", 200, 0.5, { spawnDuringLife: 1, spawnOnDeath: 2 }),
    emitter("lifetime-child", 1, 0.25, { parent: 0, useBursts: false, nParticlesPerSecond: 60 }),
    emitter("death-child", 1, 0.75, { parent: 0 }),
  ],
  "budget-bailout": [
    emitter("budget-burst", 1000, 0.5, { initialDelay: 0.0078125 }),
    emitter("catch-up", 1, 0.5, { nBursts: 30, burstDelay: Math.fround(0.01), initialDelay: 0.0078125 }),
  ],
  "budget-bailout-skip": [
    emitter("budget-burst", 1000, 0.5, { initialDelay: 0.0078125 }),
    emitter("catch-up", 1, 0.5, { nBursts: 30, burstDelay: Math.fround(0.01), initialDelay: 0.0078125, skipTime: 0.25 }),
  ],
  "index-cap": [emitter("burst", 20000, 0.5)],
};

function attestation(kind) {
  const emitters = structuredClone(fixtures[kind]);
  return { kind, reloaded: true, emitterCount: emitters.length, rootCount: emitters.filter((e) => e.parent === -1).length, emitters };
}

test("observation kinds include the skip-time variant and exclude existing proof kinds", () => {
  assert.deepEqual(OBSERVE_FIXTURE_KINDS, Object.keys(fixtures));
});

for (const kind of OBSERVE_FIXTURE_KINDS) {
  test(`${kind}: a valid reload line satisfies the contract`, () => {
    // Match the actual stdout transport: JSON follows the replay-fixture prefix.
    const line = `[replay-fixture] ${JSON.stringify(attestation(kind))}`;
    const fields = JSON.parse(line.slice("[replay-fixture] ".length));
    assert.deepEqual(validateObserveFixture(fields, kind), []);
  });

  test(`${kind}: every missing or changed attested property is rejected`, () => {
    const valid = attestation(kind);
    for (const key of ["kind", "reloaded", "emitterCount", "rootCount", "emitters"]) {
      const fields = structuredClone(valid);
      delete fields[key];
      assert.ok(validateObserveFixture(fields, kind).length > 0, key);
    }
    valid.emitters.forEach((e, index) => {
      for (const [key, value] of Object.entries(e)) {
        const fields = structuredClone(valid);
        delete fields.emitters[index][key];
        assert.ok(validateObserveFixture(fields, kind).some((p) => p.includes(`emitters[${index}].${key}`)), key);
        fields.emitters[index][key] = Array.isArray(value) ? [...value, 99] : typeof value === "number" ? value + 1 : typeof value === "boolean" ? !value : `${value}-wrong`;
        assert.ok(validateObserveFixture(fields, kind).length > 0, key);
        if (Array.isArray(value)) {
          fields.emitters[index][key] = [...value];
          fields.emitters[index][key][0] += 1;
          assert.ok(validateObserveFixture(fields, kind).length > 0, `${key} contents`);
        }
      }
    });
  });

  test(`${kind}: malformed or extra emitters cannot pass`, () => {
    for (const fields of [null, {}, { emitters: {} }])
      assert.ok(validateObserveFixture(fields, kind).length > 0);
    const fields = attestation(kind);
    fields.emitters.push(structuredClone(fields.emitters[0]));
    assert.ok(validateObserveFixture(fields, kind).some((p) => p.includes("emitters.length")));
    fields.emitters = [];
    assert.ok(validateObserveFixture(fields, kind).length > 0);
  });
}

test("child-starve: parser-default missing death link is rejected alongside the lifetime link", () => {
  const fields = attestation("child-starve");
  fields.emitters[0].spawnOnDeath = -1;
  assert.match(validateObserveFixture(fields, "child-starve").join("\n"), /spawnOnDeath/);
  delete fields.emitters[0].spawnOnDeath;
  assert.match(validateObserveFixture(fields, "child-starve").join("\n"), /spawnOnDeath/);
});

test("unknown kinds fail explicitly", () => {
  assert.match(validateObserveFixture(null, "weather")[0], /unknown observation kind/);
});

test("late-flash: the bump-shaded rock look is refused", () => {
  const fields = attestation("late-flash");
  for (const e of fields.emitters) {
    e.colorTexture = "tests\\fixtures\\bumptest\\zz_bumpgold_rock.tga";
    e.blendMode = 11;
  }
  assert.deepEqual(validateObserveFixture(fields, "late-flash"), [0, 1].flatMap((i) => [
    `fixture late-flash: emitters[${i}].colorTexture differs after reload`,
    `fixture late-flash: emitters[${i}].blendMode differs after reload`,
  ]));
});

test("late-flash: authored colors cannot substitute for byte-quantized reload values", () => {
  const fields = attestation("late-flash");
  fields.emitters[0].red = 0.5;
  fields.emitters[1].green = 0.5;
  assert.deepEqual(validateObserveFixture(fields, "late-flash"), [
    "fixture late-flash: emitters[0].red differs after reload",
    "fixture late-flash: emitters[1].green differs after reload",
  ]);
});
