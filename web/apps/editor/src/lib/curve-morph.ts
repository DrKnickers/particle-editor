// curve-morph — pure sampling/diffing core for the curve morph
// animation (Part B). See the spec:
// docs/superpowers/specs/2026-06-11-curve-morph-animation-design.md
//
// The legacy smooth curve (buildSmoothPath, CurveEditor.tsx: control
// points at 1/4 and 3/4 horizontal, cp1y=p1.y, cp2y=p2.y) reduces to
//   x(t) = x1 + dx * (0.75t + 0.75t^2 - 0.5t^3)   (monotonic: x' >= 0.75)
//   y(t) = y1 + (y2 - y1) * (3t^2 - 2t^3)          (exactly smoothstep)
// so uniform-x evaluation is one Newton inversion of a FIXED cubic
// (identical for every segment) followed by a smoothstep lerp.

import type { InterpolationType, TrackDto } from "@particle-editor/bridge-schema";

export type Key = { time: number; value: number };

/** Morph duration / easing tier. Feel-tunable (host pass). */
export const MORPH_MS = 180;
export const KEY_POP_MS = 150;
export const MORPH_SAMPLES = 160;
/** Key times closer than this are "the same key" (matches the
 *  drag-clamp epsilon order of magnitude used by the renderer). */
export const KEY_MATCH_EPS = 1e-4;

export function easeOutCubic(t: number): number {
  const u = 1 - t;
  return 1 - u * u * u;
}

/** Invert u = 0.75t + 0.75t^2 - 0.5t^3 on [0,1] (Newton, seed t=u;
 *  derivative >= 0.75 so 4 iterations are ample). */
function tForU(u: number): number {
  let t = u;
  for (let i = 0; i < 4; i++) {
    const f = 0.75 * t + 0.75 * t * t - 0.5 * t * t * t - u;
    const d = 0.75 + 1.5 * t - 1.5 * t * t;
    t -= f / d;
  }
  return t < 0 ? 0 : t > 1 ? 1 : t;
}

/** Evaluate the track's RENDERED y (data space) at time x. Formula-
 *  identical to the path builders; clamps outside the border keys.
 *  Step: left key's value; exactly at a key, the key's own value. */
export function sampleTrackY(
  keys: ReadonlyArray<Key>,
  interp: InterpolationType,
  x: number,
): number {
  if (keys.length === 0) return 0;
  if (x <= keys[0]!.time) return keys[0]!.value;
  const last = keys[keys.length - 1]!;
  if (x >= last.time) return last.value;
  let i = 0;
  while (keys[i + 1]!.time <= x) i++;
  const a = keys[i]!;
  const b = keys[i + 1]!;
  if (interp === "step") return a.value;
  // The while loop above advances i until keys[i+1].time > x, so b.time > a.time
  // (the half-open interval [a.time, b.time) is non-empty); duplicate-time keys
  // render as a vertical jump and can never cause a divide-by-zero here.
  const u = (x - a.time) / (b.time - a.time);
  if (interp === "linear") return a.value + (b.value - a.value) * u;
  const t = tForU(u);
  return a.value + (b.value - a.value) * (3 * t * t - 2 * t * t * t);
}

/** Shared x-grid for a (prev, next) morph pair: N+1 uniform samples
 *  UNION an epsilon pair (t-eps, t+eps) around every key time of BOTH
 *  key sets — step discontinuities render as true verticals mid-morph
 *  (user requirement); continuous shapes sample equal y at both, so
 *  the extra points are harmless. Sorted ascending, clamped to range. */
export function buildMorphGrid(
  prev: TrackDto,
  next: TrackDto,
  timeMin: number,
  timeMax: number,
  n: number = MORPH_SAMPLES,
): Float64Array {
  if (n < 1) n = 1;
  const range = timeMax - timeMin;
  const eps = range * 1e-6;
  const xs: number[] = [];
  for (let i = 0; i <= n; i++) xs.push(timeMin + (i / n) * range);
  for (const k of [...prev.keys, ...next.keys]) {
    if (k.time > timeMin && k.time < timeMax) {
      xs.push(k.time - eps, k.time, k.time + eps);
    }
  }
  xs.sort((a, b) => a - b);
  return Float64Array.from(xs);
}

