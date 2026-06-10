// multi-drag.ts — pure helpers for multi-selection drag-reorder of the emitter
// tree. No React/DOM, so they unit-test directly; the pointer controller in
// EmitterTree.tsx calls them. Reorder-only, root-only — reparent stays a
// single-emitter-drag affordance.
import type { EmitterTreeNode } from "@particle-editor/bridge-schema";
import { computeDropZone } from "@/lib/drop-zone";

/** The selected ids that are CURRENTLY roots, in tree (top-to-bottom) order. */
export function selectedRootIdsInOrder(
  selectedIds: number[],
  rootChildren: EmitterTreeNode[],
): number[] {
  const sel = new Set(selectedIds);
  return rootChildren.filter((c) => sel.has(c.id)).map((c) => c.id);
}

/** All ids in `node`'s subtree (the node itself + every descendant,
 *  depth-first). Drives the lifted-block dimming: the whole subtree of a
 *  dragged root reads as "in hand", not just the root row. */
export function collectSubtreeIds(node: EmitterTreeNode): number[] {
  return [node.id, ...node.children.flatMap(collectSubtreeIds)];
}

/** Whether a drag begun on `grabbedId` should move the whole selection: true
 *  iff the grabbed row is a root AND part of a multi-root selection. */
export function isMultiDrag(
  grabbedId: number,
  selectedIds: number[],
  rootChildren: EmitterTreeNode[],
): boolean {
  const roots = selectedRootIdsInOrder(selectedIds, rootChildren);
  return roots.length > 1 && roots.includes(grabbedId);
}

/** Own-footprint no-op test: a CONTIGUOUS block dropped anywhere inside
 *  [first, last+1] lands exactly where it already is. Mirrors the guard in
 *  mock-state.ts::reorderManyRoots — keep in sync. */
function isOwnFootprint(blockRootIdxs: number[], gap: number): boolean {
  const first = blockRootIdxs[0]!;
  const last = blockRootIdxs[blockRootIdxs.length - 1]!;
  return last - first + 1 === blockRootIdxs.length && gap >= first && gap <= last + 1;
}

// --- Geometric gap resolution (preview polish) ---
// The drop gap is computed from a geometry snapshot captured at drag
// activation instead of live DOM hit-testing. The live-DOM approach needed a
// HOLD workaround (inserting the make-room gap reflows the list, the pointer
// lands on the pointer-events-none gap, the target resolves null, the gap
// clears, the rows snap back — flicker), which in turn made the gap "stick"
// for ~a block height of travel on tall blocks. Resolving against fixed
// geometry has no dead zones at all: every pointer Y maps to a gap or the
// footprint no-op, continuously.

/** Root-block extents at drag activation, in scroll-CONTENT space (px from
 *  the top of the scrollable content — scroll-invariant). Block k spans the
 *  root row + its whole subtree: [tops[k], bottoms[k]). */
export type RootBlockGeometry = {
  tops: number[];
  bottoms: number[];
};

/** Content-space Y of gap boundary `g`: the top of block `g`, or the bottom
 *  of the last block for the end gap (g = N). This is also where the rendered
 *  make-room spacer's top edge sits. */
export function gapContentY(geom: RootBlockGeometry, g: number): number {
  return g < geom.tops.length ? geom.tops[g]! : geom.bottoms[geom.bottoms.length - 1]!;
}

/** Total measured height of the dragged blocks — the make-room spacer's
 *  height, so the gap previews the true size of what will land there. */
export function liftedBlockHeight(geom: RootBlockGeometry, blockRootIdxs: number[]): number {
  return blockRootIdxs.reduce((sum, k) => sum + (geom.bottoms[k]! - geom.tops[k]!), 0);
}

/** Resolve the drop gap for a pointer at content-space `pointerY`, given the
 *  gap currently rendered (`currentGap`, null = none) of height `gapHeight`.
 *
 *  Two steps:
 *  1. **Un-shift**: content at/below the rendered gap is shifted down by
 *     `gapHeight`; map the pointer back into the snapshot's original space.
 *     A pointer INSIDE the gap clamps to the gap's boundary, which resolves
 *     back to the same gap — the stability fixed point (no flicker; verified
 *     by a fixed-point property test).
 *  2. **Midpoint rule**: the gap index = the number of blocks whose midpoint
 *     lies above the pointer (the classic sortable-list rule — you displace a
 *     block by crossing its midpoint).
 *
 *  Returns `"noop"` on the block's own footprint (caller clears the gap so a
 *  release leaves the order unchanged); otherwise the target gap. Never null —
 *  there are no dead zones in geometric space. */
