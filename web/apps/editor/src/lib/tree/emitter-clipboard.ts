// emitter-clipboard.ts — tracks whether the (host-owned) emitter clipboard
// has content, so the Edit → Paste menu item and the tree context-menu
// Paste item can gate their enabled state.
//
// The actual clipboard buffer lives in the C++ host (and the MockBridge's
// in-memory store); the React side can't read it directly. Copy / cut flip
// this flag immediately (markEmittersCopied), and useSeedEmitterClipboard
// also turns it on from the host's `emitterClipboardHasContent` engine
// snapshot field: the host buffer outlives the page, so a reloaded page
// (WebView2 crash recovery, ErrorBoundary Reload) must not start with Paste
// disabled. Legacy clipboard content persists once set, so the flag never
// resets — the seed only ever turns it on, which keeps a snapshot answered
// before a copy from clearing the flag that copy just set.

import { useEffect } from "react";
import { create } from "zustand";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { useEngineField } from "@/lib/use-engine-snapshot";

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

/** Mount once at app root. Follows the host's `emitterClipboardHasContent`
 *  through the shared engine-snapshot subscription (no extra bridge
 *  request), so the flag survives a page reload the way the host buffer
 *  does. */
export function useSeedEmitterClipboard(bridge: Bridge): void {
  const hostHasContent = useEngineField(bridge, selectHostHasContent);
  useEffect(() => {
    if (hostHasContent === true) markEmittersCopied();
  }, [hostHasContent]);
}

const selectHostHasContent = (s: EngineStateDto) => s.emitterClipboardHasContent;
