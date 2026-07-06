export type CursorElementRef =
  | `curve-key:${string}:${string}`
  | `atlas-tile:${string}`
  | `channel-row:${string}`
  | `testid:${string}`;

export type CursorTarget =
  | { kind: "element"; ref: CursorElementRef }
  | { kind: "point"; x: number; y: number };

export interface RecordCursorKey {
  t: number;
  vis: boolean;
  press: boolean;
  /** Opt-in: a press on this key also dispatches real click/focus on the target
   *  (record-cursor-activate). Absent/false keeps the legacy pointer-events-only
   *  behavior, so existing clips are untouched. */
  activate: boolean;
  target: CursorTarget;
}

export interface RecordCursorTick {
  t: number;
  frame: number;
}

function coerceMessage(data: unknown): Record<string, unknown> | null {
  if (typeof data === "string") {
    try {
      return JSON.parse(data) as Record<string, unknown>;
    } catch {
      return null;
    }
  }
  return data && typeof data === "object" ? (data as Record<string, unknown>) : null;
}

function isFiniteNumber(value: unknown): value is number {
  return typeof value === "number" && Number.isFinite(value);
}

function parseElementRef(ref: string): CursorElementRef | null {
  // testid: the id is a free-form data-testid value that may itself contain ':',
  // so match by prefix (not a ":"-split count) and keep the whole remainder.
  if (ref.startsWith("testid:") && ref.length > "testid:".length) {
    return ref as CursorElementRef;
  }
  const parts = ref.split(":");
  if (parts[0] === "curve-key" && parts.length === 3 && parts[1] && parts[2]) {
    return ref as CursorElementRef;
  }
  if (parts[0] === "atlas-tile" && parts.length === 2 && parts[1]) {
    return ref as CursorElementRef;
  }
  if (parts[0] === "channel-row" && parts.length === 2 && parts[1]) {
    return ref as CursorElementRef;
  }
  return null;
}

function parseTarget(value: unknown): CursorTarget | null {
  if (!value || typeof value !== "object") return null;
  const target = value as Record<string, unknown>;
  if (target.kind === "point") {
    return isFiniteNumber(target.x) && isFiniteNumber(target.y)
      ? { kind: "point", x: target.x, y: target.y }
      : null;
  }
  if (target.kind === "element" && typeof target.ref === "string") {
    const ref = parseElementRef(target.ref);
    return ref ? { kind: "element", ref } : null;
  }
  return null;
}

function parseKey(value: unknown): RecordCursorKey | null {
  if (!value || typeof value !== "object") return null;
  const key = value as Record<string, unknown>;
  if (!isFiniteNumber(key.t)) return null;
  if (typeof key.vis !== "boolean" || typeof key.press !== "boolean") return null;
  if (key.activate !== undefined && typeof key.activate !== "boolean") return null;
  const target = parseTarget(key.target);
  return target ? { t: key.t, vis: key.vis, press: key.press, activate: key.activate === true, target } : null;
}

export function parseCursorTrackMessage(data: unknown): RecordCursorKey[] | null {
  const m = coerceMessage(data);
  if (!m || m.type !== "ui/cursor-track" || !Array.isArray(m.keys) || m.keys.length === 0) {
    return null;
  }
  const keys: RecordCursorKey[] = [];
  for (const rawKey of m.keys) {
    const key = parseKey(rawKey);
    if (!key) return null;
    keys.push(key);
  }
  return keys;
}

export function parseCursorTickMessage(data: unknown): RecordCursorTick | null {
  const m = coerceMessage(data);
  if (!m || m.type !== "ui/cursor-tick") return null;
  return isFiniteNumber(m.t) && isFiniteNumber(m.frame) ? { t: m.t, frame: m.frame } : null;
}
