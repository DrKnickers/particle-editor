// skydome-seam-fix.ts — web side of the "Smooth skydome seams" preference.
// Mirrors msaa-quality.ts: localStorage owns persistence, the engine is told via
// engine/set/skydome-seam-fix on every change AND once at app mount (App.tsx), so
// the saved setting applies at startup. Default ON — the stock skydome assets bake
// a closure-meridian seam; enabling re-maps their UVs to hide it (a deliberate,
// non-asset-accurate render divergence the engine applies at dome load).

import type { Bridge } from "@particle-editor/bridge-schema";

const KEY = "alo:skydome-seam-fix";
const DEFAULT = true;

/** Read the persisted preference; defaults to ON when absent or unreadable. */
export function readSkydomeSeamFix(): boolean {
  try {
    const raw = localStorage.getItem(KEY);
    if (raw === null) return DEFAULT;
    return raw === "1" || raw === "true";
  } catch {
    return DEFAULT;
  }
}

/** Persist the preference (stored as "1"/"0"). */
export function writeSkydomeSeamFix(enabled: boolean): void {
  try {
    localStorage.setItem(KEY, enabled ? "1" : "0");
  } catch {
    /* private-mode / quota — the in-memory UI state still reflects the choice */
  }
}

/** Push the preference to the engine — fire-and-forget so a failed send (mock
 *  quirk, host teardown) never breaks the Preferences UI. The engine reloads the
 *  current dome so the toggle is live. */
export function applySkydomeSeamFix(bridge: Bridge, enabled: boolean): void {
  void bridge
    .request({ kind: "engine/set/skydome-seam-fix", params: { enabled } })
    .catch(() => {});
}
