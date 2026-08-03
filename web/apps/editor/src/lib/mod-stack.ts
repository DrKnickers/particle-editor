// mod-stack.ts — global mod layer stack store + bridge wiring (Task 12).
//
// Holds the ordered content-layer stack returned by mods/list (front = highest
// priority). Components read it via useModStack(); the preview cache is keyed
// on it so a stack change automatically invalidates all cached previews.
//
// initModStack(bridge) seeds the store from mods/list — DEFERRED to the first idle
// slot after first paint (perf-audit P1a startup fan-out) — and subscribes to
// engine/state/changed to refresh whenever the host broadcasts a state transition
// (mod-switch, file-open, etc.). It returns an unsubscribe function.
//
// useSeedModStack(bridge) is the React hook wrapper — call it once in AppShell
// so it re-runs only when bridge changes (i.e. never in practice).

import { useEffect } from "react";
import { create } from "zustand";
import { invalidatePreviewCache } from "./atlas-preview-cache";
import { runWhenIdle } from "./run-after-paint";
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
/**
 * Force a mods/list refresh through the live initModStack subscription.
 * The engine/state/changed gate below keys on activeModPath (the FRONT
 * layer only — src/ModManager.h GetPrimaryLayerPath), so a stack edit
 * that keeps the front unchanged (reorder / remove / append of a
 * secondary layer) would otherwise leave this store stale. The two
 * mods/set-layers call sites (MenuBar, LoadOrderDialog) call this after
 * a successful apply. No-op before initModStack runs.
 */
export function refreshModStack(): void {
  activeRefresh?.();
}
let activeRefresh: (() => void) | null = null;

export function initModStack(bridge: Bridge): () => void {
  let latest = 0;
  let hasSeenActiveModPath = false;
  let lastSeenActiveModPath: string | null | undefined;

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

  // Seed the initial stack DEFERRED to the first idle slot after first interactive
  // paint (perf-audit P1a startup fan-out) — non-paint-critical: the stack defaults
  // to [] and the live subscription below re-seeds on any host broadcast. (Tests
  // calling initModStack directly drive this via a faked requestIdleCallback.)
  const cancelSeed = runWhenIdle(refresh);

  const off = bridge.on("engine/state/changed", (e) => {
    const activeModPath = e.payload?.activeModPath;
    if (hasSeenActiveModPath && activeModPath === lastSeenActiveModPath) return;
    hasSeenActiveModPath = true;
    lastSeenActiveModPath = activeModPath;
    refresh();
  });
  activeRefresh = refresh;
  return () => {
    latest++; // invalidate any in-flight (deferred) refresh so it bails before setState
    if (activeRefresh === refresh) activeRefresh = null;
    cancelSeed();
    off();
  };
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
