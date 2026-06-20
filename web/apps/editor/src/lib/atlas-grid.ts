// Pure atlas math, mirrors the engine: side=floor(sqrt(max(1,textureSize)))
// (EmitterInstance.cpp:698); frame k -> col=k%side, row=floor(k/side) (:638-641).
export const ATLAS_MAX_SIDE = 32;
export function gridSide(textureSize: number): number {
  const n = Number.isFinite(textureSize) ? Math.max(1, Math.floor(textureSize)) : 1;
  return Math.floor(Math.sqrt(n));
}
export function frameCount(textureSize: number): number { const s = gridSide(textureSize); return s * s; }
export function isAtlasEligible(textureSize: number): boolean { return gridSide(textureSize) >= 2; }
export function isAtlasTooLarge(textureSize: number): boolean { return gridSide(textureSize) > ATLAS_MAX_SIDE; }
export function resolveFrame(value: number, side: number): number | null {
  if (!Number.isFinite(value)) return null;
  const f = Math.floor(value);
  return f < 0 || f > side * side - 1 ? null : f;
}
export interface CellRect { left: number; top: number; width: number; height: number; }
export function cellRect(k: number, side: number, srcW: number, srcH: number): CellRect {
  const col = k % side, row = Math.floor(k / side), cw = srcW / side, ch = srcH / side;
  return { left: col * cw, top: row * ch, width: cw, height: ch };
}
export type SelectionKind = "none" | "single" | "multi-same" | "multi-diff";
// `frame` = shared floor(value) when 1 key or N share a floor(value), else null.
export function classifySelection(keyTimes: number[], frame: number | null): SelectionKind {
  if (keyTimes.length === 0) return "none";
  if (keyTimes.length === 1) return "single";
  return frame === null ? "multi-diff" : "multi-same";
}
