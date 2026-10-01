// stored-pref.ts — guarded localStorage access for the web-owned preferences
// (theme, overload guard, MSAA, the boolean render prefs). localStorage can
// throw (private mode, quota, a disabled storage partition); a preference read
// or write must never break the UI that asked for it.

/** The stored string for `key`, or null when absent or unreadable. */
export function readStoredPref(key: string): string | null {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

/** Persist `value` under `key`. Silently no-ops on quota/private-mode. */
export function writeStoredPref(key: string, value: string): void {
  try {
    localStorage.setItem(key, value);
  } catch {
    /* private-mode / quota — the in-memory UI state still reflects the choice */
  }
}
