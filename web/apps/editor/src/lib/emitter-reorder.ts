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
import type { Bridge } from "@particle-editor/bridge-schema";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";

/** Re-select the emitters identified by `newIds` (aligned to `inputIds`),
 *  preserving which one is primary, and sync the host's single selection. */
function applyNewSelection(
  bridge: Bridge,
  inputIds: number[],
  oldPrimary: number | null,
  newIds: number[],
): void {
  if (newIds.length === 0) return;
  const pos = oldPrimary === null ? -1 : inputIds.indexOf(oldPrimary);
  const newPrimary = (pos >= 0 && pos < newIds.length ? newIds[pos] : newIds[0])!;
  useEmitterSelectionStore.getState().setIds(newIds, newPrimary);
  // Keep the host's single-selection (drives the inspector / get-properties)
  // in lock-step with the new primary.
  void bridge.request({ kind: "emitters/select", params: { id: newPrimary } });
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
