// status-feedback.ts — one-slot transient action feedback for the StatusBar
// (design follow-ups, F4).
//
// A single latest-wins message ("Deleted "Sparks" — Ctrl+Z to undo") that the
// StatusBar renders in its OWN polite live region and fades after ~2.5s. No
// queue by design: rapid actions replace the slot and restart the timer
// (`epoch` guards the auto-clear against clearing a newer message).
//
// `announceWhenOk` is the success write path from mutation call sites:
// it announces after the bridge promise RESOLVES without an explicit
// `ok: false` (fire-and-forget sites were announcing nothing before; they
// must never announce success for a refused/failed mutation — plan-review
// finding). Rejections stay silent (the FileOpError / console paths own
// failure reporting). `fireAndReport` is the failure write path for
// fire-and-forget user actions that have no other failure surface.

import { create } from "zustand";
import { isRefusal, type Bridge, type Request, type ResponseFor } from "@particle-editor/bridge-schema";

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
      if (isRefusal(r)) return;
      useStatusFeedback.getState().announce(message);
    })
    .catch(() => {
      /* failure surfaces elsewhere; never announce success */
    });
}

/** Send a fire-and-forget user action and report its failure in the status
 *  feedback slot as "<label> failed: <why>" (plus a console warning).
 *
 *  A bridge request fails through one of TWO channels, and both are reported:
 *    1. the promise REJECTS — transport failure, host teardown, or the host
 *       answered with an `ok:false` wire envelope (an Error);
 *    2. the promise RESOLVES with an in-band refusal `{ ok: false, error? }`
 *       for the kinds whose response type carries one (see `isRefusal`).
 *  Success stays silent. Never rejects: resolves with the response on success
 *  and `undefined` on either failure, so callers can chain follow-ups. */
export function fireAndReport<R extends Request>(
  bridge: Bridge,
  req: R,
  label: string,
): Promise<ResponseFor<R> | undefined> {
  const fail = (why: string | undefined, detail: unknown) => {
    console.warn(`[bridge] ${req.kind} failed:`, detail);
    useStatusFeedback.getState().announce(why ? `${label} failed: ${why}` : `${label} failed`);
    return undefined;
  };
  let p: Promise<ResponseFor<R>>;
  try {
    p = Promise.resolve(bridge.request(req));
  } catch (err) {
    p = Promise.reject(err);
  }
  return p.then(
    (r) => (isRefusal(r) ? fail(r.error, r) : r),
    (err: unknown) => fail(err instanceof Error ? err.message : String(err), err),
  );
}

/** Test-only: reset the module singleton between cases. */
export function __resetStatusFeedbackForTests(): void {
  useStatusFeedback.setState({ message: null, epoch: 0 });
}
