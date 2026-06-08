// Pure helpers for the EmitterTree drag/drop layer (Phase 3 Screen 4
// Batch B3). Kept module-local + side-effect-free so Vitest can exercise
// the math + validation in isolation without faking jsdom's DnD
// machinery.
//
// The four exported functions mirror the four design-locked decisions
// from `tasks/lt4_design_parking_lot.md`:
//
//   - computeDropZone        : y-position → upper/middle/lower third
//   - isDescendant           : DFS in source's subtree, used for cycle
//                              detection during reparent validation
//   - resolveReparentSlot    : both-free → "lifetime", else the free
//                              one, else null (refused)
//   - computeRootGapIndex    : drop position relative to the rendered
//                              roots list → gap index per
//                              ParticleSystem::moveEmitterToRootIndex's
//                              contract

import type { EmitterTreeNode } from "@particle-editor/bridge-schema";

export type DropZone = "above" | "onto" | "below";

/** Compute drop zone from a y-coordinate relative to a row's top edge.
 *  Upper third = "above" (reorder above), middle third = "onto"
 *  (reparent under), lower third = "below" (reorder below). The bounds
 *  use strict-less comparisons so the middle band is exclusive at both
 *  ends — matches what the user sees because the insertion-line
 *  rendering is most discoverable when the click happens deep inside
 *  the row, not on the literal pixel-edge between bands. */
export function computeDropZone(yWithinRow: number, rowHeight: number): DropZone {
  if (rowHeight <= 0) return "onto";
  const third = rowHeight / 3;
  if (yWithinRow < third) return "above";
  if (yWithinRow >= rowHeight - third) return "below";
  return "onto";
}

/** Returns true when `candidateId` appears anywhere in `source`'s
 *  subtree (including `source` itself). Used to refuse reparent drops
 *  that would create a cycle. DFS via the children array; bounded by
 *  the tree's depth, which in practice is in the single digits. */
export function isDescendant(
  source: EmitterTreeNode,
  candidateId: number,
): boolean {
  if (source.id === candidateId) return true;
  for (const c of source.children) {
    if (isDescendant(c, candidateId)) return true;
  }
  return false;
}

/** Auto-pick the reparent slot for a drop on `target`. Matches the
 *  legacy auto-pick from `EmitterList.cpp`:
 *    - both free → "lifetime"
 *    - only lifetime free → "lifetime"
 *    - only death free → "death"
 *    - both filled → null (caller refuses the drop)
 *  Uses each child's `role` field — Batch A populated this on every
 *  EmitterTreeNode. */
export function resolveReparentSlot(
  target: EmitterTreeNode,
): "lifetime" | "death" | null {
  const hasLifetime = target.children.some((c) => c.role === "lifetime");
  const hasDeath    = target.children.some((c) => c.role === "death");
  if (!hasLifetime) return "lifetime";
  if (!hasDeath)    return "death";
  return null;
}

/** Map a drop on a root row (with `targetRootIdx` its position in the
 *  rendered root list) + zone → the `rootIndex` argument for the bridge
 *  call. Mirrors `ParticleSystem::moveEmitterToRootIndex`'s gap
 *  semantics (gap K means "insert at position K", so dropping above
 *  the row at index K resolves to gap K, dropping below resolves to
 *  gap K+1). */
export function computeRootGapIndex(
  targetRootIdx: number,
  zone: "above" | "below",
): number {
  return zone === "above" ? targetRootIdx : targetRootIdx + 1;
}
