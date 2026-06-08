// emitter-clipboard.ts — tracks whether the (host-owned) emitter clipboard
// has content, so the Edit → Paste menu item and the tree context-menu
// Paste item can gate their enabled state (/).
//
// The actual clipboard buffer lives in the C++ host (and the MockBridge's
// in-memory store); the React side can't read it directly. We approximate
// the legacy "Paste enabled when the clipboard format is available" gate by
// flipping this flag the first time anything is copied/cut this session
// (legacy clipboard content persists once set, so we never reset it).

import { create } from "zustand";

type EmitterClipboardStore = {
  hasContent: boolean;
  markCopied: () => void;
};

export const useEmitterClipboardStore = create<EmitterClipboardStore>((set) => ({
  hasContent: false,
  markCopied: () => set({ hasContent: true }),
}));

/** Imperative setter for non-render call sites (keyboard / menu handlers). */
export function markEmittersCopied(): void {
  useEmitterClipboardStore.getState().markCopied();
}

/** Reactive hook for the Paste item's `disabled` state. */
export function useEmitterClipboardHasContent(): boolean {
  return useEmitterClipboardStore((s) => s.hasContent);
}
