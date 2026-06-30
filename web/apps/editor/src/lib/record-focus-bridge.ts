/**
 * Parse a host->web ui/focus-channel push; returns the channel string (a track
 * name like "scale" or a channel id like "rotation"), or null for any other
 * message. The host sends it during --record so a scripted curve scrub shows the
 * channel it edits (the curve panel's focus is React-local and defaults to "red").
 * The host sends it via PostWebMessageAsJson (object) or PostWebMessageAsString
 * (string); accept both. NativeBridge ignores it (type is neither "res" nor
 * "evt"), so a raw listener owns it without conflict.
 */
export function parseFocusChannelMessage(data: unknown): string | null {
  let m: Record<string, unknown> | null = null;
  if (typeof data === "string") {
    try {
      m = JSON.parse(data) as Record<string, unknown>;
    } catch {
      return null;
    }
  } else if (data && typeof data === "object") {
    m = data as Record<string, unknown>;
  }
  if (!m || m.type !== "ui/focus-channel") return null;
  return typeof m.channel === "string" && m.channel.length > 0 ? m.channel : null;
}

/**
 * Parse a host->web ui/hide-panel push; returns true for that message, false
 * otherwise. The host sends it during --record so a recorded clip hides the
 * right-dock (Spawner/Lighting/Atlas) panel and gives the viewport + curve
 * editor more room. Same transport caveats as parseFocusChannelMessage.
 */
export function parseHidePanelMessage(data: unknown): boolean {
  let m: Record<string, unknown> | null = null;
  if (typeof data === "string") {
    try {
      m = JSON.parse(data) as Record<string, unknown>;
    } catch {
      return false;
    }
  } else if (data && typeof data === "object") {
    m = data as Record<string, unknown>;
  }
  return !!m && m.type === "ui/hide-panel";
}

/** Coerce a host message (object or JSON string) to a record, or null. */
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

const SHOW_PANELS = ["spawner", "lighting", "atlas"] as const;

/**
 * Parse a host->web ui/show-panel push; returns `{ panel }` (the right-dock to
 * open, or null to close) for that message, or null for any other. The host
 * sends it during --record so a clip can open the Atlas dock (or close all).
 * `panel: null` is a valid command (close) — distinct from "not this message".
 */
export function parseShowPanelMessage(
  data: unknown,
): { panel: "spawner" | "lighting" | "atlas" | null } | null {
  const m = coerceMessage(data);
  if (!m || m.type !== "ui/show-panel") return null;
  if (m.panel === null) return { panel: null };
  return typeof m.panel === "string" && (SHOW_PANELS as readonly string[]).includes(m.panel)
    ? { panel: m.panel as "spawner" | "lighting" | "atlas" }
    : null;
}

/**
 * Parse a host->web ui/select-key push; returns `{ track, time }` for that
 * message, or null otherwise. The host sends it during --record so a scripted
 * atlas edit can SELECT the curve key it is about to reassign — the same state a
 * user creates by clicking a key. That selection (via the curve panel) populates
 * atlas-context's `selection.frame`/`keyTimes`, which in turn lights the atlas
 * picker's preview box and grid highlight. Without it the --record path mutates
 * the track with no key selected, so the preview box stays empty. Same transport
 * caveats as parseFocusChannelMessage.
 */
export function parseSelectKeyMessage(data: unknown): { track: string; time: number } | null {
  const m = coerceMessage(data);
  if (!m || m.type !== "ui/select-key") return null;
  if (typeof m.track !== "string" || m.track.length === 0) return null;
  if (typeof m.time !== "number" || !Number.isFinite(m.time)) return null;
  return { track: m.track, time: m.time };
}

export type PickerWhich = "reference" | "background" | "mods";
const PICKER_WHICH = ["reference", "background", "mods"] as const;

/**
 * Parse a host->web ui/open-picker push; returns `{ which, open }` for that
 * message, or null otherwise. The host sends it during --record so a clip can
 * open (or close) one of the workspace pickers — the reference-object dropdown,
 * the background/skydome dropdown, or the Mods menu. `open` defaults to true.
 */
export function parseOpenPickerMessage(
  data: unknown,
): { which: PickerWhich; open: boolean } | null {
  const m = coerceMessage(data);
  if (!m || m.type !== "ui/open-picker") return null;
  if (typeof m.which !== "string" || !(PICKER_WHICH as readonly string[]).includes(m.which)) {
    return null;
  }
  // `open` is optional (defaults true) but, when present, must be a real boolean
  // — a string "false"/null/0 from a malformed push must reject, not open.
  if (m.open !== undefined && typeof m.open !== "boolean") return null;
  return { which: m.which as PickerWhich, open: m.open !== false };
}
