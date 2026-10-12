import { test } from "node:test";
import assert from "node:assert/strict";
import {
  F32_MAX, F32_MIN_POSITIVE, acceptScalar, float32Text, inRawDomain, interveneContract, nextFloat32, project,
  projectInterval, restoreContract, seedContract,
} from "./persisted.mjs";

const f = Math.fround;
const pass = (accept, v) => assert.equal(acceptScalar(accept, v).ok, true, `${v} should pass`);
const fail = (accept, v, reason) => {
  const r = acceptScalar(accept, v);
  assert.equal(r.ok, false, `${v} should fail`);
  if (reason) assert.match(r.reason, reason);
};

test("raw domain precedes projection", () => {
  assert.equal(inRawDomain("randomLifetimePerc", -1e-50), false);
  assert.equal(project("randomLifetimePerc", 0), 0);
  assert.ok(Object.is(project("randomLifetimePerc", -0), 0));
  assert.equal(inRawDomain("randomLifetimePerc", -0.5), false);
  assert.equal(inRawDomain("nParticlesPerBurst", 2.5), false);
  assert.equal(inRawDomain("nParticlesPerBurst", 4294967296), false);
  assert.equal(inRawDomain("lifetime", Infinity), false);
  assert.equal(inRawDomain("initialDelay", -0.1), false);
  assert.equal(inRawDomain("lifetime", 0), false);
});

test("restore: base 4, case 2 accepts exactly 2.5 to 4", () => {
  const c = restoreContract("lifetime", 4, 2);
  assert.deepEqual(c.interval, [2.5, 4]);
  pass(c, 2.5); pass(c, 4); pass(c, 3);
  fail(c, nextFloat32(2.5, "decrease"), /interval/);
  fail(c, nextFloat32(4, "increase"), /interval/);
  fail(c, 5, /interval/);
  fail(c, 2, /side/);
  fail(c, 1, /side/);
});

test("restore: base 2, case 4 rejects the overshoot 1", () => {
  const c = restoreContract("lifetime", 2, 4);
  assert.equal(c.direction, "decrease");
  assert.deepEqual(c.interval, [2, 3.5]);
  fail(c, 1, /interval/);
  pass(c, 2);
});

test("restore: the projected quarter endpoint passes where a ratio rule would not", () => {
  const c = restoreContract("randomLifetimePerc", f(0.1), f(0.9));
  assert.equal(c.interval[1], f(0.7));
  assert.equal(float32Text(c.interval[1]), "0.7");
  assert.equal(c.interval[1], 0.699999988079071);
  pass(c, 0.699999988079071);
  fail(c, nextFloat32(c.interval[1], "increase"), /interval/);
  pass(c, f(0.1));
  fail(c, nextFloat32(f(0.1), "decrease"), /interval/);
});

test("restore: a zero delta is refused", () => {
  assert.match(restoreContract("lifetime", 4, 4).error, /planted delta/);
});

test("restore on counts uses exact integers, sorted before ceiling and floor", () => {
  const big = restoreContract("nParticlesPerBurst", 16777217, 100663302);
  assert.deepEqual(big.interval, [16777217, 79691780]);
  pass(big, 79691780); pass(big, 16777217);
  fail(big, 79691781, /interval/);
  fail(big, 16777216, /interval/);
  fail(big, 79691780.5, /domain/);
  const up = restoreContract("nParticlesPerBurst", 16777217, 8388608);
  assert.deepEqual(up.interval, [10485761, 16777217]);
  pass(up, 10485761); pass(up, 16777216); pass(up, 16777217);
  fail(up, 16777218); fail(up, 10485760);
});

