// curve-snap.ts — pure snap-to-grid math for the curve editor (#618).
//
// The curve canvas draws `gridCells` major divisions per axis with a finer
// minor sub-grid (`GRID_SUBDIVISIONS` per major cell). When the snap-to-grid
// toggle is on, dragged/inserted keys snap to the nearest MINOR grid stop.
//
// Kept dependency-free and side-effect-free so it unit-tests trivially and can
// be reused by both the single-key and group-drag paths.

/** Major grid divisions per axis — mirrors `gridCells` in CurveEditor.tsx. */
export const GRID_CELLS = 10;

/** Minor divisions per major cell (the faint sub-grid density). */
export const GRID_SUBDIVISIONS = 5;

/** Total minor divisions per axis (== number of snap stops minus one). */
export const GRID_MINOR_CELLS = GRID_CELLS * GRID_SUBDIVISIONS; // 50

/**
 * Snap `value` to the nearest of `cells + 1` evenly-spaced stops spanning
 * `[min, max]`. Returns `value` unchanged when the axis is degenerate
 * (`min === max`) so a zero-height/zero-width range can't divide by zero.
 *
 * Stops are `min + (i / cells) * (max - min)` for integer `i`. For any value
 * inside `[min, max]` the result stays inside the range; callers still apply
 * their own bounds/neighbour clamps afterward, so no clamping is done here.
 */
export function snapToGrid(
  value: number,
  min: number,
  max: number,
  cells: number = GRID_MINOR_CELLS,
): number {
  if (min === max || cells <= 0 || !Number.isFinite(value)) return value;
  const step = (max - min) / cells;
  return min + Math.round((value - min) / step) * step;
}
