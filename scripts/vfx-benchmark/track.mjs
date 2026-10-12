// Step-track rules for the flipbook class (plan 3.5 row 3, 3.8 clause 4).
//
// A proposal is first checked as the editor would persist it, then projected
// through the save and reload, re-checked, and only then compared with the
// base by playback: the engine samples a step track at the particle's
// normalised age a in [0, 100] as the value of the last key whose time is
// strictly below a (the first key's value at a = 0), floors it to an atlas
// cell, and keeps sampling the final key past 100 through float32 age
// overshoot. Equivalence is decided exactly, with no arithmetic on values:
// on every interval of the union of both tracks' key times, at a = 0, and on
// the final key.

export const MAX_TRACK_KEYS = 65536;   // the benchmark's conservative limit (the loader takes 65,537)

const f = Math.fround;

// The atlas cells the renderer uses: floor(sqrt(max(1, textureSize)))^2.
export function atlasCells(textureSize) {
  const side = Math.floor(Math.sqrt(Math.max(1, textureSize)));
  return side * side;
}

export function isPerfectSquare(textureSize) {
  return Number.isInteger(textureSize) && textureSize > 0 && atlasCells(textureSize) === textureSize;
}

// Structural validity: [] when the keys are a track the editor would persist as written.
export function trackErrors(keys, cells) {
  const errors = [];
  if (!Array.isArray(keys) || keys.length < 2) return ["a track needs at least two keys"];
  if (keys.length > MAX_TRACK_KEYS) errors.push(`${keys.length} keys (limit ${MAX_TRACK_KEYS})`);
  if (!keys.every((k) => Array.isArray(k) && k.length === 2 && Number.isFinite(k[0]) && Number.isFinite(k[1]))) {
    return [...errors, "every key must be two finite numbers"];
  }
  if (keys[0][0] !== 0) errors.push("the first key must be at time 0");
  if (keys[keys.length - 1][0] !== 100) errors.push("the last key must be at time 100");
  for (let i = 1; i < keys.length - 1; i++) {
    const t = keys[i][0];
    if (!(t > 0 && t < 100)) { errors.push(`interior key ${i} at ${t} is not strictly inside (0, 100)`); break; }
  }
  for (let i = 1; i < keys.length; i++) {
    if (!(keys[i][0] > keys[i - 1][0])) { errors.push(`key ${i} is not strictly after key ${i - 1}`); break; }
  }
  for (let i = 0; i < keys.length; i++) {
    const v = f(keys[i][1]);
    if (!(v >= 0 && v < cells)) { errors.push(`key ${i} value ${keys[i][1]} is outside the atlas cells [0, ${cells})`); break; }
  }
  return errors;
}

// The keys a save and reload hand back: endpoints re-stamped at 0 and 100,
// interior times divided by 100 on save and multiplied back on load in
// float32, every value stored as float32.
export function projectTrack(keys) {
  return keys.map(([t, v], i) => {
    const time = i === 0 ? 0 : i === keys.length - 1 ? 100 : f(f(f(t) / 100) * 100);
    return [time, Object.is(f(v), -0) ? 0 : f(v)];
  });
}

// The value a step track plays on the interval just above `time`: the last
// key whose time is at or below it (strictly below every age in the interval).
function valueAfter(keys, time) {
  let value = keys[0][1];
  for (const [t, v] of keys) {
    if (t <= time) value = v;
    else break;
  }
  return value;
}

// Floored playback equality over [0, 100] plus the terminal tail.
export function stepTrackEquivalent(base, proposal) {
  if (Math.floor(base[0][1]) !== Math.floor(proposal[0][1])) return false;   // a = 0
  if (Math.floor(base[base.length - 1][1]) !== Math.floor(proposal[proposal.length - 1][1])) return false;   // a > 100
  const times = [...new Set([...base, ...proposal].map(([t]) => t))].sort((a, b) => a - b);
  for (const t of times) {
    if (t >= 100) break;
    if (Math.floor(valueAfter(base, t)) !== Math.floor(valueAfter(proposal, t))) return false;
  }
  return true;
}

// A proposal against the base track: { ok } or { ok: false, reason }.
export function acceptTrack(baseKeys, cells, proposal) {
  const raw = trackErrors(proposal, cells);
  if (raw.length) return { ok: false, reason: `invalid track: ${raw[0]}` };
  const projected = projectTrack(proposal);
  const saved = trackErrors(projected, cells);
  if (saved.length) return { ok: false, reason: `invalid after save: ${saved[0]}` };
  return stepTrackEquivalent(baseKeys, projected) ? { ok: true } : { ok: false, reason: "plays differently from the intended track" };
}
