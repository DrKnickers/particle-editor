// use-engine-snapshot.ts — subscribe a component to the engine state DTO.
//
// Seeds from a one-shot `engine/state/snapshot`, then tracks every
// `engine/state/changed` broadcast; returns null until the first snapshot
// resolves. Errors are SWALLOWED (the consumer degrades to its defaults) —
// this is the toolbar-dropdown contract. Do NOT add a console.warn here: the
// picker BODIES that warn keep their own richer subscriptions; this hook is
// only for the lightweight trigger dropdowns (BackgroundDropdown /
// GroundDropdown / ReferenceObjectDropdown), which had three byte-identical
// copies of this effect (DRY audit web-screens-0).

import { useCallback, useRef, useSyncExternalStore } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";

type Listener = () => void;

type EngineSnapshotStore = {
  getSnapshot: () => EngineStateDto | undefined;
  subscribe: (listener: Listener) => () => void;
};

const stores = new WeakMap<Bridge, EngineSnapshotStore>();

function getEngineSnapshotStore(bridge: Bridge): EngineSnapshotStore {
  const existing = stores.get(bridge);
  if (existing) return existing;

  let snapshot: EngineStateDto | undefined;
  let unsubscribe: (() => void) | null = null;
  let generation = 0;
  const listeners = new Set<Listener>();

  const notify = () => {
    listeners.forEach((listener) => listener());
  };

  const start = () => {
    if (unsubscribe) return;
    const seedGeneration = generation;
    void bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((next) => {
        if (unsubscribe === null || seedGeneration !== generation) return;
        snapshot = next;
        notify();
      })
      .catch(() => {
        /* ignore */
      });
    unsubscribe = bridge.on("engine/state/changed", (e) => {
      generation += 1;
      snapshot = e.payload;
      notify();
    });
  };

  const stop = () => {
    generation += 1;
    unsubscribe?.();
    unsubscribe = null;
  };

  const store: EngineSnapshotStore = {
    getSnapshot: () => snapshot,
    subscribe(listener) {
      listeners.add(listener);
      start();
      return () => {
        listeners.delete(listener);
        if (listeners.size === 0) stop();
      };
    },
  };

  stores.set(bridge, store);
  return store;
}

export function useEngineSnapshot(bridge: Bridge): EngineStateDto | null {
  const store = getEngineSnapshotStore(bridge);
  return useSyncExternalStore(
    store.subscribe,
    () => store.getSnapshot() ?? null,
    () => null,
  );
}

/**
 * Select a narrow value from the cached engine snapshot. The hook returns
 * undefined until the first snapshot or broadcast arrives. Selectors that
 * allocate fresh objects or arrays should pass a custom isEqual function so
 * getSnapshot can keep returning the previous reference while fields match.
 */
export function useEngineField<T>(
  bridge: Bridge,
  selector: (s: EngineStateDto) => T,
  isEqual: (a: T, b: T) => boolean = Object.is,
): T | undefined {
  const store = getEngineSnapshotStore(bridge);
  const selected = useRef<{ hasValue: boolean; value: T | undefined }>({
    hasValue: false,
    value: undefined,
  });

  const getSelectedSnapshot = useCallback(() => {
    const snapshot = store.getSnapshot();
    const next = snapshot === undefined ? undefined : selector(snapshot);
    if (selected.current.hasValue) {
      const previous = selected.current.value;
      if (previous === undefined || next === undefined) {
        if (previous === next) return previous;
      } else if (isEqual(previous, next)) {
        return previous;
      }
    }
    selected.current = { hasValue: true, value: next };
    return next;
  }, [store, selector, isEqual]);

  return useSyncExternalStore(store.subscribe, getSelectedSnapshot, getSelectedSnapshot);
}
