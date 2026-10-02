import type { TrackDto } from "@particle-editor/bridge-schema";
import { clampGroupTimeShift } from "./curve-group-shift";

/** Per-track y-axis range used by the curve editor's multi-channel overlay. */
export function valueRangeForTrack(track: TrackDto): { min: number; max: number } {
  switch (track.name) {
    case "red":
    case "green":
    case "blue":
    case "alpha":
      return { min: 0, max: 1 };
    case "scale": {
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "index": {
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "rotationSpeed": {
      let min = 0;
      let max = 1;
      for (const k of track.keys) {
        if (k.value < min) min = k.value;
        if (k.value > max) max = k.value;
      }
      return { min, max };
    }
  }
}

/** Per-key result of shifting a multi-selection by (dTime, dValue). */
export function computeGroupMoves(
  trackKeys: ReadonlyArray<{ time: number; value: number }>,
  selectedTimes: ReadonlySet<number>,
  borderTimes: ReadonlySet<number>,
  dTime: number,
  dValue: number,
  bounds: { min: number; max: number },
): Array<{ oldTime: number; newTime: number; newValue: number }> {
  if (trackKeys.length === 0) return [];
  const clampedDTime = clampGroupTimeShift(
    trackKeys.map((k) => k.time), selectedTimes, borderTimes, dTime,
  );
  const out: Array<{ oldTime: number; newTime: number; newValue: number }> = [];
  for (const k of trackKeys) {
    if (!selectedTimes.has(k.time)) continue;
    const isBorder = borderTimes.has(k.time);
    const nt = isBorder ? k.time : k.time + clampedDTime;
    const nv = Math.min(bounds.max, Math.max(bounds.min, k.value + dValue));
    out.push({ oldTime: k.time, newTime: nt, newValue: nv });
  }
  return out;
}
