// localStorage-backed persistence for the reference-object picker's UI state:
// the faction filter, which tree groups are collapsed, and the tree scroll
// position. The Radix popover unmounts the picker on close, so without this the
// faction/expansion/scroll reset every time it reopens.
//
// Defensive by design: any read error or shape mismatch falls back to defaults
// (never throws into render), and writes are debounced (scroll fires rapidly)
// and swallow quota/disabled errors. An in-memory copy keeps read-after-write
// consistent without re-hitting localStorage on every keystroke.

const KEY = "alo:refpicker:v1";

export interface PickerState {
  /** Selected faction filter; null = "All". */
  faction: string | null;
  /** Collapsed group keys (everything else is expanded). */
  collapsed: string[];
  /** Tree scroll offset in px. */
  scrollTop: number;
}

const DEFAULTS: PickerState = { faction: null, collapsed: [], scrollTop: 0 };

function readStorage(): PickerState {
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return { ...DEFAULTS };
    const o = JSON.parse(raw) as Partial<PickerState>;
    return {
      faction: typeof o.faction === "string" ? o.faction : null,
      collapsed: Array.isArray(o.collapsed)
        ? o.collapsed.filter((x): x is string => typeof x === "string")
        : [],
      scrollTop:
        typeof o.scrollTop === "number" && Number.isFinite(o.scrollTop) && o.scrollTop >= 0
          ? o.scrollTop
          : 0,
    };
  } catch {
    return { ...DEFAULTS };
  }
}

let current: PickerState | null = null;
let writeTimer: ReturnType<typeof setTimeout> | null = null;

/** Current persisted picker state (cached in memory after the first read). */
export function loadPickerState(): PickerState {
  if (!current) current = readStorage();
  return { ...current };
}

/** Merge `patch` into the persisted state; the localStorage write is debounced. */
export function savePickerState(patch: Partial<PickerState>): void {
  const next = { ...loadPickerState(), ...patch };
  current = next;
  if (writeTimer) clearTimeout(writeTimer);
  writeTimer = setTimeout(() => {
    try {
      localStorage.setItem(KEY, JSON.stringify(next));
    } catch {
      /* quota exceeded / storage disabled -> drop silently */
    }
  }, 150);
}

/**
 * Resolve a saved faction against the factions the CURRENT content actually has:
 * keep it only if present, otherwise fall back to "All" (null). This stops a
 * saved faction (e.g. one mod's "Empire") from filtering a different mod's list
 * to empty after a mod switch.
 */
export function resolveFaction(saved: string | null, available: readonly string[]): string | null {
  return saved !== null && available.includes(saved) ? saved : null;
}

/** Test-only: drop the in-memory cache so a fresh localStorage read happens. */
export function __resetPickerStateCacheForTests(): void {
  current = null;
  if (writeTimer) {
    clearTimeout(writeTimer);
    writeTimer = null;
  }
}
