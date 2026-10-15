// [vfx-build] The pair rule, checked on a built effect.
//   node scripts/vfx-build/pair-check.mjs <effect.alo> [--exe <ParticleEditor.exe>]
//   node scripts/vfx-build/pair-check.mjs --inspect <inspect.json>
//
// A layered explosion draws its core as triplets of root emitters rendered
// from one simulation: <T>_SMOKE_EARLY, <T>_FIRE and <T>_SMOKE_LATE (the late
// smoke is optional). Every emitter rolls its own randoms per particle
// (src/simulation/EmitterInstance.cpp, GenerateRandomProperty and
// ResetParticle), so the members line up only when nothing is rolled and
// everything that moves or sizes a particle is the same:
//   - no randoms: randomScalePerc, randomLifetimePerc, randomColors,
//     randomRotation and randomRotationDirection off, and speed and position
//     groups that roll nothing (exact, or of zero extent);
//   - the same spawning, motion and parent speed inherit (100 %), and root
//     emitters, the same atlas;
//   - the same scale and rotation-speed values at every 60 fps game frame
//     both live through, with the smoke not rotating (its light is baked);
//   - early smoke and fire showing the same cell on every game frame, for the
//     same lifetime;
//   - each member's cells playing forward and ending on the atlas's last
//     cell, never past it;
//   - the fire drawn after its early smoke.
// The design: tasks/2026-10-14-layered-explosion-design.md, "The pair rule".
//
// Prints one `[pair-check]` JSON line. Exit 0 every triplet holds (or there
// are none), 5 a rule is broken, 2 usage, 3 the effect could not be inspected.
import { spawnSync } from "node:child_process";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { isDeepStrictEqual } from "node:util";
import { INDEX, ROTATION, SCALE, at, frameAges } from "./tracks.mjs";

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..", "..");
export const ROLES = { early: "_SMOKE_EARLY", fire: "_FIRE", late: "_SMOKE_LATE" };
// Float32 tracks compare to a millionth of their size.
const close = (a, b) => Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(a), Math.abs(b));

// The triplets in an inspect dump, by name: { T: { early, fire, late } },
// each member { position, emitter }.
export function triplets(doc) {
  const found = {};
  doc.emitters.forEach((emitter, position) => {
    for (const [role, suffix] of Object.entries(ROLES)) {
      if (!emitter.name.endsWith(suffix) || emitter.name.length === suffix.length) continue;
      const t = emitter.name.slice(0, -suffix.length);
      (found[t] ??= {})[role] = { position, emitter };
    }
  });
  return found;
}

// A group that gives every particle the same value.
function rollsNothing(g) {
  if (g.type === 0) return true;
  if (g.type === 1) return g.minX === g.maxX && g.minY === g.maxY && g.minZ === g.maxZ;
  if (g.type === 2) return g.sideLength === 0;
  if (g.type === 3) return g.sphereRadius === 0;
  if (g.type === 4) return g.cylinderRadius === 0 && g.cylinderHeight === 0;
  return false;
}

// The fields that spawn, move or place a particle; members must agree on all.
const SHARED = ["parent", "linkToSystem", "objectSpaceAcceleration", "affectedByWind", "isWorldOriented", "useBursts",
  "gravity", "initialDelay", "burstDelay", "inwardSpeed", "inwardAcceleration", "acceleration", "parentLinkStrength",
  "bounciness", "freezeTime", "skipTime", "nBursts", "nParticlesPerSecond", "nParticlesPerBurst", "groundBehavior",
  "textureSize", "emitFromMesh"];

