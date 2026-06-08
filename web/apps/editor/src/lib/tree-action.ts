// tree-action.ts — Zustand atom for menu→tree action plumbing
// (Phase 4.1 Fix dispatch 5).
//
// The EmitterTree owns inline rename as local component state (the
// input + focus target both live in the tree itself, so lifting state
// to Zustand would just create more boilerplate). The MenuBar's
// "Rename Emitter" item lives outside the tree's React subtree, so it
// can't call `beginEdit()` directly.
//
// Solution: a single-shot request atom. The menu writes the target
// emitter id into `renameRequest`; the EmitterTree's effect picks it
// up, begins inline rename on the matching row, and consumes the
// request (sets it back to null). If the target id no longer resolves
// to a row (race), the effect silently no-ops — same defensive guard
// as the existing F2 handler.
//
// Why a dedicated atom rather than reusing `tree-context.ts`:
// `tree-context` carries dialog-target state (Rescale / Link Group /
// Set Link Group / Increment Index). Adding "rename" to its
// discriminated union would conflate "open a modal dialog" with
// "trigger inline rename inside the tree" — different code paths,
// different ownership. A tiny separate atom keeps each store
// focused.

import { create } from "zustand";

type TreeActionStore = {
  /** When non-null, the EmitterTree should begin inline rename for this
   *  emitter id. Set by `requestRename(id)`. Cleared back to null by
   *  the tree itself after consuming via `consumeRenameRequest()`. */
  renameRequest: number | null;
  requestRename: (id: number) => void;
  consumeRenameRequest: () => void;
};

export const useTreeActionStore = create<TreeActionStore>((set) => ({
  renameRequest: null,
  requestRename: (id) => set({ renameRequest: id }),
  consumeRenameRequest: () => set({ renameRequest: null }),
}));

/** Imperative request — for handlers outside React render scope
 *  (e.g. menu-item onSelect handlers). */
export function requestEmitterRename(id: number): void {
  useTreeActionStore.getState().requestRename(id);
}
