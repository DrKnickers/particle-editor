// boolean-pref.ts — shared localStorage backing for the persisted boolean
// render preferences (soft-shadows / model-shadows), which
// each had a byte-identical read/write pair.
//
// Stored as "1"/"0"; reads tolerate the legacy "true" encoding. localStorage
// access goes through stored-pref.ts (guarded against private-mode / quota) so
// the Preferences UI never breaks. The engine-push (`apply*`) stays per-module: `bridge.request`
// is a typed discriminated union, so the bridge `kind` must be a literal at the
// call site — it can't be parameterised here without losing type-safety.

import { readStoredPref, writeStoredPref } from "./stored-pref";

/** Read a persisted boolean pref; `defaultOn` when absent or unreadable. */
export function readBooleanPref(key: string, defaultOn: boolean): boolean {
  const raw = readStoredPref(key);
  if (raw === null) return defaultOn;
  return raw === "1" || raw === "true";
}

/** Persist a boolean pref as "1"/"0". Silently no-ops on quota/private-mode. */
export function writeBooleanPref(key: string, enabled: boolean): void {
  writeStoredPref(key, enabled ? "1" : "0");
}
