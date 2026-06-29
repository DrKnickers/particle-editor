export interface CursorMessage {
  x: number;
  y: number;
  visible: boolean;
  pressed: boolean;
}

/**
 * Parse a host->web ui/cursor push; returns null for any other message. The host
 * sends it via PostWebMessageAsJson (object) or PostWebMessageAsString (string);
 * accept both. NativeBridge ignores it (type is neither "res" nor "evt"), so this
 * raw listener owns it without conflict.
 */
export function parseCursorMessage(data: unknown): CursorMessage | null {
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
  if (!m || m.type !== "ui/cursor") return null;
  return {
    x: Number(m.x) || 0,
    y: Number(m.y) || 0,
    visible: Boolean(m.visible),
    pressed: Boolean(m.pressed),
  };
}

/**
 * Acknowledge that frame `frame`'s state has been applied + painted. Call AFTER a
 * double-rAF so the host captures a committed composite (mirrors app/ready). Sent
 * as a JSON string to match how the host's OnWebMessage reads bridge messages.
 */
export function postFrameAcked(frame: number): void {
  const wv = (window as unknown as { chrome?: { webview?: { postMessage(m: unknown): void } } }).chrome?.webview;
  wv?.postMessage(JSON.stringify({ type: "ui/frame-acked", frame }));
}