// The broken rules of triplet `t`, as [{ rule, message }].
export function checkTriplet(t, members) {
  const out = [];
  const broke = (rule, message) => out.push({ rule, message });
  const { early, fire, late } = members;
  if (!early) { broke("incomplete", `${t} has no ${t}${ROLES.early}`); return out; }
  const present = Object.entries(members);
  const name = (role) => `${t}${ROLES[role]}`;

  for (const [role, { emitter: e }] of present) {
    const rolled = ["randomScalePerc", "randomLifetimePerc"].filter((f) => e[f] !== 0);
    if (e.randomColors.some((c) => c !== 0)) rolled.push("randomColors");
    if (e.randomRotation) rolled.push("randomRotation");
    if (e.randomRotationDirection) rolled.push("randomRotationDirection");
    if (!rollsNothing(e.groups[0])) rolled.push("groups[0] (speed)");
    if (!rollsNothing(e.groups[2])) rolled.push("groups[2] (position)");
    if (rolled.length) broke("randoms", `${name(role)} rolls ${rolled.join(", ")} per particle`);
    if (e.parent !== null) broke("root", `${name(role)} is not a root emitter`);
    if (e.parentLinkStrength !== 1) broke("inherit", `${name(role)} inherits ${e.parentLinkStrength * 100} % of its parent's speed, not 100 %`);
  }
  for (const [role, { emitter: e }] of present) {
    if (role === "early") continue;
    for (const field of ["groups[0]", "groups[2]", ...SHARED]) {
      const pick = (x) => (field.startsWith("groups") ? x.groups[Number(field[7])] : x[field]);
      if (!isDeepStrictEqual(pick(e), pick(early.emitter)))
        broke("shared", `${name(role)}.${field} is ${JSON.stringify(pick(e))}, ${name("early")}'s is ${JSON.stringify(pick(early.emitter))}`);
    }
  }

  // Scale and rotation speed, at every game frame both members live through.
  for (const [role, { emitter: e }] of present) {
    if (role === "early") continue;
    const life = Math.min(e.lifetime, early.emitter.lifetime);
    for (const [track, label] of [[SCALE, "scale"], [ROTATION, "rotation speed"]]) {
      const ages = frameAges(life);
      const bad = ages.find((a) => !close(at(e.tracks[track], a, e.lifetime), at(early.emitter.tracks[track], a, early.emitter.lifetime)));
      if (bad !== undefined)
        broke("tracks", `${name(role)}'s ${label} is ${at(e.tracks[track], bad, e.lifetime)} at ${bad.toFixed(4)} s, ` +
              `${name("early")}'s ${at(early.emitter.tracks[track], bad, early.emitter.lifetime)}`);
    }
  }
  for (const role of ["early", "late"]) {
    const e = members[role]?.emitter;
    if (e && frameAges(e.lifetime).some((a) => at(e.tracks[ROTATION], a, e.lifetime) !== 0))
      broke("rotation", `${name(role)} rotates; its light is baked from above`);
  }

  // Cells.
  const cell = (e, a) => Math.floor(at(e.tracks[INDEX], a, e.lifetime));
  for (const [role, { emitter: e }] of present) {
    const shown = frameAges(e.lifetime).map((a) => cell(e, a));
    const last = Math.floor(e.tracks[INDEX].keys.at(-1)[1]);
    if (shown.some((c, i) => i > 0 && c < shown[i - 1])) broke("index", `${name(role)}'s cells play backwards`);
    if (shown.some((c) => c < 0 || c >= e.textureSize) || last >= e.textureSize)
      broke("index", `${name(role)}'s index passes its ${e.textureSize} cells; the engine wraps it`);
    else if (last !== e.textureSize - 1) broke("index", `${name(role)}'s index ends on cell ${last}, not its last, ${e.textureSize - 1}`);
  }
  if (fire) {
    if (fire.emitter.lifetime !== early.emitter.lifetime)
      broke("lifetime", `${name("fire")} lives ${fire.emitter.lifetime} s, ${name("early")} ${early.emitter.lifetime} s`);
    const bad = frameAges(early.emitter.lifetime).find((a) => cell(fire.emitter, a) !== cell(early.emitter, a));
    if (bad !== undefined)
      broke("cells", `at ${bad.toFixed(4)} s ${name("fire")} shows cell ${cell(fire.emitter, bad)}, ${name("early")} cell ${cell(early.emitter, bad)}`);
    if (fire.position < early.position) broke("order", `${name("fire")} draws before ${name("early")}; the fire goes after its smoke`);
  }
  return out;
}

export function checkPairs(doc) {
  const found = triplets(doc);
  const results = Object.entries(found).map(([t, members]) => ({
    triplet: t,
    members: Object.fromEntries(Object.entries(members).map(([role, m]) => [role, m.position])),
    violations: checkTriplet(t, members),
  }));
  return { ok: results.every((r) => r.violations.length === 0), triplets: results };
}

function inspect(exe, alo) {
  const dir = mkdtempSync(join(tmpdir(), "pe-pair-check-"));
  try {
    const out = join(dir, "inspect.json");
    const r = spawnSync(exe, ["--tool", "inspect", alo, "--out", out], { encoding: "utf8", windowsHide: true, timeout: 60000 });
    if (r.status !== 0) return { error: `inspect exited ${r.status}: ${(r.stderr ?? "").trim()}` };
    return { doc: JSON.parse(readFileSync(out, "utf8")) };
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

const USAGE = "usage: pair-check.mjs <effect.alo> [--exe <ParticleEditor.exe>] | --inspect <inspect.json>";
export function main(argv, { say = console.log } = {}) {
  const report = (record) => { say(`[pair-check] ${JSON.stringify(record)}`); return record.exitCode; };
  let alo, exe = join(repoRoot, "x64", "Release", "ParticleEditor.exe"), inspectPath;
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--exe" && argv[i + 1]) exe = argv[++i];
    else if (argv[i] === "--inspect" && argv[i + 1]) inspectPath = argv[++i];
    else if (!argv[i].startsWith("--") && alo === undefined) alo = argv[i];
    else return report({ ok: false, exitCode: 2, error: USAGE });
  }
  if ((alo === undefined) === (inspectPath === undefined)) return report({ ok: false, exitCode: 2, error: USAGE });
  let doc;
  if (inspectPath !== undefined) {
    try { doc = JSON.parse(readFileSync(inspectPath, "utf8")); }
    catch (error) { return report({ ok: false, exitCode: 3, error: `cannot read ${inspectPath}: ${error.message}` }); }
  } else {
    if (!existsSync(exe)) return report({ ok: false, exitCode: 3, error: `exe not found: ${exe}` });
    const r = inspect(exe, resolve(alo));
    if (r.error) return report({ ok: false, exitCode: 3, error: r.error });
    doc = r.doc;
  }
  if (!Array.isArray(doc?.emitters)) return report({ ok: false, exitCode: 3, error: "not an inspect document" });
  const result = checkPairs(doc);
  return report({ ...result, exitCode: result.ok ? 0 : 5 });
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) process.exitCode = main(process.argv.slice(2));