/** Sample pixel-space y at each grid x (y-inverted: larger value =
 *  smaller pixel y), using the channel's OWN projection. */
export function sampleTrackPx(
  track: TrackDto,
  gridX: Float64Array,
  proj: { vMin: number; vMax: number; height: number },
): Float64Array {
  const { vMin, vMax, height } = proj;
  const span = vMax - vMin;
  const out = new Float64Array(gridX.length);
  for (let i = 0; i < gridX.length; i++) {
    const v = sampleTrackY(track.keys, track.interpolation, gridX[i]!);
    const tnorm = span > 0 ? (v - vMin) / span : 0;
    out[i] = height - Math.max(0, Math.min(1, tnorm)) * height;
  }
  return out;
}

/** Linear-resample a displayed piecewise-linear polyline (xsOld, ysOld)
 *  onto a new grid — interruption folding across grid changes. */
export function resampleOntoGrid(
  xsOld: Float64Array,
  ysOld: Float64Array,
  xsNew: Float64Array,
): Float64Array {
  if (xsOld.length === 0) return new Float64Array(xsNew.length);
  if (xsOld.length === 1) return new Float64Array(xsNew.length).fill(ysOld[0]!);
  const out = new Float64Array(xsNew.length);
  let j = 0;
  for (let i = 0; i < xsNew.length; i++) {
    const x = xsNew[i]!;
    while (j < xsOld.length - 2 && xsOld[j + 1]! < x) j++;
    const x0 = xsOld[j]!;
    const x1 = xsOld[j + 1]!;
    const y0 = ysOld[j]!;
    const y1 = ysOld[j + 1]!;
    out[i] = x1 > x0
      ? y0 + ((y1 - y0) * Math.max(0, Math.min(1, (x - x0) / (x1 - x0))))
      : y0;
  }
  return out;
}

/** Shape-change classification between consecutive TrackDto snapshots.
 *  NOTE (spec refinement): lockedTo is deliberately IGNORED — it is
 *  styling, not shape; a re-mirror always arrives as key deltas.
 *    "none"       identical keys + interpolation
 *    "moved"      same count & times (EPS), >=1 value differs
 *    "structural" anything else (count / time / interp) */
export function classifyTrackChange(
  prev: TrackDto,
  next: TrackDto,
): "none" | "moved" | "structural" {
  if (prev.interpolation !== next.interpolation) return "structural";
  if (prev.keys.length !== next.keys.length) return "structural";
  let moved = false;
  for (let i = 0; i < prev.keys.length; i++) {
    const a = prev.keys[i]!;
    const b = next.keys[i]!;
    if (Math.abs(a.time - b.time) > KEY_MATCH_EPS) return "structural";
    if (Math.abs(a.value - b.value) > KEY_MATCH_EPS) moved = true;
  }
  return moved ? "moved" : "none";
}

/** Match keys old->new by time (EPS) for the marker choreography. */
export function matchKeys(prev: TrackDto, next: TrackDto): {
  moved: Array<{ from: Key; to: Key }>;
  added: Key[];
  removed: Key[];
} {
  const moved: Array<{ from: Key; to: Key }> = [];
  const added: Key[] = [];
  const usedPrev = new Set<number>();
  for (const k of next.keys) {
    const idx = prev.keys.findIndex(
      (p, i) => !usedPrev.has(i) && Math.abs(p.time - k.time) <= KEY_MATCH_EPS,
    );
    if (idx >= 0) {
      usedPrev.add(idx);
      moved.push({ from: prev.keys[idx]!, to: k });
    } else {
      added.push(k);
    }
  }
  const removed = prev.keys.filter((_, i) => !usedPrev.has(i));
  return { moved, added, removed };
}
