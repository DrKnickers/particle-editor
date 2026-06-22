// curve-key-clipboard.ts — in-app clipboard for curve track keys (an-audit-finding).
//
// Legacy `CurveEditor.cpp` used a process-wide Win32 clipboard format
// (`RegisterClipboardFormat("Alamo_EmitterTrackKeys")`) so copied keys
// survived across the editor and even across applications. The web app has
// no host buffer for individual track keys, and cross-*application* paste
// was never a real workflow here — so we keep the copied keys in a small
// module-level store. Copy/cut writes the selected keys' `{time, value}`;
// paste re-adds them on the focus channel via `emitters/add-track-key`
// (which dedupes by epsilon and returns the actual inserted time, so
// pasting onto an occupied slot is safe). Cross-*track* and cross-*emitter*
// paste within the session are preserved.
//
// Interpolation is a per-TRACK property (TrackDto.interpolation), not
// per-key, so the clipboard only carries `{time, value}` — pasted keys
// adopt the destination track's interpolation, matching how the engine
// stores curves.

import { create } from "zustand";

export type CopiedCurveKey = { time: number; value: number };

type CurveKeyClipboardStore = {
  keys: CopiedCurveKey[];
  setKeys: (keys: CopiedCurveKey[]) => void;
};

// Module-local store backing the imperative setter/getter below; not exported
// (no external subscriber — the dead reactive hook was removed, DRY audit xcut-0).
const useCurveKeyClipboardStore = create<CurveKeyClipboardStore>((set) => ({
  keys: [],
  setKeys: (keys) => set({ keys }),
}));

/** Imperative setter for non-render call sites (keyboard handlers). Stores
 *  a defensive copy so later mutation of the source array can't alias it. */
export function setCurveKeysClipboard(keys: CopiedCurveKey[]): void {
  useCurveKeyClipboardStore.getState().setKeys(keys.map((k) => ({ time: k.time, value: k.value })));
}

/** Imperative reader for non-render call sites (keyboard handlers). */
export function getCurveKeysClipboard(): CopiedCurveKey[] {
  return useCurveKeyClipboardStore.getState().keys;
}
