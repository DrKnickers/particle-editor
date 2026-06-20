// mod-stack.ts — global mod layer stack store + bridge wiring (Task 12).
//
// Holds the ordered content-layer stack returned by mods/list (front = highest
// priority). Components read it via useModStack(); the preview cache is keyed
// on it so a stack change automatically invalidates all cached previews.
//
// initModStack(bridge) seeds the store from mods/list on call and subscribes
// to engine/state/changed to refresh whenever the host broadcasts a state
// transition (mod-switch, file-open, etc.). It returns an unsubscribe function.
//
// useSeedModStack(bridge) is the React hook wrapper — call it once in AppShell
// so it re-runs only when bridge changes (i.e. never in practice).

import { useEffect } from "react";
import { create } from "zustand";
import { invalidatePreviewCache } from "./atlas-preview-cache";
import type { Bridge } from "@particle-editor/bridge-schema";

interface ModStackState {
  stack: string[];
}

const useStore = create<ModStackState>(() => ({ stack: [] }));

/** React hook: subscribe to the mod stack (re-renders on change). */
export function useModStack(): string[] {
  return useStore((s) => s.stack);
}

/** Imperative read — safe outside React (e.g. in the preview-fetch callback). */
export function getModStack(): string[] {
  return useStore.getState().stack;
}

/**
 * Seed the mod-stack store from mods/list and subscribe to engine/state/changed
 * so the store refreshes (and the preview cache is invalidated) on every state
 * broadcast. Returns an unsubscribe function.
 *
 * A monotonic sequence counter ensures that if two refreshes overlap (e.g. the
 * initial seed races with a quick engine/state/changed), only the response from
 * the most-recently-initiated request is applied.
 *
 * invalidatePreviewCache() is only called when the stack actually changes, since
 * engine/state/changed is a broad broadcast (file-open, dirty toggle, etc.) and
 * invalidating on every fire would defeat the preview cache.
 *
 * Called once at app startup (via useSeedModStack) and directly in unit tests.
 */
export function initModStack(bridge: Bridge): () => void {
  let latest = 0;

  const refresh = (): void => {
    const seq = ++latest;
    void bridge
      .request({ kind: "mods/list", params: {} })
      .then((r) => {
        if (seq !== latest) return; // a newer refresh superseded us
        const prev = useStore.getState().stack;
        const next = r.stack ?? [];
        useStore.setState({ stack: next });
        if (prev.join("|") !== next.join("|")) invalidatePreviewCache();
      })
      .catch(() => {
        // Leave existing stack in place; don't crash on a failed refresh.
      });
  };

  // Seed immediately.
  refresh();

  // Re-seed on every host-side state broadcast (file open, mod switch, etc.).
  const off = bridge.on("engine/state/changed", refresh);
  return off;
}

/**
 * React hook — call once in AppShell to wire initModStack for the app's
 * lifetime. Cleans up on unmount (bridge identity never changes in practice).
 */
export function useSeedModStack(bridge: Bridge): void {
  useEffect(() => {
    const off = initModStack(bridge);
    return off;
  }, [bridge]);
}

// ── test helpers ──────────────────────────────────────────────────────────────

/** Directly set the stack — for unit tests that don't want a real bridge. */
export function __setModStackForTests(stack: string[]): void {
  useStore.setState({ stack });
}

/** Reset store to initial state between tests. */
export function __resetModStackForTests(): void {
  useStore.setState({ stack: [] });
}
