// The --tool build ops that turn a new emitter into a copy of an inspected one.
//
// `emitter` and `base` are emitters as `--tool inspect` writes them; `base` is a new
// emitter's (fixtures/new-emitter.json). Only the fields that differ from `base` get
// an op. Left out, as the build sets or derives them: the name, index, parent, link
// group and children (the spec's own entries and `lifeChild`), and the lifetime
// group, which a save derives from `lifetime` and `randomLifetimePerc`.
import { isDeepStrictEqual } from "node:util";

const STRUCTURE = new Set(["name", "index", "parent", "linkGroup", "spawnDuringLife", "spawnOnDeath"]);
const VECTORS = new Set(["acceleration", "randomColors"]);
const INTERPOLATION = ["linear", "smooth", "step"];
const ROTATION = 6;
const SPAWN_GROUPS = [0, 2];   // speed and position; group 1 (lifetime) is derived

export function cloneOps(emitter, base) {
  const ops = [];
  const set = (field, value) => ops.push({ op: "setProperty", field, value });
  for (const [field, value] of Object.entries(emitter)) {
    if (STRUCTURE.has(field) || field === "tracks" || field === "groups") continue;
    if (isDeepStrictEqual(value, base[field])) continue;
    if (field === "colorTexture") ops.push({ op: "setColorTexture", name: value });
    else if (field === "normalTexture") ops.push({ op: "setNormalTexture", name: value });
    else if (VECTORS.has(field)) value.forEach((v, i) => { if (v !== base[field][i]) set(`${field}[${i}]`, v); });
    else set(field, value);
  }
  for (const g of SPAWN_GROUPS)
    for (const [field, value] of Object.entries(emitter.groups[g]))
      if (value !== base.groups[g][field]) set(`groups[${g}].${field}`, value);
  return [...ops, ...trackOps(emitter, base)];
}

// Locks first, then each track that owns its keys: its interpolation and keys,
// naming the channels locked to it as the lock rules require.
function trackOps(emitter, base) {
  const ops = [];
  const source = (t) => emitter.tracks[t].source;
  for (let t = 1; t < 4; t++) {
    if (source(t) === base.tracks[t].source) continue;
    ops.push(source(t) === t ? { op: "setTrackLock", track: t } : { op: "setTrackLock", track: t, lockTo: source(t) });
  }
  emitter.tracks.forEach((track, t) => {
    if (source(t) !== t) return;
    // Rotation keys are not saved while randomRotation is on: nothing to copy.
    if (t === ROTATION && emitter.randomRotation) return;
    if (isDeepStrictEqual(track, base.tracks[t]) && source(t) === base.tracks[t].source) return;
    const dependents = emitter.tracks.map((u, j) => j).filter((j) => j !== t && source(j) === t);
    const withDependents = (op) => (dependents.length ? { ...op, dependents } : op);
    if (track.interpolation !== base.tracks[t].interpolation)
      ops.push(withDependents({ op: "setInterpolation", track: t, interpolation: INTERPOLATION[track.interpolation] }));
    ops.push(withDependents({ op: "replaceTrack", track: t, keys: track.keys }));
  });
  return ops;
}