test("intervene: a 0.2 s target gives 0.02-0.1 s, a zero target 0.1-0.5 s", () => {
  const c = interveneContract("initialDelay", f(0.2), f(0.2), "decrease");
  assert.deepEqual(c.interval, [f(f(0.2) - 0.5 * f(0.2)), f(f(0.2) - 0.1 * f(0.2))]);
  pass(c, 0.1); pass(c, 0.18);
  fail(c, 0.19, /interval/);
  fail(c, 0.25, /side/);
  const zero = interveneContract("initialDelay", 0, 0, "increase");
  assert.deepEqual(zero.interval, [f(0.1), f(0.5)]);
  pass(zero, 0.3); fail(zero, 0.6);
  assert.match(interveneContract("initialDelay", 0, 0, "decrease").error, /empty/);
  assert.match(interveneContract("initialDelay", f(0.2), f(0.3), "decrease").error, /stated target/);
});

test("intervene: two packs of one base derive the same interval", () => {
  assert.deepEqual(interveneContract("lifetime", 4, 4, "increase"), interveneContract("lifetime", 4, 4, "increase"));
});

test("a count intervention is clipped to uint32 and 4294967296 is refused", () => {
  const c = interveneContract("nParticlesPerSecond", 3000000000, 3000000000, "increase");
  assert.deepEqual(c.interval, [3300000000, 4294967295]);
  pass(c, 4294967295);
  fail(c, 4294967296, /domain/);
});

test("a float intervention is clipped to the finite float32 domain", () => {
  const v = f(3e38);
  const c = interveneContract("lifetime", v, v, "increase");
  assert.deepEqual(c.interval, [3.299999965482712e38, F32_MAX]);
  pass(c, F32_MAX);
  fail(c, 3.5e38, /projects outside/);
  assert.match(interveneContract("lifetime", f(3.2e38), f(3.2e38), "increase").error, /empty/);
});

test("seed: fractions 0.6 and 0.1 decreased print the generated endpoints", () => {
  const six = seedContract("randomLifetimePerc", f(0.6), "decrease");
  assert.deepEqual(six.interval.map(float32Text), ["0.100000024", "0.47500002"]);
  pass(six, six.interval[0]); pass(six, six.interval[1]);
  fail(six, f(0.1), /interval/);
  fail(six, nextFloat32(six.interval[1], "increase"), /interval/);
  const one = seedContract("randomLifetimePerc", f(0.1), "decrease");
  assert.deepEqual(one.interval, [0, f(0.075)]);
  pass(one, 0); pass(one, 0.075);
  fail(one, nextFloat32(f(0.075), "increase"), /interval/);
  fail(one, -F32_MIN_POSITIVE, /domain/);
  assert.match(seedContract("randomLifetimePerc", 0, "decrease").error, /zero/);
});

test("seed: the subnormal fraction moves to 0, never stays put", () => {
  const c = seedContract("randomLifetimePerc", F32_MIN_POSITIVE, "decrease");
  assert.deepEqual(c.interval, [0, F32_MIN_POSITIVE]);
  fail(c, F32_MIN_POSITIVE, /side/);
  pass(c, 0);
});

test("seed: a lifetime at the smallest float32 has no positive correction", () => {
  assert.match(seedContract("lifetime", F32_MIN_POSITIVE, "decrease").error, /holds no value/);
  const c = seedContract("lifetime", 4, "decrease");
  assert.deepEqual(c.interval, [2, 3.5]);
  assert.match(seedContract("nParticlesPerBurst", 4, "decrease").error, /no seed budget/);
});

test("projectInterval sorts, projects and clips", () => {
  assert.deepEqual(projectInterval("randomScalePerc", 1.5, 0.5), [0.5, 1]);
  assert.equal(projectInterval("randomScalePerc", 1.5, 2), null);
  assert.deepEqual(projectInterval("nParticlesPerBurst", 0.2, 3.7), [1, 3]);
});

test("float32Text is the shortest round trip", () => {
  assert.equal(float32Text(f(0.1)), "0.1");
  assert.equal(float32Text(0.100000024), "0.100000024");
  assert.equal(float32Text(F32_MAX), "3.4028235e+38");
});
