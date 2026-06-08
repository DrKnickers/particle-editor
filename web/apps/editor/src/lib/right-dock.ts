// right-dock.ts — Zustand store for the single shared right-dock column.
//
// The right column holds EITHER the Spawner OR the Lighting pane
// (mutually exclusive), or nothing. This replaces lib/spawner-visibility.ts:
// the Spawner used to be the only docked right column and Lighting was a
// floating `ToolPanel` overlay. session 11 promotes Lighting to a
// docked pane that shares one slot with the Spawner — opening one closes
// the other.
//
// Persists to localStorage('alo:right-dock') as "spawner" | "lighting" |
// "none". Migrates the legacy 'alo:spawner-visible' boolean on first read
// so existing users keep their column: "true" → "spawner", "false" → none.
// Default: "spawner" (matches the legacy spawner-visible=true default).
//
// The Toolbar's Spawner toggle, the View menu's Spawner (F7) + Lighting
// entries, the SpawnerPanel's X-close, and PanelLayout's column all read +
// write through this single store so they stay in sync. Mirrors the pattern
// in lib/tool-panel.ts.

import { create } from "zustand";

export type RightDock = "spawner" | "lighting" | null;
/** The non-null dock targets — what a toggle can open. */
export type DockTarget = Exclude<RightDock, null>;

const KEY = "alo:right-dock";
const LEGACY_KEY = "alo:spawner-visible";

function readInitial(): RightDock {
  if (typeof localStorage === "undefined") return "spawner";
  const v = localStorage.getItem(KEY);
  if (v === "spawner" || v === "lighting") return v;
  if (v === "none") return null;
  // No new-key value yet — migrate the legacy spawner-visible boolean so
  // a user who had the Spawner open (or closed) keeps that state.
  const legacy = localStorage.getItem(LEGACY_KEY);
  if (legacy === "false") return null;
  // legacy "true", or no legacy key at all → default to the Spawner.
  return "spawner";
}

function persist(d: RightDock): void {
  try {
    localStorage.setItem(KEY, d ?? "none");
  } catch {
    /* localStorage full / disabled — drop silently */
  }
}

type RightDockStore = {
  dock: RightDock;
  setDock: (d: RightDock) => void;
  toggle: (target: DockTarget) => void;
};

const useStore = create<RightDockStore>((set, get) => ({
  dock: readInitial(),
  setDock: (d) => {
    set({ dock: d });
    persist(d);
  },
  // Exclusive toggle: clicking the open target closes it; clicking the
  // other target swaps the column's content (the column stays open).
  toggle: (target) => {
    const next: RightDock = get().dock === target ? null : target;
    set({ dock: next });
    persist(next);
  },
}));

/** Read the current right-dock target. Subscribes the caller. */
export function useRightDock(): RightDock {
  return useStore((s) => s.dock);
}

/** Get the stable toggle function without subscribing to the value. */
export function useToggleDock(): (t: DockTarget) => void {
  return useStore((s) => s.toggle);
}

/** Imperative toggle for handlers outside React render (menu/toolbar). */
export function toggleDock(target: DockTarget): void {
  useStore.getState().toggle(target);
}

/** Imperative set (e.g. the SpawnerPanel X-close → `setDock(null)`). */
export function setDock(d: RightDock): void {
  useStore.getState().setDock(d);
}

/** Test-only: reset the module-level store back to the
 *  localStorage-derived default. A Vitest `beforeEach` clearing
 *  localStorage is not enough on its own — the store state is
 *  module-level and survives across tests. */
export function __resetRightDockForTests(): void {
  useStore.setState({ dock: readInitial() });
}

/** Test-only: the raw Zustand store, for `getState()` assertions without
 *  mounting a component. */
export function useRightDockStoreForTests() {
  return useStore;
}
