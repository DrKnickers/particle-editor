// curve-group-shift.ts — shared group-drag time-shift clamp (#619).
//
// A multi-key group drag shifts every selected key by one rigid dTime. The
// clamp must stop that shift before any moving key crosses a key that stays put
// — otherwise a selected key lands on an unselected key's time and the host's
// multiset insert creates a duplicate. "Stays put" = anything that is NOT a
// selected interior key: unselected keys AND selected BORDER keys (borders are
// pinned in time). Global endpoints are borders, so they're walls too.
//
// Kept pure + dependency-free so both the live-drag/preview (CurveEditor.tsx)
// and the commit (CurveEditorPanel.computeGroupMoves) can share ONE source of
// the bound and never diverge.

/** Epsilon gap kept between a moved key and the wall it stops at. Matches
 *  `computeGroupMoves` (range/10000, floored at 1e-4). */
function epsFor(sortedKeyTimes: readonly number[]): number {
  if (sortedKeyTimes.length < 2) return 1e-4;
  const span = sortedKeyTimes[sortedKeyTimes.length - 1]! - sortedKeyTimes[0]!;
  return span / 10000 || 1e-4;
}

/**
 * Clamp a rigid group time-shift so no selected INTERIOR key crosses the nearest
 * wall (an unselected key, a selected border key, or a global endpoint),
 * preserving an eps gap. Returns the clamped dTime (same sign, magnitude ≤ input).
 * Returns 0 when nothing can move (no selected interior keys).
 *
 * `sortedKeyTimes` MUST be ascending. Selected/border membership is by exact time.
 */
export function clampGroupTimeShift(
  sortedKeyTimes: readonly number[],
  selectedTimes: ReadonlySet<number>,
  borderTimes: ReadonlySet<number>,
  dTime: number,
): number {
  const isMovable = (t: number) => selectedTimes.has(t) && !borderTimes.has(t);
  const movable = sortedKeyTimes.filter(isMovable);
  if (movable.length === 0) return 0;
  const eps = epsFor(sortedKeyTimes);

  let maxRight = Infinity; // how far the selection can move right (≥ 0)
  let maxLeft = Infinity;  // how far it can move left (≥ 0)
  for (const k of movable) {
    // Nearest wall to the right / left — skip other movable keys (they move together).
    let rightWall = Infinity;
    for (const t of sortedKeyTimes) {
      if (t > k && !isMovable(t)) { rightWall = t; break; }
    }
    let leftWall = -Infinity;
    for (let i = sortedKeyTimes.length - 1; i >= 0; i--) {
      const t = sortedKeyTimes[i]!;
      if (t < k && !isMovable(t)) { leftWall = t; break; }
    }
    maxRight = Math.min(maxRight, rightWall - k - eps);
    maxLeft = Math.min(maxLeft, k - leftWall - eps);
  }
  // A key already within eps of its wall yields a negative bound — clamp to 0.
  maxRight = Math.max(0, maxRight);
  maxLeft = Math.max(0, maxLeft);
  return Math.max(-maxLeft, Math.min(maxRight, dTime));
}
