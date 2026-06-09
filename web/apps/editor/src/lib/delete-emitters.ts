// delete-emitters.ts — the proportional delete-confirm logic.
//
// Delete confirms only when destructive-and-non-obvious (the emitter has
// children → the host recursively deletes the subtree, or it is a
// multi-selection). A single childless leaf deletes immediately (it is
// trivially undoable: emitters/delete captures undo pre-mutation host-side).
// A default-on localStorage toggle ("alo:confirm-delete") governs the confirm.
//
// `bridge` is threaded in — it is a prop, not a module singleton. The confirm
// STORE never calls bridge; <DeleteConfirmModal> (mounted in App, where bridge
// lives) runs performDelete(bridge, ids) on confirm.
import { create } from "zustand";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterTreeStore } from "@/lib/emitter-tree";

export type DeleteImpact = {
  affectedCount: number; // deduped union of every selected id's subtree
  primaryName: string;   // name of the first selected emitter ("" if unknown)
  isDestructive: boolean;
};

export function computeDeleteImpact(
  ids: number[],
  tree: EmitterTreeDto | null,
): DeleteImpact {
  if (ids.length === 0) return { affectedCount: 0, primaryName: "", isDestructive: false };

  const byId = new Map<number, EmitterTreeNode>();
  const index = (n: EmitterTreeNode) => { byId.set(n.id, n); n.children.forEach(index); };
  if (tree) tree.root.children.forEach(index);

  const affected = new Set<number>();
  const addSubtree = (n: EmitterTreeNode) => {
    if (affected.has(n.id)) return;
    affected.add(n.id);
    n.children.forEach(addSubtree);
  };

  let anyHasChildren = false;
  for (const id of ids) {
    const n = byId.get(id);
    if (!n) { affected.add(id); continue; } // unknown id still counts as one
    if (n.children.length > 0) anyHasChildren = true;
    addSubtree(n);
  }

  const primary = byId.get(ids[0]);
  return {
    affectedCount: affected.size,
    primaryName: primary ? primary.name : "",
    isDestructive: ids.length > 1 || anyHasChildren,
  };
}

const CONFIRM_DELETE_KEY = "alo:confirm-delete";
export function readConfirmDelete(): boolean {
  if (typeof localStorage === "undefined") return true;
  return localStorage.getItem(CONFIRM_DELETE_KEY) !== "false"; // default true
}
export function writeConfirmDelete(value: boolean): void {
  if (typeof localStorage === "undefined") return;
  localStorage.setItem(CONFIRM_DELETE_KEY, value ? "true" : "false");
}

// The single descending-order delete loop (collapses the prior two inline
// copies in EmitterTree + MenuBar). `id` is a position index host-side, which
// shifts as siblings vanish — descending order keeps queued ids valid for the
// common case. (The parent+descendant-both-selected stale-index footgun is a
// pre-existing, separately-tracked bug; behaviour is preserved as-is here.)
export function performDelete(bridge: Bridge, ids: number[]): void {
  for (const id of [...ids].sort((a, b) => b - a)) {
    void bridge.request({ kind: "emitters/delete", params: { id } });
  }
}

type DeleteConfirmStore = {
  pending: { ids: number[]; impact: DeleteImpact } | null;
  open: (ids: number[], impact: DeleteImpact) => void;
  clear: () => void;
};
export const useDeleteConfirmStore = create<DeleteConfirmStore>((set) => ({
  pending: null,
  open: (ids, impact) => set({ pending: { ids, impact } }),
  clear: () => set({ pending: null }),
}));

// The single entry point for all four delete call sites.
export function requestDeleteEmitters(bridge: Bridge, ids: number[]): void {
  if (ids.length === 0) return;
  const impact = computeDeleteImpact(ids, useEmitterTreeStore.getState().tree);
  if (!readConfirmDelete() || !impact.isDestructive) {
    performDelete(bridge, ids);
    return;
  }
  useDeleteConfirmStore.getState().open(ids, impact);
}
