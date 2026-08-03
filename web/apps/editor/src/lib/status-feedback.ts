// status-feedback.ts — one-slot transient action feedback for the StatusBar
// (design follow-ups, F4).
//
// A single latest-wins message ("Deleted "Sparks" — Ctrl+Z to undo") that the
// StatusBar renders in its OWN polite live region and fades after ~2.5s. No
// queue by design: rapid actions replace the slot and restart the timer
// (`epoch` guards the auto-clear against clearing a newer message).
//
// `announceWhenOk` is the ONLY intended write path from mutation call sites:
// it announces after the bridge promise RESOLVES without an explicit
// `ok: false` (fire-and-forget sites were announcing nothing before; they
// must never announce success for a refused/failed mutation — plan-review
// finding). Rejections stay silent (the FileOpError / console paths own
// failure reporting).

import { create } from "zustand";

export const STATUS_FEEDBACK_CLEAR_MS = 2500;

type StatusFeedbackStore = {
  message: string | null;
  epoch: number;
  announce: (message: string) => void;
  /** Clear iff `epoch` is still current (the auto-clear timer's guard). */
  clear: (epoch: number) => void;
};

export const useStatusFeedback = create<StatusFeedbackStore>((set, get) => ({
  message: null,
  epoch: 0,
  announce: (message) => set((s) => ({ message, epoch: s.epoch + 1 })),
  clear: (epoch) => {
    if (get().epoch === epoch) set({ message: null });
  },
}));

/** Announce `message` once `p` resolves without `ok: false`. Silent on
 *  reject and on explicit refusal. */
export function announceWhenOk(p: Promise<unknown>, message: string): void {
  void p
    .then((r) => {
      if (
        r !== null &&
        typeof r === "object" &&
        "ok" in r &&
        (r as { ok: unknown }).ok === false
      ) {
        return;
      }
      useStatusFeedback.getState().announce(message);
    })
    .catch(() => {
      /* failure surfaces elsewhere; never announce success */
    });
}

/** Test-only: reset the module singleton between cases. */
export function __resetStatusFeedbackForTests(): void {
  useStatusFeedback.setState({ message: null, epoch: 0 });
}
