// model-shadows.ts — web side of the "Model shadows" preference.
// Mirrors skydome-seam-fix.ts: localStorage owns persistence, the engine is told via
// engine/set/model-shadows on every change AND once at app mount (App.tsx), so
// the saved setting applies at startup. Default ON — the game casts stencil
// shadows for reference objects; enabling makes the preview match in-game behaviour.

import type { Bridge } from "@particle-editor/bridge-schema";

const KEY = "alo:model-shadows";
const DEFAULT = true;

/** Read the persisted preference; defaults to ON when absent or unreadable. */
export function readModelShadows(): boolean {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw === null) return DEFAULT;
    return raw === "1" || raw === "true";
  } catch {
    return DEFAULT;
  }
}

/** Persist the preference (stored as "1"/"0"). */
export function writeModelShadows(enabled: boolean): void {
  try {
    localStorage.setItem(KEY, enabled ? "1" : "0");
  } catch {
    /* private-mode / quota — the in-memory UI state still reflects the choice */
  }
}

/** Push the preference to the engine — fire-and-forget so a failed send (mock
 *  quirk, host teardown) never breaks the Preferences UI. */
export function applyModelShadows(bridge: Bridge, enabled: boolean): void {
  void bridge
    .request({ kind: "engine/set/model-shadows", params: { enabled } })
    .catch(() => {});
}
