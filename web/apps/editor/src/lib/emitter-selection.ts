// emitter-selection.ts — Zustand atom driving React-side multi-select
// state for the EmitterTree.
//
// Server-side, the BridgeDispatcher tracks only the *primary* selected
// emitter id (the row that owns focus and drives keyboard nav).
// Multi-select is purely a React concern: batch
// operations like "Set Link Group…" / "Leave Link Group" take
// `ids: number[]` so the host doesn't need a parallel selection set.
//
// Shape:
//   - `ids: number[]`   — ordered insertion. Treated as a set via the
//                         actions below; the array form is what
//                         Zustand v5 + tests need (Set isn't
//                         structurally comparable).
//   - `primary: number | null` — the focus row.
//
// Actions:
//   - `setSingle(id)`           — replace the selection with just {id}.
//                                 primary = id.
//   - `toggle(id)`              — add or remove id from `ids`. On add,
//                                 primary becomes id. On remove of
//                                 the current primary, primary moves
//                                 to the next still-selected id (or
//                                 null if the set is now empty);
//                                 removing a non-primary leaves primary
//                                 unchanged. Matches modern OS
//                                 Ctrl/Cmd+click behaviour.
//   - `range(toId, orderedIds)` — select every id between the current
//                                 primary and `toId` (inclusive) along
//                                 the rendered tree order. Updates
//                                 primary to toId.
//   - `clear()`                 — empty selection.
//   - `setIds(ids, primary)`    — test/seed escape hatch.

import { create } from "zustand";

type EmitterSelectionStore = {
  ids: number[];
  primary: number | null;
  // The shift-range pivot. Set by every NON-shift gesture (setSingle,
  // toggle-add, setIds) and held FIXED across consecutive shift-clicks so
  // a series of shift-clicks all extend from the same origin row — not
  // from the previously shift-clicked row (the moving-anchor bug).
  anchor: number | null;
  setSingle: (id: number) => void;
  toggle: (id: number) => void;
  range: (toId: number, orderedIds: number[]) => void;
  clear: () => void;
  setIds: (ids: number[], primary: number | null) => void;
};

export const useEmitterSelectionStore = create<EmitterSelectionStore>(
  (set, get) => ({
    ids: [],
    primary: null,
    anchor: null,
    setSingle: (id) => set({ ids: [id], primary: id, anchor: id }),
    toggle: (id) => {
      const { ids, primary } = get();
      const idx = ids.indexOf(id);
      if (idx === -1) {
        // Add. Becomes the new primary AND the new shift anchor.
        set({ ids: [...ids, id], primary: id, anchor: id });
      } else {
        // Remove. Primary stays unless we just removed it.
        const nextIds = ids.filter((x) => x !== id);
        let nextPrimary: number | null = primary;
        if (primary === id) {
          // Move primary to the previous id (matches the visual order),
          // falling back to null when the set is now empty.
          nextPrimary = nextIds.length > 0 ? nextIds[nextIds.length - 1]! : null;
        }
        set({ ids: nextIds, primary: nextPrimary, anchor: nextPrimary });
      }
    },
    range: (toId, orderedIds) => {
      // Pivot from the stable anchor (fall back to primary, then toId) so
      // repeated shift-clicks all extend from the same origin row. primary
      // moves to toId (the focus row); anchor is left UNTOUCHED.
      const { anchor, primary } = get();
      const pivot = anchor ?? primary;
      if (pivot === null) {
        set({ ids: [toId], primary: toId, anchor: toId });
        return;
      }
      const fromIdx = orderedIds.indexOf(pivot);
      const toIdx   = orderedIds.indexOf(toId);
      if (fromIdx === -1 || toIdx === -1) {
        // One of the endpoints isn't in the rendered tree (mid-mutation
        // race?). Fall back to a single-row selection so the UI stays
        // consistent.
        set({ ids: [toId], primary: toId, anchor: toId });
        return;
      }
      const lo = Math.min(fromIdx, toIdx);
      const hi = Math.max(fromIdx, toIdx);
      const slice = orderedIds.slice(lo, hi + 1);
      set({ ids: slice, primary: toId }); // anchor unchanged
    },
    clear: () => set({ ids: [], primary: null, anchor: null }),
    setIds: (ids, primary) => set({ ids: [...ids], primary, anchor: primary }),
  }),
);

// ─── Scalar selector hooks (Zustand v5 fresh-object rule)
//
// Subscribing to `ids` directly hands components a stable reference
// from the store; React's rendering is fine with that. We expose
// dedicated hooks for `primary` and a per-id "is selected" check so
// callers don't have to do their own membership tests.

export function useEmitterSelectionIds(): number[] {
  return useEmitterSelectionStore((s) => s.ids);
}

export function useEmitterSelectionPrimary(): number | null {
  return useEmitterSelectionStore((s) => s.primary);
}

/** Imperative escape hatch — for handlers outside React render scope
 *  (e.g. tests, context-menu handlers reading current selection at
 *  click time). */
export function getEmitterSelectionSnapshot(): {
  ids: number[];
  primary: number | null;
} {
  const s = useEmitterSelectionStore.getState();
  return { ids: [...s.ids], primary: s.primary };
}
