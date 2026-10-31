// Refusal events belong to cause sites; the suppression latch also handles
// already-counted refusals, so counting it would double-count or misattribute.
// Catch-up and index-cap accounting run against the real engine in the observe
// lane: budget-bailout, budget-bailout-skip and index-cap pin exact totals and
// lifecycle endpoints. Keep source checks only for unobserved ordering/state.

import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";

function source(file) {
  return readFileSync(new URL("../src/" + file, import.meta.url), "utf8")
    .replace(/\/\*[\s\S]*?\*\/|\/\/[^\r\n]*|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'/g,
      (text) => text.replace(/[^\r\n]/g, " "));
}

function block(code, signature) {
  const match = signature.exec(code);
  assert.ok(match, "missing cause site: " + signature);
  const start = code.indexOf("{", match.index + match[0].length);
  assert.ok(start !== -1, "missing block: " + signature);
  let depth = 1;
  let end = start + 1;
  for (; end < code.length && depth > 0; ++end) {
    if (code[end] === "{") ++depth;
    if (code[end] === "}") --depth;
  }
  assert.equal(depth, 0, "unbalanced block: " + signature);
  return code.slice(start + 1, end - 1);
}

const header = source("rendering/engine.h");
const engine = source("rendering/engine.cpp");
const emitter = source("simulation/EmitterInstance.cpp");
const counter = /m_(?:runtimeGuard|hardGuard|indexCap)Refusals/;

test("runtime counters increment only after budget admission has failed", () => {
  const spawn = block(header, /\bbool\s+TryConsumeSpawnBudget\s*\(\s*\)/);
  assert.match(spawn, /^\s*if\s*\(\s*!m_overloadGuardEnabled\s*\)\s*return\s+true\s*;\s*if\s*\(\s*m_spawnBudget\s*>\s*0\s*\)\s*\{\s*m_spawnBudget\s*--\s*;\s*return\s+true\s*;\s*\}\s*\+\+\s*m_runtimeGuardRefusals\s*;\s*m_overloadThisFrame\s*=\s*true\s*;\s*return\s+false\s*;\s*$/);
  const instance = block(header, /\bbool\s+TryConsumeInstanceBudget\s*\(\s*\)/);
  assert.match(instance, /^\s*if\s*\(\s*!m_overloadGuardEnabled\s*\)\s*return\s+true\s*;\s*if\s*\(\s*m_numEmitters\s*<\s*m_maxPreviewInstances\s*\)\s*return\s+true\s*;\s*\+\+\s*m_runtimeGuardRefusals\s*;\s*m_overloadThisFrame\s*=\s*true\s*;\s*return\s+false\s*;\s*$/);
});

test("both exhausted-budget catch-up bailouts retain suppression and stop iterating", () => {
  // Observation totals cover counting, but do not expose the overload latch.
  const sites = [...emitter.matchAll(/if\s*\(\s*m_engine\s*\.\s*SpawnBudgetExhausted\s*\(\s*\)\s*\)/g)];
  assert.equal(sites.length, 2, "spawn-round and skip-time bailout sites");
  for (const site of sites) {
    const refusal = block(emitter.slice(site.index), /^if\s*\(\s*m_engine\s*\.\s*SpawnBudgetExhausted\s*\(\s*\)\s*\)/);
    assert.match(refusal, /m_engine\s*\.\s*NoteSpawnSuppressed\s*\(\s*\)\s*;/);
    assert.match(refusal, /\bbreak\s*;\s*$/);
  }
});

test("hard counters sit inside the spawn-time and edit-time estimate refusals", () => {
  const spawn = block(engine, /Engine::SpawnParticleSystem\s*\([^)]*\)/);
  const spawnRefusal = block(spawn, /if\s*\(\s*projected\s*>\s*\(\s*double\s*\)\s*m_maxPreviewParticles\s*\)/);
  assert.match(spawnRefusal, /^\s*\+\+\s*m_hardGuardRefusals\s*;\s*m_spawnRefusal\s*=/);
  assert.match(spawnRefusal, /m_spawnRefusalPending\s*=\s*true\s*;\s*Clear\s*\(\s*\)\s*;\s*return\s+nullptr\s*;\s*$/);
  const edit = block(engine, /Engine::SetEstimatedLoad\s*\([^)]*\)/);
  const editRefusal = block(edit, /if\s*\(\s*m_overloadGuardEnabled\s*&&\s*perInstance\s*>\s*0\.0\s*&&\s*n\s*>\s*0\s*&&\s*n\s*\*\s*perInstance\s*>\s*\(\s*double\s*\)\s*m_maxPreviewParticles\s*\)/);
  assert.match(editRefusal, /^\s*\+\+\s*m_hardGuardRefusals\s*;\s*m_spawnRefusal\s*=/);
  assert.match(editRefusal, /m_spawnRefusalPending\s*=\s*true\s*;\s*Clear\s*\(\s*\)\s*;\s*$/);
  assert.equal((engine.match(/\+\+\s*m_hardGuardRefusals/g) ?? []).length, 2);
});

test("index cap frees the refused slot before returning", () => {
  const spawn = block(emitter, /EmitterInstance::SpawnParticle\s*\([^)]*\)/);
  const refusal = block(spawn, /if\s*\(\s*particle\s*\.\s*m_index\s*>\s*kMaxParticleIndex\s*\)/);
  // The observe lane checks index-only counting with the guard disabled;
  // telemetry cannot prove that the rejected allocation was freed here.
  assert.match(refusal, /FreeParticle\s*\(\s*particle\s*\)\s*;\s*return\s+false\s*;\s*$/);
  assert.doesNotMatch(refusal, /\bif\s*\(/);
});

test("suppression aggregation preserves the overload latch", () => {
  const suppression = block(header, /\bvoid\s+NoteSpawnSuppressed\s*\(\s*\)/);
  assert.match(suppression, /^\s*m_overloadThisFrame\s*=\s*true\s*;\s*$/);
  assert.doesNotMatch(suppression, counter);
  // Exact native refusal totals catch double-counting at the call sites;
  // the overload latch itself is not present in observation telemetry.
});

test("observation hooks are increment-only and Clear preserves lifetime counters", () => {
  for (const [hook, member] of [["RuntimeGuard", "runtimeGuard"], ["IndexCap", "indexCap"]]) {
    const body = block(header, new RegExp("\\bvoid\\s+Note" + hook + "RefusalForObservation\\s*\\(\\s*\\)"));
    assert.match(body, new RegExp("^\\s*\\+\\+\\s*m_" + member + "Refusals\\s*;\\s*$"));
  }
  for (const [getter, member] of [["RuntimeGuard", "runtimeGuard"], ["HardGuard", "hardGuard"], ["IndexCap", "indexCap"]]) {
    assert.match(header, new RegExp("std::uint64_t\\s+" + getter + "RefusalCountForObservation\\s*\\(\\s*\\)\\s+const\\s*\\{\\s*return\\s+m_" + member + "Refusals\\s*;\\s*\\}"));
    assert.match(header, new RegExp("std::uint64_t\\s+m_" + member + "Refusals\\s*=\\s*0\\s*;"));
  }
  assert.doesNotMatch(block(engine, /Engine::Clear\s*\(\s*\)/), counter);
  assert.equal((header.match(/\+\+\s*m_runtimeGuardRefusals/g) ?? []).length, 3);
  assert.equal((header.match(/\+\+\s*m_indexCapRefusals/g) ?? []).length, 1);
});
