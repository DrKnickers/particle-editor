// soft-shadows.ts — web side of the "Soft shadows" preference.
// Mirrors model-shadows.ts: localStorage owns persistence, the engine is told via
// engine/set/soft-shadows on every change AND once at app mount (App.tsx), so
// the saved setting applies at startup. Default ON — the game uses soft-edged
// (blurred) stencil shadows; enabling makes the preview match in-game behaviour.

import type { Bridge } from "@particle-editor/bridge-schema";

const KEY = "alo:soft-shadows";
const DEFAULT = true;

/** Read the persisted preference; defaults to ON when absent or unreadable. */
export function readSoftShadows(): boolean {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw === null) return DEFAULT;
    return raw === "1" || raw === "true";
  } catch {
    return DEFAULT;
  }
}

/** Persist the preference (stored as "1"/"0"). */
export function writeSoftShadows(enabled: boolean): void {
  try {
    localStorage.setItem(KEY, enabled ? "1" : "0");
  } catch {
    /* private-mode / quota — the in-memory UI state still reflects the choice */
  }
}

/** Push the preference to the engine — fire-and-forget so a failed send (mock
 *  quirk, host teardown) never breaks the Preferences UI. */
export function applySoftShadows(bridge: Bridge, enabled: boolean): void {
  void bridge
    .request({ kind: "engine/set/soft-shadows", params: { enabled } })
    .catch(() => {});
}
