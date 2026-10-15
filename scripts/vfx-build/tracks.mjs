// [vfx-build] Emitter tracks from a generated flipbook, and the engine's way of
// reading a track, for `--tool build` specs and the pair check.
//
// A particle's tracks are keyed in percent of its life. Each frame the engine
// advances a track's cursor while the particle's age (in percent) is past the
// next key's time, then reads it (src/simulation/EmitterInstance.cpp,
// UpdateTrackCursors and SampleTrack): `step` returns the earlier key's value,
// so at exactly a key's time the earlier value still shows. The flipbook cell
// is floor of the index track, with no blending between cells.
//
// So an index key placed at frame k's exact time would show cell k one frame
// late, or not, depending on float rounding. indexKeys places each key half a
// game frame early: cell k shows from the frame its sim time names, whatever
// the rounding.

export const FPS = 60;
// The interpolation codes in the inspect dump (ParticleSystem.h).
export const LINEAR = 0, SMOOTH = 1, STEP = 2;
// The track numbers (ParticleSystem.h).
export const RED = 0, GREEN = 1, BLUE = 2, ALPHA = 3, SCALE = 4, INDEX = 5, ROTATION = 6;

const fail = (message) => { throw new Error(message); };

// A track's value `rel` percent into a particle's life, as the engine reads
// it. `track` is an inspect track: { interpolation, keys: [[time, value], ...] }.
export function sample(track, rel) {
  const keys = track.keys;
  if (rel > keys.at(-1)[0]) return keys.at(-1)[1];   // the cursor parks on the last key
  let next = 0;
  while (next < keys.length - 1 && rel > keys[next][0]) next++;
  const prev = next > 0 && rel > 0 ? next - 1 : next;
  const [t0, v0] = keys[prev], [t1, v1] = keys[next];
  if (t1 === t0) return v1;
  const u = (rel - t0) / (t1 - t0);
  if (track.interpolation === STEP) return v0;
  if (track.interpolation === SMOOTH) return v0 * (2 * u ** 3 - 3 * u ** 2 + 1) + v1 * (3 * u ** 2 - 2 * u ** 3);
  return v0 + u * (v1 - v0);
}

// The game frames a particle of `lifetime` seconds is drawn on, as ages in
// seconds: 0, 1/60, ... while the age is short of its death.
export function frameAges(lifetime) {
  const ages = [];
  for (let n = 0; n / FPS < lifetime; n++) ages.push(n / FPS);
  return ages;
}

// A track's value at `age` seconds into a particle of `lifetime` seconds.
export const at = (track, age, lifetime) => sample(track, (age / lifetime) * 100);

const percent = (seconds, lifetime) => Math.fround((seconds / lifetime) * 100);

// The index track (step) that shows cell k from the game frame at
// cellSeconds[k], for a particle of `lifetime` seconds spawned at sim time 0.
// The last key, at 100, holds the last cell: the track never reaches the cell
// count, so it never wraps to cell 0.
export function indexKeys(cellSeconds, lifetime) {
  if (!Array.isArray(cellSeconds) || cellSeconds.length < 1) fail("indexKeys needs the cells' times");
  if (!(lifetime > 0)) fail("indexKeys needs a positive lifetime");
  if (cellSeconds[0] !== 0) fail("the first cell must show from age 0");
  const keys = [[0, 0]];
  for (let k = 1; k < cellSeconds.length; k++) {
    if (!(cellSeconds[k] > cellSeconds[k - 1])) fail(`cell ${k}'s time is not after cell ${k - 1}'s`);
    const t = percent(cellSeconds[k] - 0.5 / FPS, lifetime);
    if (!(t < 100)) fail(`cell ${k} at ${cellSeconds[k]} s never shows in a ${lifetime} s life`);
    keys.push([t, k]);
  }
  keys.push([100, cellSeconds.length - 1]);
  return keys;
}

// A linear track through `samples` ([[seconds, value], ...], the first at 0),
// for a particle of `lifetime` seconds: past the last sample it holds, and a
// sample after the particle's death is cut at 100.
export function linearKeys(samples, lifetime) {
  if (!Array.isArray(samples) || samples.length < 1 || samples[0][0] !== 0) fail("linearKeys needs samples from 0 s");
  if (!(lifetime > 0)) fail("linearKeys needs a positive lifetime");
  const keys = [];
  for (let i = 0; i < samples.length; i++) {
    const [s, v] = samples[i];
    if (i > 0 && !(s > samples[i - 1][0])) fail(`sample ${i} is not after sample ${i - 1}`);
    if (s >= lifetime) {
      const [s0, v0] = samples[i - 1];
      keys.push([100, Math.fround(v0 + ((lifetime - s0) / (s - s0)) * (v - v0))]);
      return keys;
    }
    keys.push([percent(s, lifetime), Math.fround(v)]);
  }
  if (keys.at(-1)[0] !== 100) keys.push([100, keys.at(-1)[1]]);
  return keys;
}

// A cache entry's per-cell spans as [[seconds, span], ...] (provenance
// cellSeconds and cellSpan).
export function spanSamples(provenance) {
  const { cellSeconds, cellSpan } = provenance ?? {};
  if (!Array.isArray(cellSeconds) || !Array.isArray(cellSpan) || cellSeconds.length !== cellSpan.length)
    fail("the provenance has no cellSeconds and cellSpan of one length");
  return cellSeconds.map((t, k) => [t, cellSpan[k]]);
}

// The late smoke's spans: the early smoke's, held to the early smoke's death
// (`earlyLifetime`, so the two agree on every frame both draw, the last one
// included), then its own, which start after it.
export function handOffSamples(earlySamples, earlyLifetime, lateSamples) {
  if (!(lateSamples[0]?.[0] > earlyLifetime)) fail("the late samples must start after the early smoke's death");
  return [...earlySamples.filter(([s]) => s < earlyLifetime), [earlyLifetime, earlySamples.at(-1)[1]], ...lateSamples];
}

// The `--tool build` ops that play a flipbook entry: its index track, and the
// scale track from `samples` (spans in sim units) times `size` (world units
// a sim unit). `textureSize` is the cell count.
export function flipbookOps({ cellSeconds, samples, lifetime, size, cells }) {
  if (!(size > 0)) fail("flipbookOps needs a positive size");
  return [
    { op: "setProperty", field: "lifetime", value: lifetime },
    { op: "setProperty", field: "textureSize", value: cells },
    { op: "setInterpolation", track: INDEX, interpolation: "step" },
    { op: "replaceTrack", track: INDEX, keys: indexKeys(cellSeconds, lifetime) },
    { op: "replaceTrack", track: SCALE, keys: linearKeys(samples.map(([s, span]) => [s, span * size]), lifetime) },
  ];
}
