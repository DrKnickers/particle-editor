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

/** Pick the FLIP transition duration (ms). Kept pure so the record-mode policy is
 *  unit-tested without a DOM.
 *
 *  The glide is a wall-clock CSS transition. Under `--record` the capture loop
 *  grabs each frame only after a slow paint+PrintWindow (tens of ms of REAL time),
 *  so a normal-length transition finishes between grabs and the reorder is never
 *  caught mid-glide (rows snap in the clip). Only the ACTIVE-DRAG glide gets the
 *  long `recordDragMs` (empirically tuned to that capture cadence) so it spans
 *  many captures. The post-drop SETTLE defaults to `settleMs` even under record —
 *  a long settle would out-last the ~200ms drag-chip despawn, so the chip would
 *  vanish while rows were still gliding; matching the chip is both correct and
 *  enough. A consumer that ALSO stretches its chip despawn under record can opt
 *  the settle in via `recordSettleMs` (the same finishes-between-captures math
 *  applies to the commit reflow — a 200ms settle snaps in the clip too). */
export function pickFlipDuration(
  recording: boolean,
  dragging: boolean,
  d: { dragMs: number; settleMs: number; recordDragMs: number; recordSettleMs?: number },
): number {
  if (recording) return dragging ? d.recordDragMs : (d.recordSettleMs ?? d.settleMs);
  return dragging ? d.dragMs : d.settleMs;
}
