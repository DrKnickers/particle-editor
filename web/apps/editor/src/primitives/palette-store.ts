// palette-store.ts — Zustand slice for the 16-slot custom color palette.
// Persisted to localStorage in browser mode; native host registry persistence
// is wired in Phase 3 Screen 8 when Lighting dialog ships.
//
// Slot shape: an RGB tuple [r, g, b] (0-255 each) or null for empty slots.
// 16 slots matches Win32 ChooseColor's custom-color array size.

import { create } from "zustand";

export type RgbColor = { r: number; g: number; b: number };

const STORAGE_KEY = "particle-editor:custom-palette";
const SLOT_COUNT = 16;

function loadFromStorage(): (RgbColor | null)[] {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (!raw) return Array(SLOT_COUNT).fill(null) as null[];
    const parsed = JSON.parse(raw) as unknown;
    if (!Array.isArray(parsed) || parsed.length !== SLOT_COUNT) {
      return Array(SLOT_COUNT).fill(null) as null[];
    }
    return parsed as (RgbColor | null)[];
  } catch {
    return Array(SLOT_COUNT).fill(null) as null[];
  }
}

function saveToStorage(slots: (RgbColor | null)[]): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(slots));
  } catch {
    // Best-effort; storage quota errors are silently ignored.
  }
}

type PaletteState = {
  slots: (RgbColor | null)[];
  /** Set a specific slot directly (overwrite). */
  setSlot: (index: number, color: RgbColor | null) => void;
  /** Add to the first empty slot. If no empty slot, replace slot 15. */
  addColor: (color: RgbColor) => void;
};

export const usePaletteStore = create<PaletteState>((set, get) => ({
  slots: loadFromStorage(),

  setSlot: (index, color) => {
    const next = [...get().slots];
    next[index] = color;
    saveToStorage(next);
    set({ slots: next });
  },

  addColor: (color) => {
    const current = get().slots;
    const emptyIdx = current.findIndex((s) => s === null);
    const idx = emptyIdx >= 0 ? emptyIdx : SLOT_COUNT - 1;
    const next = [...current];
    next[idx] = color;
    saveToStorage(next);
    set({ slots: next });
  },
}));
