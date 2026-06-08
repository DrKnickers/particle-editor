// mod-nickname.ts — programmatic trigger surface for the Mod Nickname
// dialog (Phase 3 Screen 8 Batch 4).
//
// The real auto-trigger (on file-load with an unknown mod-data path) is
// deferred to the file-load batch. For Batch 4 the dialog is built,
// tested, and exposed via:
//
//   1. `usePromptModNickname()` — a hook returning
//      `Promise<string | null>` that opens the dialog. Caller awaits the
//      resolved nickname or null on cancel.
//   2. A `?demo=mod-nickname` route gate in App.tsx that renders the
//      dialog so design / Playwright can drive it without a menu entry.
//
// Pattern mirrors SaveChangesPrompt: a small Zustand atom owns the
// open state + a resolver closure. The dialog reads / clears the atom
// on user action; the hook returns the same resolver to the caller.

import { create } from "zustand";

type Resolver = (value: string | null) => void;

type Store = {
  open: boolean;
  /** When `open` is true, this resolver fires when the dialog dismisses
   *  (OK with text → string; Cancel / Esc → null). Always non-null
   *  while `open` is true; cleared on dismiss. */
  resolver: Resolver | null;
  setOpen: (open: boolean) => void;
  setResolver: (resolver: Resolver | null) => void;
};

export const useModNicknameStore = create<Store>((set) => ({
  open: false,
  resolver: null,
  setOpen: (open) => set({ open }),
  setResolver: (resolver) => set({ resolver }),
}));

/** Imperative trigger. Opens the Mod Nickname dialog and resolves the
 *  returned promise with the chosen nickname (or null on cancel). */
export function promptModNickname(): Promise<string | null> {
  return new Promise<string | null>((resolve) => {
    useModNicknameStore.setState({ open: true, resolver: resolve });
  });
}

/** Hook form — convenient for components that want to fire the dialog
 *  from inside a handler. The returned function is stable across
 *  renders (it lives on the Zustand store). */
export function usePromptModNickname(): () => Promise<string | null> {
  return promptModNickname;
}

// Phase 3 Screen 8 Batch 4: expose the trigger on `window.__promptModNickname`
// so Playwright specs can open the dialog without a menu entry (Batch 4
// deliberately omits one — real auto-trigger lands in the file-load
// batch). Diagnostic-only; no production code reads this.
if (typeof window !== "undefined") {
  (window as unknown as {
    __promptModNickname?: () => Promise<string | null>;
  }).__promptModNickname = promptModNickname;
}
