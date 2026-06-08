// tree-context.ts — Zustand atom driving the EmitterTree right-click
// context-menu dialog flow (Phase 3 Screen 4 Batch B1).
//
// The context menu items that open a modal (Increment Index / Rescale
// Emitter / Link Group Settings / Set Link Group) park their target id
// here so the modal mounted at App level can pick it up. The atom is
// reset to `{ open: null }` on dialog close, which also acts as the
// cancel path for any of the dialogs.
//
// Why a single atom rather than four booleans: only one of the dialogs
// can be open at a time (Radix Dialog steals focus + locks scrolling,
// mounting more than one is a UX bug), and the target id + the
// open-which-dialog choice are inherently coupled. A single
// discriminated-union atom makes the "open dialog X for emitter Y"
// transition atomic.
//
// Note: "rename" was removed in Batch C — inline rename (F2 / double-
// click / context-menu Rename) is now handled by local state in the
// EmitterTree component, not via a modal. The `RenameEmitterDialog`
// component was deleted alongside this change.

import { create } from "zustand";

export type TreeContextDialog =
  | "increment"
  | "rescale"
  | "link-group"
  | "set-link-group"
  | null;

type TreeContextStore = {
  open: TreeContextDialog;
  targetEmitterId: number | null;
  /** Only set when `open === "link-group"` — the link group whose
   *  exempt set the dialog edits. Reset to null on close. */
  targetLinkGroupId: number | null;
  /** Open a dialog with a specific target. Closes any currently-open
   *  dialog (only one of the four can be open at a time). */
  openDialog: (
    open: Exclude<TreeContextDialog, null>,
    emitterId: number,
    linkGroupId?: number,
  ) => void;
  /** Close whichever dialog is open. */
  close: () => void;
};

export const useTreeContextStore = create<TreeContextStore>((set) => ({
  open: null,
  targetEmitterId: null,
  targetLinkGroupId: null,
  openDialog: (open, emitterId, linkGroupId) =>
    set({
      open,
      targetEmitterId: emitterId,
      targetLinkGroupId: linkGroupId ?? null,
    }),
  close: () => set({ open: null, targetEmitterId: null, targetLinkGroupId: null }),
}));

/** Imperative open — for handlers outside a React render (e.g. tests). */
export function openTreeContextDialog(
  open: Exclude<TreeContextDialog, null>,
  emitterId: number,
  linkGroupId?: number,
): void {
  useTreeContextStore.getState().openDialog(open, emitterId, linkGroupId);
}

/** Imperative close — symmetric with the open helper. */
export function closeTreeContextDialog(): void {
  useTreeContextStore.getState().close();
}
