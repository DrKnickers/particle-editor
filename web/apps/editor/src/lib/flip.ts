// flip.ts — pure position-delta math for the emitter-tree reorder glide.
// FLIP (First-Last-Invert-Play): after a reorder re-render, each row that
// changed layout position gets an inverted translateY (back to where it WAS)
// which then transitions to zero — rows glide to their new slots instead of
// snapping. Positions are keyed by the emitter's stableId (NOT the positional
// id, which reshuffles on every structural change) and measured via
// offsetTop (layout position — immune to in-flight transforms, unlike
// getBoundingClientRect). DOM measuring/animating lives in EmitterTree's
// layout effect; this module stays pure so the math unit-tests directly.

/** stableId → offsetTop (px, content space). */
export type FlipPositions = Map<number, number>;

/** Rows present in BOTH maps whose position changed: stableId → (prev - next),
 *  i.e. the translateY that visually puts the row back where it was. Rows
 *  only in `next` (newly created) or only in `prev` (deleted) don't glide. */
export function computeFlipDeltas(
  prev: FlipPositions,
  next: FlipPositions,
): Map<number, number> {
  const deltas = new Map<number, number>();
  for (const [stableId, top] of next) {
    const was = prev.get(stableId);
    if (was !== undefined && was !== top) deltas.set(stableId, was - top);
  }
  return deltas;
}
