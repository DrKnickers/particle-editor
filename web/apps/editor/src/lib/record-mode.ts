// record-mode.ts — a tiny latched global flag: is this page running under --record?
//
// During --record the host drives a synthetic cursor (a React sprite in App.tsx);
// it fires NO real DOM pointer/blur events. So a Radix tooltip that focus opened
// on a toolbar trigger during record setup never receives a pointer-leave/blur to
// close it, and stays pinned open for the entire clip (observed: a stuck "Redo"
// tooltip below the disabled Redo button in every captured frame). The web learns
// it is recording when the host streams the cursor track — or, on the legacy
// literal-cursor path (hero/faith), the first `ui/cursor` message — in App.tsx's
// message listener, latches this flag, and `Tip` reads it to render its child bare
// so no tooltip can attach in the first place.
//
// Latch-only (never cleared) is deliberate: a page either boots interactive or is
// a --record run for its whole life — there is no transition back, so once set it
// stays set. A zustand store (not a plain module boolean) so `Tip` subscribers
// re-render the instant the flag flips, dropping any already-open tooltip.

import { create } from "zustand";
import { parseCursorTrackMessage } from "./record-cursor-track";
import { parseCursorMessage } from "./record-cursor-bridge";

type RecordModeStore = {
  recording: boolean;
  // Headless --record (PE_RECORD_HEADLESS): the TitleBar HIDES its window controls
  // (a close button in a tutorial clip is noise). Latched from the host's one-shot
  // ui/record-headless push. Implies `recording`. Reactive (not a ref) so the
  // TitleBar re-renders on the latch.
  headless: boolean;
  setRecording: () => void;
  setHeadless: () => void;
};

const useStore = create<RecordModeStore>((set) => ({
  recording: false,
  headless: false,
  setRecording: () => set({ recording: true }),
  setHeadless: () => set({ recording: true, headless: true }),
}));

/** Subscribe to whether this page is running under --record. */
export function useRecording(): boolean {
  return useStore((s) => s.recording);
}

/** Subscribe to whether this is a HEADLESS --record run (title strip shown). */
export function useRecordHeadless(): boolean {
  return useStore((s) => s.headless);
}

/** Non-hook read of the latch — for non-React consumers (e.g. the curve morph
 *  snaps instead of gliding under --record: the rAF-driven glide freezes when the
 *  window is minimized in headless record, holding a stale curve for seconds). */
export function isRecording(): boolean {
  return useStore.getState().recording;
}

/** Latch record mode on — called when the host's cursor track (or a legacy
 *  `ui/cursor` message) arrives in App.tsx's listener. Idempotent. */
export function markRecording(): void {
  useStore.getState().setRecording();
}

/** Latch HEADLESS record mode — called when the host's ui/record-headless push
 *  arrives. Also sets `recording`. Idempotent. */
export function markHeadless(): void {
  useStore.getState().setHeadless();
}

/** Latch record mode iff `data` is a record-cursor host message — the cursor
 *  track (target clips) or a legacy `ui/cursor` (hero/faith). App.tsx's host-message
 *  listener calls this first, so the latch is one guarded wiring point independent of
 *  the downstream track/tick/cursor handling (and unit-testable without mounting the
 *  app). A per-frame `ui/cursor-tick` is neither, so it never latches. */
export function latchRecordModeFromMessage(data: unknown): void {
  if (parseCursorTrackMessage(data) || parseCursorMessage(data)) markRecording();
}

/** Test-only: clear the latch (the store is a module singleton that survives across tests). */
export function __resetRecordModeForTests(): void {
  useStore.setState({ recording: false, headless: false });
}
