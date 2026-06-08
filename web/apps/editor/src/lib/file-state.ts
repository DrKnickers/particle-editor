// file-state.ts — Zustand atom that mirrors the host's editor-level
// file state (currentFilePath + dirty + recentFiles) plus the
// SaveChangesPrompt's pending-action slot.
//
// Phase 3 Screen 8 Batch 3.
//
// The host is the source of truth for `currentFilePath`, `dirty`, and
// the recent-files list. This module's job is:
//
//   1. Seed the atom from a one-shot snapshot + file/recent/list on mount.
//   2. Subscribe to `dirty/changed`, `recent/changed`, and
//      `engine/state/changed` events so the atom stays in lockstep.
//   3. Expose ergonomic selectors (`useFileState`) and the
//      `usePromptSaveChanges()` hook that gates destructive ops
//      (New / Open / Recent) behind the modal save-changes prompt
//      when dirty.
//
// The pending-action slot is a closure. When the SaveChangesPrompt is
// open, clicking Save / Don't Save runs the closure; Cancel discards it
// and closes the modal. This keeps the prompt decoupled from any
// specific destructive op: the caller passes "run this once the user
// has decided what to do about unsaved changes" as a function.

import { useEffect } from "react";
import { create } from "zustand";
import type { Bridge } from "@particle-editor/bridge-schema";

// ─── Atom shape ─────────────────────────────────────────────────────

type PendingAction = (() => void | Promise<void>) | null;

type FileStateStore = {
  currentFilePath: string | null;
  dirty: boolean;
  recentFiles: string[];

  /** When non-null, the SaveChangesPrompt is open and `pendingAction`
   *  fires on Save (after a successful file/save) or on Don't Save. */
  pendingAction: PendingAction;

  setCurrentFilePath: (path: string | null) => void;
  setDirty: (dirty: boolean) => void;
  setRecentFiles: (paths: string[]) => void;
  setPendingAction: (action: PendingAction) => void;
};

export const useFileStateStore = create<FileStateStore>((set) => ({
  currentFilePath: null,
  dirty: false,
  recentFiles: [],
  pendingAction: null,
  setCurrentFilePath: (currentFilePath) => set({ currentFilePath }),
  setDirty: (dirty) => set({ dirty }),
  setRecentFiles: (recentFiles) => set({ recentFiles }),
  setPendingAction: (pendingAction) => set({ pendingAction }),
}));

// ─── Selectors ─────────────────────────────────────────────────────

export function useFileState(): {
  currentFilePath: string | null;
  dirty: boolean;
  recentFiles: string[];
} {
  // Subscribe to each scalar individually so we don't return a new
  // object identity on every render — that would cause an infinite
  // re-render loop with Zustand v5 / React 19 (the snapshot diff is
  // by Object.is). Three useStore calls collapse into a single
  // subscription internally; the ergonomic destructuring at call
  // sites stays unchanged.
  const currentFilePath = useFileStateStore((s) => s.currentFilePath);
  const dirty = useFileStateStore((s) => s.dirty);
  const recentFiles = useFileStateStore((s) => s.recentFiles);
  return { currentFilePath, dirty, recentFiles };
}

/** Open / close the SaveChangesPrompt. Read by the SaveChangesPrompt
 *  itself + by the destructive-op handlers that gate behind it. */
export function usePendingAction(): PendingAction {
  return useFileStateStore((s) => s.pendingAction);
}

// ─── Event subscription hook ─────────────────────────────────────────

/** Mount once at app root. Wires the atom to the bridge's snapshot +
 *  the three relevant events. Returns nothing — the atom is the
 *  external state container.
 *
 *  Why an effect instead of a one-shot module-level subscription? The
 *  bridge instance is supplied by the App component (useMemo over
 *  `makeBridge()`), and React's strict-mode double-invokes effects.
 *  An effect with a cleanup function is the canonical way to handle
 *  both. */
export function useSeedFileState(bridge: Bridge): void {
  useEffect(() => {
    let cancelled = false;

    // 1. Seed from snapshot — the snapshot DTO carries currentFilePath
    //    + dirty as top-level fields (added in Batch 3).
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (cancelled) return;
        useFileStateStore.getState().setCurrentFilePath(s.currentFilePath);
        useFileStateStore.getState().setDirty(s.dirty);
      })
      .catch((err) => console.warn("[file-state] snapshot failed:", err));

    // 2. Seed recent files from file/recent/list.
    bridge
      .request({ kind: "file/recent/list", params: {} })
      .then((r) => {
        if (cancelled) return;
        useFileStateStore.getState().setRecentFiles(r.paths);
      })
      .catch((err) =>
        console.warn("[file-state] file/recent/list failed:", err),
      );

    // 3. Subscribe to the three event channels.
    const offDirty = bridge.on("dirty/changed", (e) => {
      useFileStateStore.getState().setDirty(e.payload.dirty);
    });
    const offRecent = bridge.on("recent/changed", (e) => {
      useFileStateStore.getState().setRecentFiles(e.payload.paths);
    });
    // The full snapshot also carries currentFilePath + dirty; pick them
    // up on every engine/state/changed broadcast so the atom stays
    // accurate even if the dedicated dirty/changed event is dropped.
    const offSnap = bridge.on("engine/state/changed", (e) => {
      useFileStateStore
        .getState()
        .setCurrentFilePath(e.payload.currentFilePath);
      useFileStateStore.getState().setDirty(e.payload.dirty);
    });

    return () => {
      cancelled = true;
      offDirty();
      offRecent();
      offSnap();
    };
  }, [bridge]);
}

// ─── Save-changes prompt orchestration ───────────────────────────────

/** Returns a function `promptSaveChanges(action)` that:
 *
 *    - If `dirty` is false: runs `action()` immediately.
 *    - If `dirty` is true: opens the SaveChangesPrompt with `action`
 *      stored as the pending closure. The prompt's buttons run the
 *      closure (Save / Don't Save) or discard it (Cancel).
 *
 *  Returns a Promise that resolves once the action either ran or was
 *  cancelled. Useful when the caller wants to chain another UI step.
 *  For fire-and-forget callers (most menu items), ignore the Promise.
 *
 *  The closure is stored in a Zustand slot rather than passed to the
 *  prompt as a prop because the prompt is mounted at app-level and
 *  driven from anywhere — see App.tsx's `<SaveChangesPrompt />`. */
export function promptSaveChanges(action: () => void | Promise<void>): void {
  const dirty = useFileStateStore.getState().dirty;
  if (!dirty) {
    void action();
    return;
  }
  useFileStateStore.getState().setPendingAction(action);
}

/** Imperative accessor — same as `useFileStateStore.getState()` but
 *  scoped to the small surface destructive-op handlers care about. */
export function getFileStateSnapshot(): {
  currentFilePath: string | null;
  dirty: boolean;
  recentFiles: string[];
} {
  const s = useFileStateStore.getState();
  return {
    currentFilePath: s.currentFilePath,
    dirty: s.dirty,
    recentFiles: s.recentFiles,
  };
}
