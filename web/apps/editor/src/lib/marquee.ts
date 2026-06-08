// marquee.ts — pure geometry helpers for the emitter-tree rubber-band
// (marquee) selection (). The DOM wiring lives in EmitterTree; these
// functions are kept pure so the intersection math is unit-testable
// (jsdom's getBoundingClientRect returns zeroes, so the rect logic can't be
// exercised through a rendered component).

export type Rect = { left: number; top: number; right: number; bottom: number };

/** Normalised rect from two corner points in any order. */
export function rectFromPoints(ax: number, ay: number, bx: number, by: number): Rect {
  return {
    left: Math.min(ax, bx),
    top: Math.min(ay, by),
    right: Math.max(ax, bx),
    bottom: Math.max(ay, by),
  };
}

/** Do two rects overlap? Edge-inclusive so a 1px sweep still hits. */
export function rectsIntersect(a: Rect, b: Rect): boolean {
  return (
    a.left <= b.right &&
    a.right >= b.left &&
    a.top <= b.bottom &&
    a.bottom >= b.top
  );
}

/** Ids of rows whose rect intersects the marquee, in the given row order. */
export function emittersInMarquee(
  rows: { id: number; rect: Rect }[],
  marquee: Rect,
): number[] {
  return rows.filter((r) => rectsIntersect(r.rect, marquee)).map((r) => r.id);
}

/** Merge the pre-marquee base selection (Ctrl-additive) with the swept ids,
 *  de-duplicated, base first then newly-swept in row order. Returns the
 *  merged id list plus the primary (the last swept id, or the last of the
 *  merged set when nothing new was swept, or null when empty). */
export function mergeMarqueeSelection(
  base: number[],
  swept: number[],
): { ids: number[]; primary: number | null } {
  const ids = [...base];
  for (const id of swept) if (!ids.includes(id)) ids.push(id);
  const primary =
    swept.length > 0
      ? swept[swept.length - 1]!
      : ids.length > 0
        ? ids[ids.length - 1]!
        : null;
  return { ids, primary };
}
