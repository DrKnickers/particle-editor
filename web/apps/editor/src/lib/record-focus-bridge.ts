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