export function resolveGapFromGeometry(
  geom: RootBlockGeometry,
  blockRootIdxs: number[],
  pointerY: number,
  currentGap: number | null,
  gapHeight: number,
): { rootIndex: number } | "noop" {
  const y = unshiftPointer(geom, pointerY, currentGap, gapHeight);
  const g = midpointGap(geom, y);
  if (isOwnFootprint(blockRootIdxs, g)) return "noop";
  return { rootIndex: g };
}

/** Map a pointer back into the snapshot's original space: content at/below the
 *  rendered gap is shifted down by `gapHeight`; a pointer inside the gap clamps
 *  to the gap boundary (the stability fixed point). No gap → identity. */
function unshiftPointer(
  geom: RootBlockGeometry,
  pointerY: number,
  currentGap: number | null,
  gapHeight: number,
): number {
  if (currentGap === null) return pointerY;
  const boundary = gapContentY(geom, currentGap);
  return pointerY > boundary ? Math.max(boundary, pointerY - gapHeight) : pointerY;
}

/** The midpoint rule: gap index = the number of root blocks whose midpoint
 *  lies above `y`. Continuous in `y`, no dead zones. */
function midpointGap(geom: RootBlockGeometry, y: number): number {
  let g = 0;
  for (let k = 0; k < geom.tops.length; k++) {
    if ((geom.tops[k]! + geom.bottoms[k]!) / 2 < y) g++;
  }
  return g;
}

/** Per-row extents at drag activation, in scroll-CONTENT space, flat (rendered)
 *  order — parallel arrays keyed by `ids[i]`. Used by the single-drag resolver
 *  to hit-test the hovered row (reparent onto detection) geometrically, so a
 *  reflowing make-room gap never corrupts the live DOM hit-test. */
export type RowGeometry = { ids: number[]; tops: number[]; bottoms: number[] };

/** Single-root drag resolution — one geometric pass that yields BOTH the
 *  multi-style reorder gap (drop above/below a root) AND a reparent target
 *  (drop onto the middle of a row). A single root is treated as a size-1 block,
 *  so the reorder gap reuses the same midpoint machinery as multi-drag.
 *
 *    - `{ kind: "reorder", rootIndex }` — make-room gap at that root gap;
 *    - `{ kind: "onto", targetId }`     — reparent under that row (its middle
 *                                         third, when `reparentOk(targetId)`);
 *    - `"noop"`                         — the root's own footprint gap.
 *
 *  `reparentOk` injects the tree-aware reparent validity (slot / cycle /
 *  same-parent) so this stays pure. Like the multi resolver it never returns a
 *  dead zone: every pointer maps to onto, a gap, or the footprint no-op. */
export function resolveSingleRootDrop(
  block: RootBlockGeometry,
  rows: RowGeometry,
  srcRootIdx: number,
  sourceId: number,
  reparentOk: (targetId: number) => boolean,
  pointerY: number,
  currentGap: number | null,
  gapHeight: number,
): { kind: "reorder"; rootIndex: number } | { kind: "onto"; targetId: number } | "noop" {
  const y = unshiftPointer(block, pointerY, currentGap, gapHeight);
  const k = rowIndexAt(rows, y);
  if (k >= 0) {
    const top = rows.tops[k]!;
    const zone = computeDropZone(y - top, rows.bottoms[k]! - top);
    const id = rows.ids[k]!;
    if (zone === "onto" && id !== sourceId && reparentOk(id)) {
      return { kind: "onto", targetId: id };
    }
  }
  const g = midpointGap(block, y);
  if (isOwnFootprint([srcRootIdx], g)) return "noop";
  return { kind: "reorder", rootIndex: g };
}

/** Index of the row whose [top, bottom) contains `y`; clamped to the first row
 *  above the list and the last row below it. -1 only for an empty list. */
function rowIndexAt(rows: RowGeometry, y: number): number {
  const n = rows.ids.length;
  if (n === 0) return -1;
  if (y < rows.tops[0]!) return 0;
  for (let i = 0; i < n; i++) {
    if (y < rows.bottoms[i]!) return i;
  }
  return n - 1;
}

/** Where the cursor chip wants to be: anchored at the pointer (+12px offset,
 *  clear of the cursor glyph), with its Y pulled `pull` of the way toward the
 *  active gap's screen-space center — the chip "flows into" the gap. No gap
 *  (or footprint no-op) → plain pointer offset. */
export function computeChipTarget(
  pointerX: number,
  pointerY: number,
  gapScreenCenterY: number | null,
  pull: number,
): { x: number; y: number } {
  const x = pointerX + 12;
  const y = pointerY + 12;
  if (gapScreenCenterY === null) return { x, y };
  return { x, y: y + (gapScreenCenterY - y) * pull };
}
