// Reload-attestation contract for the native observation fixture generator.
// No renderer dependency: the observe lane checks this before measuring frames.

export const OBSERVE_FIXTURE_KINDS = [
  "late-flash", "guard-short", "death-child", "child-starve",
  "budget-bailout", "budget-bailout-skip", "index-cap",
];

const base = {
  parent: -1, spawnDuringLife: -1, spawnOnDeath: -1,
  colorTexture: "tests\\fixtures\\bumptest\\zz_bumpgold_rock.tga",
  normalTexture: "tests\\fixtures\\bumptest\\zz_bumptest_nm.tga",
  blendMode: 11, textureSize: 1,
  useBursts: true, nBursts: 1, nParticlesPerSecond: 0,
  initialDelay: 0, skipTime: 0, burstDelay: 10000,
  gravity: 0, freezeTime: 0, randomLifetimePerc: 0, randomScalePerc: 0,
  isWeatherParticle: false, positionGroup: 1,
  positionMin: [-8, -8, -8], positionMax: [8, 8, 8],
  speedGroup: 0, speed: [0, 0, 0], scale: 8,
  red: 1, green: 1, blue: 1, alpha: 1,
};

// ALO stores RGBA endpoints as bytes, then reloads them as float32 / 255.
const dim = Math.fround(31 / 255);   // authored 0.125
const tint = Math.fround(63 / 255);  // authored 0.25
const half = Math.fround(127 / 255); // authored 0.5
const softTexture = "tests\\fixtures\\observe\\zz_observe_soft.tga";

function emitter(name, nParticlesPerBurst, lifetime, extra = {}) {
  return { ...base, name, nParticlesPerBurst, lifetime, ...extra };
}

function expectedEmitters(kind) {
  switch (kind) {
    // Grey alpha smoke plus a bright additive flash, so the timing reads by eye.
    case "late-flash": return [
      emitter("core", 64, 1, {
        colorTexture: softTexture, blendMode: 2,
        positionMin: [-40, -20, -40], positionMax: [40, 20, 40],
        speed: [0, 0, 1], scale: 24, red: half, green: half, blue: half, alpha: tint,
      }),
      emitter("flash", 8, 0.5, {
        colorTexture: softTexture, blendMode: 1,
        initialDelay: 0.75, scale: 40, green: half, blue: dim,
      }),
    ];
    case "guard-short": return [emitter("burst", 2048, 0.5)];
    case "death-child": return [
      emitter("parent", 8, 0.25, { spawnOnDeath: 1 }),
      emitter("death-child", 1, 0.75, { parent: 0 }),
    ];
    case "child-starve": return [
      emitter("parent", 200, 0.5, { spawnDuringLife: 1, spawnOnDeath: 2 }),
      emitter("lifetime-child", 1, 0.25, { parent: 0, useBursts: false, nParticlesPerSecond: 60 }),
      emitter("death-child", 1, 0.75, { parent: 0 }),
    ];
    // Finite catch-up bursts let the budget cases end on their quiet tail.
    case "budget-bailout":
    case "budget-bailout-skip": return [
      emitter("budget-burst", 1000, 0.5, { initialDelay: 0.0078125 }),
      emitter("catch-up", 1, 0.5, {
        nBursts: 30, burstDelay: Math.fround(0.01), initialDelay: 0.0078125,
        skipTime: kind === "budget-bailout-skip" ? 0.25 : 0,
      }),
    ];
    case "index-cap": return [emitter("burst", 20000, 0.5)];
    default: return null;
  }
}

export function validateObserveFixture(fields, kind) {
  const expected = expectedEmitters(kind);
  if (!expected) return [`fixture ${kind}: unknown observation kind`];
  const problems = [];
  const check = (actual, wanted, key) => {
    const equal = Array.isArray(wanted)
      ? Array.isArray(actual) && actual.length === wanted.length && wanted.every((value, index) => actual[index] === value)
      : actual === wanted;
    if (!equal) problems.push(`fixture ${kind}: ${key} differs after reload`);
  };
  check(fields?.kind, kind, "kind");
  check(fields?.reloaded, true, "reloaded");
  check(fields?.emitterCount, expected.length, "emitterCount");
  check(fields?.rootCount, expected.filter((e) => e.parent === -1).length, "rootCount");
  if (!Array.isArray(fields?.emitters)) {
    problems.push(`fixture ${kind}: emitters missing after reload`);
    return problems;
  }
  check(fields.emitters.length, expected.length, "emitters.length");
  expected.forEach((e, index) => {
    for (const [key, value] of Object.entries(e))
      check(fields.emitters[index]?.[key], value, `emitters[${index}].${key}`);
  });
  return problems;
}
