import type {
  EmitterTreeDto,
  EmitterTreeNode,
} from "@particle-editor/bridge-schema";
import {
  computeRootGapIndex,
  isDescendant,
  resolveReparentSlot,
  type DropZone,
} from "@/lib/tree/drop-zone";
import {
  collectSubtreeIds,
  type RootBlockGeometry,
  type RowGeometry,
} from "@/lib/multi-drag";
import type { FlatRow } from "./types";

// [multi-drag] Chip-magnetize tuning: how far the chip's Y leans toward the
// active gap's center (0 = stays at the pointer, 1 = docks onto the gap), and
// the per-frame spring factor for the glide between targets.
export const CHIP_PULL = 0.6;
export const CHIP_SPRING = 0.25;

// --record only, active-drag glide (see pickFlipDuration in lib/flip.ts): the FLIP
// glide is a wall-clock CSS transition, but the capture loop grabs each frame only
// after a slow paint+PrintWindow (tens of ms of REAL time per frame). At
// At the short live-drag duration, the transition finishes between captures, so the reorder is never
// caught mid-glide and the rows snap in the recorded clip. This value is
// EMPIRICALLY TUNED to the capture cadence (≈800ms spans ~8 captured frames) — not
// an animation-feel choice; re-verify against a --record clip before changing it.

// [glide] Chip despawn: on release the chip flies into the landing gap (or
// the reparent target row) while fading; cancels/no-ops fade in place.
export const CHIP_EXIT_MS = 160;

// Validated parameters for the `emitters/drop` bridge call — the output
// of resolveDropIntent. `null` means the drop is refused.
export type DropParams =
  | { mode: "reparent"; id: number; targetId: number; slot: "lifetime" | "death" }
  | { mode: "reorder"; id: number; rootIndex: number };

/** [multi-drag] Measure every root block's extent (root row + whole subtree)
 *  in scroll-CONTENT space at drag activation. Measured, never assumed — row
 *  height varies with density and a block's height with its subtree. Returns
 *  null if any row element is missing (defensive: the drag then shows no gap
 *  and a release is a no-op). */
export function captureRootBlockGeometry(
  sc: HTMLElement | null,
  roots: EmitterTreeNode[],
): RootBlockGeometry | null {
  if (sc === null) return null;
  // content Y of a rect = rect.top - scroll viewport top + scrollTop
  const scTop = sc.getBoundingClientRect().top - sc.scrollTop;
  const tops: number[] = [];
  const bottoms: number[] = [];
  for (const r of roots) {
    const ids = collectSubtreeIds(r);
    const firstEl = sc.querySelector(`[data-emitter-id="${ids[0]}"]`);
    const lastEl = sc.querySelector(`[data-emitter-id="${ids[ids.length - 1]}"]`);
    if (firstEl === null || lastEl === null) return null;
    tops.push(firstEl.getBoundingClientRect().top - scTop);
    bottoms.push(lastEl.getBoundingClientRect().bottom - scTop);
  }
  return { tops, bottoms };
}

/** [single-drag] Measure EVERY row's extent in scroll-CONTENT space at drag
 *  activation, flat (rendered) order. The single-drag resolver hit-tests the
 *  hovered row (for reparent-onto detection) against this snapshot instead of
 *  the live DOM, so a reflowing make-room gap can't corrupt the hit-test.
 *  Returns null if any row element is missing. */
export function captureRowGeometry(
  sc: HTMLElement | null,
  rows: FlatRow[],
): RowGeometry | null {
  if (sc === null) return null;
  const scTop = sc.getBoundingClientRect().top - sc.scrollTop;
  const ids: number[] = [];
  const tops: number[] = [];
  const bottoms: number[] = [];
  for (const r of rows) {
    const el = sc.querySelector(`[data-emitter-id="${r.node.id}"]`);
    if (el === null) return null;
    const rect = el.getBoundingClientRect();
    ids.push(r.node.id);
    tops.push(rect.top - scTop);
    bottoms.push(rect.bottom - scTop);
  }
  return { ids, tops, bottoms };
}

/** Find the parent node of `id` in the tree (null for a root / not found). */
export function findParentNode(
  root: EmitterTreeNode,
  id: number,
): EmitterTreeNode | null {
  for (const c of root.children) {
    if (c.id === id) return root;
    const hit = findParentNode(c, id);
    if (hit) return hit;
  }
  return null;
}

/** Pure drop-intent resolution, shared by the pointer-drag controller.
 *  Returns the validated `emitters/drop` params, or null when the drop is
 *  refused:
 *    - self-drop / cycle (target inside source's subtree) → refused
 *    - middle third ("onto") → reparent under target (auto-pick slot;
 *      refuse if both child slots are full or target is already the
 *      source's parent)
 *    - upper/lower third → reorder, only when BOTH source and target are
 *      roots (gap semantics apply to the root list).
 *  This was an inline `resolveDropIntent`; lifted to a pure fn so
 *  the pointer-drag controller (which replaced HTML5 DnD — dead under
 *  composition hosting) can call it for any hovered target row. */
export function resolveDropIntent(
  source: EmitterTreeNode,
  target: EmitterTreeNode,
  targetRootIdx: number,
  zone: DropZone,
  tree: EmitterTreeDto | null,
  rootChildren: EmitterTreeNode[],
): DropParams | null {
  if (source.id === target.id) return null;
  if (isDescendant(source, target.id)) return null;
  if (zone === "onto") {
    const slot = resolveReparentSlot(target);
    if (slot === null) return null;
    if (tree !== null) {
      const parent = findParentNode(tree.root, source.id);
      if (parent !== null && parent.id === target.id) return null;
    }
    return { mode: "reparent", id: source.id, targetId: target.id, slot };
  }
  const sourceIsRoot = rootChildren.some((c) => c.id === source.id);
  if (!sourceIsRoot || target.role !== "root" || targetRootIdx === -1) return null;
  return {
    mode: "reorder",
    id: source.id,
    rootIndex: computeRootGapIndex(targetRootIdx, zone),
  };
}
