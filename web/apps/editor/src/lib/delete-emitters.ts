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
// lives) runs confirmPendingDelete(bridge) on confirm, which re-validates the
// pending ids against the live tree (aborting if it changed) before delegating
// to performDelete(bridge, ids, tree).
import { create } from "zustand";
import type { Bridge, EmitterTreeDto, EmitterTreeNode } from "@particle-editor/bridge-schema";
import { useEmitterTreeStore } from "@/lib/emitter-tree";
import { useFileOpErrorStore } from "@/lib/file-op";
import { announceWhenOk } from "./status-feedback";

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

// Collapse a selection to its top-level roots: drop any id whose ancestor is
// ALSO selected. Deleting the ancestor already removes its whole subtree
// recursively host-side, so issuing a separate delete for a selected descendant
// is both redundant AND the source of the stale-positional-id footgun (deleting
// the parent shifts indices, so a later stale id resolves to the wrong node).
// Pure; unit-tested (release-audit #8).
export function collapseToRoots(ids: number[], tree: EmitterTreeDto | null): number[] {
  if (ids.length <= 1 || !tree) return [...ids];
  const selected = new Set(ids);
  const parentOf = new Map<number, number>();
  const index = (n: EmitterTreeNode, parent: number | null) => {
    if (parent !== null) parentOf.set(n.id, parent);
    n.children.forEach((c) => index(c, n.id));
  };
  tree.root.children.forEach((n) => index(n, null));
  const hasSelectedAncestor = (id: number): boolean => {
    let p = parentOf.get(id);
    while (p !== undefined) {
      if (selected.has(p)) return true;
      p = parentOf.get(p);
    }
    return false;
  };
  return ids.filter((id) => !hasSelectedAncestor(id));
}

// The single delete entry point (collapses the prior two inline copies in
// EmitterTree + MenuBar). `id` is a position index host-side, which shifts as
// siblings vanish. We first collapse the selection to roots (so a parent + its
// descendant never both issue a delete), then sort descending so the remaining
// sibling indices stay valid as earlier-deleted ones vanish.
//
// ONE batched emitters/delete-many, not one emitters/delete per root: the host
// captures a single undo entry around its whole loop, so one Ctrl+Z reverses
// the whole gesture. The per-root fan-out this replaced captured N undo
// entries, and a single Ctrl+Z restored one emitter out of N (2026-07
// audit). Batching for a single id is harmless — delete-many with one id is
// the same capture, sweep and events as delete — and matches duplicateEmitters,
// which sends duplicate-many unconditionally.
export function performDelete(bridge: Bridge, ids: number[], tree: EmitterTreeDto | null): void {
  const roots = collapseToRoots(ids, tree);
  if (roots.length === 0) return;
  const request = bridge.request({
    kind: "emitters/delete-many",
    params: { ids: [...roots].sort((a, b) => b - a) },
  });
  // StatusBar feedback once the delete resolves (F4) — a multi-root delete
  // is one gesture, so it announces once.
  announceWhenOk(
    request,
    `Deleted ${roots.length === 1 ? "emitter" : `${roots.length} emitters`} — Ctrl+Z to undo`,
  );
}

type DeleteConfirmStore = {
  // `tree` is the tree snapshot captured when the confirm opened — confirm-time
  // revalidation compares it by reference against the live tree to detect a
  // structural change that would make the captured ids stale (release-audit #8).
  pending: { ids: number[]; impact: DeleteImpact; tree: EmitterTreeDto | null } | null;
  open: (ids: number[], impact: DeleteImpact, tree: EmitterTreeDto | null) => void;
  clear: () => void;
};
export const useDeleteConfirmStore = create<DeleteConfirmStore>((set) => ({
  pending: null,
  open: (ids, impact, tree) => set({ pending: { ids, impact, tree } }),
  clear: () => set({ pending: null }),
}));

// The single entry point for all four delete call sites.
export function requestDeleteEmitters(bridge: Bridge, ids: number[]): void {
  if (ids.length === 0) return;
  const tree = useEmitterTreeStore.getState().tree;
  const impact = computeDeleteImpact(ids, tree);
  if (!readConfirmDelete() || !impact.isDestructive) {
    performDelete(bridge, ids, tree);
    return;
  }
  useDeleteConfirmStore.getState().open(ids, impact, tree);
}

// Called by <DeleteConfirmModal> when the user confirms. Re-validates against the
// CURRENT tree: if it changed since the confirm opened (an emitters/tree/changed
// landed), the captured positional ids are stale, so ABORT rather than delete the
// wrong nodes (release-audit #8). Otherwise collapse-to-roots + delete against the
// live tree. Always clears the pending state.
export function confirmPendingDelete(bridge: Bridge): void {
  const pending = useDeleteConfirmStore.getState().pending;
  const live = useEmitterTreeStore.getState().tree;
  useDeleteConfirmStore.getState().clear();
  if (!pending) return;
  if (pending.tree !== live) {
    // The tree changed since the confirm opened — the captured positional ids are
    // stale. Surface it (not a silent no-op) so the user re-selects (#8).
    useFileOpErrorStore.getState().show(
      "The emitter list changed — the delete was cancelled. Please re-select and try again.",
    );
    return;
  }
  performDelete(bridge, pending.ids, live);
}
