// msaa-quality.ts — web side of the Antialiasing preference.
// Mirrors the overload-guard.ts pattern: localStorage owns persistence,
// the engine is told via engine/set/msaa-level on every change AND once
// at app mount (App.tsx), so the saved setting applies at startup.
// The hardware-gated level list is queried once from the engine
// (engine/query/msaa-levels) so the Preferences UI only shows what the
// current GPU actually supports.

import type { Bridge } from "@particle-editor/bridge-schema";

export type MsaaLevel = 0 | 2 | 4 | 8;

const VALID_LEVELS: ReadonlySet<number> = new Set([0, 2, 4, 8]);
const DEFAULT_LEVEL: MsaaLevel = 4;

const KEY = "alo:msaa-quality";

/** Read the persisted MSAA level, returning DEFAULT_LEVEL (4) when absent,
 *  corrupt, or not one of {0, 2, 4, 8}. */
export function readMsaaLevel(): MsaaLevel {
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return DEFAULT_LEVEL;
    const n = parseInt(raw, 10);
    if (!VALID_LEVELS.has(n)) return DEFAULT_LEVEL;
    return n as MsaaLevel;
  } catch {
    return DEFAULT_LEVEL;
  }
}

/** Persist `level` to localStorage. */
export function writeMsaaLevel(level: MsaaLevel): void {
  localStorage.setItem(KEY, String(level));
}

/** Push the saved MSAA level to the engine — fire-and-forget so a failed
 *  send (mock quirk, host teardown) never breaks the Preferences UI. */
export function applyMsaaLevel(bridge: Bridge, level: number): void {
  void bridge
    .request({ kind: "engine/set/msaa-level", params: { level } })
    .catch(() => {});
}

/** Query the engine for supported MSAA levels (hardware-gated).
 *  Returns `{ levels: [], current: -1 }` ("unknown") on error or a
 *  malformed response — callers must treat an empty `levels` as
 *  "couldn't determine support" and leave the saved preference intact. */
export async function queryMsaaLevels(
  bridge: Bridge,
): Promise<{ levels: number[]; current: number }> {
  try {
    const result = await bridge.request({ kind: "engine/query/msaa-levels", params: {} });
    // Guard against stubs/mocks that return {} instead of the real shape.
    if (!Array.isArray(result?.levels)) return { levels: [], current: -1 };
    return result;
  } catch {
    return { levels: [], current: -1 };
  }
}
