// tool-panel.ts — Zustand atom that drives the single-open-panel host.
//
// Phase 3 Screen 8 Batch 2 architectural call: only one modeless tool
// window may be open at a time. Opening any panel via the menu, the
// BackgroundButton, or any future trigger replaces whichever was
// previously open. Closing is by setting the atom to `null` (via the
// panel's close glyph, the Background pill toggling itself off, etc).
//
// Why one-at-a-time? Tabbed sidebar / stacked panels are bigger
// architectural changes — deferred to a later refactor if user feedback
// says one-at-a-time is too restrictive. One-at-a-time matches the
// existing BackgroundPicker pattern, keeps the layout math trivial, and
// ships in this batch with no new bridge surface.
//
// Consumers:
//   - `useSetOpenToolPanel()`         → setter (stable identity, no resubscribe)
//   - `setOpenToolPanel(id)`          → imperative setter (for handlers
//                                        outside React render, e.g. tests)

import { create } from "zustand";

// Lighting + Bloom moved out of this overlay store into lib/right-dock.ts
// (session 11): they're now a single docked pane sharing the
// Spawner's right slot, not floating overlays. Background + Ground remain
// overlays here.
export type ToolPanelId =
  | "background"
  | "ground"
  | null;

type ToolPanelStore = {
  open: ToolPanelId;
  setOpen: (id: ToolPanelId) => void;
};

export const useToolPanelStore = create<ToolPanelStore>((set) => ({
  open: null,
  setOpen: (id) => set({ open: id }),
}));

/** Get the stable setter without subscribing to the current value.
 *  Use in handlers that mutate but don't need to read. */
export function useSetOpenToolPanel(): (id: ToolPanelId) => void {
  return useToolPanelStore((s) => s.setOpen);
}

/** Imperative setter. Equivalent to `useToolPanelStore.getState().setOpen(id)`. */
export function setOpenToolPanel(id: ToolPanelId): void {
  useToolPanelStore.getState().setOpen(id);
}
