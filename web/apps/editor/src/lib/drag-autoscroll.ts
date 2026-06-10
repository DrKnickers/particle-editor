// drag-autoscroll.ts —. Pure decision for how fast to scroll a list
// while a drag hovers near its top/bottom edge. The emitter-tree reorder drag
// (EmitterTree.tsx) runs a requestAnimationFrame loop that adds this delta to
// the scroll container's scrollTop each frame.
//
// Kept pure (no DOM) so it's unit-testable under jsdom, which can't exercise
// real layout or scrolling — the live behaviour is verified in the browser.

export type AutoscrollOpts = {
  /** Height in px of the hot band at each edge that triggers scrolling. */
  zone?: number;
  /** Scroll speed in px/frame at the very edge (ramps down to 0 at the
   *  inner boundary of the zone). */
  maxSpeed?: number;
};

/**
 * px to scroll this frame given the pointer's viewport-Y and the scroll
 * container's top/bottom (viewport coords). Returns 0 when the pointer is
 * outside both edge zones; a negative value to scroll up (near the top) and a
 * positive value to scroll down (near the bottom). Speed ramps linearly from 0
 * at the inner zone boundary to ±maxSpeed at (and past) the edge.
 */
export function computeAutoscrollDelta(
  pointerY: number,
  rect: { top: number; bottom: number },
  opts: AutoscrollOpts = {},
): number {
  const zone = opts.zone ?? 28;
  const maxSpeed = opts.maxSpeed ?? 12;

  const distFromTop = pointerY - rect.top;
  const distFromBottom = rect.bottom - pointerY;

  // In a viewport shorter than 2×zone the two edge zones overlap. Tie-break by
  // the NEARER edge so dragging toward the bottom scrolls DOWN — testing the top
  // first unconditionally made the bottom branch dead on a short panel (the
  // pointer always sat within `zone` of the top), so the end-of-list gap was
  // unreachable by autoscroll. On a tall viewport the zones don't overlap and
  // this is identical to checking each edge independently.
  if (distFromTop < zone && distFromTop <= distFromBottom) {
    const intensity = Math.min(1, Math.max(0, (zone - distFromTop) / zone));
    return -maxSpeed * intensity;
  }

  if (distFromBottom < zone) {
    const intensity = Math.min(1, Math.max(0, (zone - distFromBottom) / zone));
    return maxSpeed * intensity;
  }

  return 0;
}
