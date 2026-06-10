// emitter-reorder.ts — multi-aware duplicate + move that keep the selection
// on the affected emitters.
//
// The host owns the emitter reindex (an emitter "id" is a position index that
// shifts on every structural change), so these go through the batch bridge
// messages (emitters/move-many, emitters/duplicate-many). Each returns
// `newIds` — the moved/copied emitters' final indices, aligned to the input
// `ids` order — and we re-select those so the highlight follows the reorder
// (move) or lands on the new copies (duplicate). `bridge` is threaded in (it
// is a prop, not a module singleton).
import type { Bridge, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";

/** Re-select the emitters identified by `newIds` (aligned to `inputIds`),
 *  preserving which one is primary, and sync the host's single selection.
 *  `untouchedRemap` (oldId → newId, resolved by stable identity) carries any
 *  prior-selection members that were NOT part of the moved block, so they stay
 *  highlighted and follow the host reindex instead of being dropped. */
function applyNewSelection(
  bridge: Bridge,
  inputIds: number[],
  oldPrimary: number | null,
  newIds: number[],
  untouchedRemap: Map<number, number> = new Map(),
): void {
  // Moved block first (input order), then the surviving untouched members.
  const merged = [...newIds];
  for (const id of untouchedRemap.values()) if (!merged.includes(id)) merged.push(id);
  if (merged.length === 0) return;

  // Primary follows: the moved block by position, else the old primary's own
  // remap if it was an untouched member, else the first moved root.
  const pos = oldPrimary === null ? -1 : inputIds.indexOf(oldPrimary);
  const newPrimary =
    pos >= 0 && pos < newIds.length
      ? newIds[pos]!
      : oldPrimary !== null && untouchedRemap.has(oldPrimary)
        ? untouchedRemap.get(oldPrimary)!
        : merged[0]!;

  useEmitterSelectionStore.getState().setIds(merged, newPrimary);
  // Keep the host's single-selection (drives the inspector / get-properties)
  // in lock-step with the new primary.
  void bridge.request({ kind: "emitters/select", params: { id: newPrimary } });
}

/** Walk every non-root node of a tree DTO. */
function forEachNode(root: EmitterTreeNode, fn: (n: EmitterTreeNode) => void): void {
  for (const c of root.children) {
    fn(c);
    forEachNode(c, fn);
  }
}

/** Capture each positional id's stable identity from the CURRENT tree. */
async function captureStableIds(bridge: Bridge, ids: number[]): Promise<Map<number, number>> {
  const dto = await bridge.request({ kind: "emitters/list", params: {} });
  const out = new Map<number, number>();
  forEachNode(dto.root, (n) => {
    if (ids.includes(n.id)) out.set(n.id, n.stableId);
  });
  return out;
}

/** Re-resolve an (oldId → stableId) capture to (oldId → newId) against the
 *  FRESH tree after a reorder reindexed positional ids. Members that no longer
 *  exist (impossible for a pure reorder, but defensive) drop out. */
async function remapByStableId(
  bridge: Bridge,
  captured: Map<number, number>,
): Promise<Map<number, number>> {
  if (captured.size === 0) return new Map();
  const dto = await bridge.request({ kind: "emitters/list", params: {} });
  const byStable = new Map<number, number>();
  forEachNode(dto.root, (n) => byStable.set(n.stableId, n.id));
  const out = new Map<number, number>();
  for (const [oldId, stable] of captured) {
    const nid = byStable.get(stable);
    if (nid !== undefined) out.set(oldId, nid);
  }
  return out;
}

/** Move `ids` up/down as a block; the selection follows to the new order. */
export async function moveEmitters(
  bridge: Bridge,
  ids: number[],
  direction: "up" | "down",
): Promise<void> {
  if (ids.length === 0) return;
  const primary = useEmitterSelectionStore.getState().primary;
  const r = await bridge.request({ kind: "emitters/move-many", params: { ids, direction } });
  applyNewSelection(bridge, ids, primary, r?.newIds ?? []);
}

/** Duplicate `ids`; the selection moves to the new copies. */
export async function duplicateEmitters(bridge: Bridge, ids: number[]): Promise<void> {
  if (ids.length === 0) return;
  const primary = useEmitterSelectionStore.getState().primary;
  const r = await bridge.request({ kind: "emitters/duplicate-many", params: { ids } });
  if (r.ok) applyNewSelection(bridge, ids, primary, r.newIds);
}

/** Drag-reorder `ids` (the selected roots, in tree order) to land contiguous
 *  at gap `rootIndex`; the selection follows to the new positions. */
export async function reorderManyEmitters(
  bridge: Bridge,
  ids: number[],
  rootIndex: number,
): Promise<void> {
  if (ids.length === 0) return;
  const { ids: fullSelection, primary } = useEmitterSelectionStore.getState();
  // Prior-selection members NOT in the moved block (e.g. a selected child of an
  // unrelated root) must stay selected and follow the host reindex. Capture
  // their stable identity BEFORE the reorder reshuffles positional ids; the
  // cost is two extra list reads, paid only when the selection is mixed.
  const untouched = fullSelection.filter((id) => !ids.includes(id));
  const captured =
    untouched.length > 0 ? await captureStableIds(bridge, untouched) : new Map<number, number>();
  const r = await bridge.request({ kind: "emitters/reorder-many", params: { ids, rootIndex } });
  if (!r.ok) return;
  const untouchedRemap = await remapByStableId(bridge, captured);
  applyNewSelection(bridge, ids, primary, r.newIds, untouchedRemap);
}
