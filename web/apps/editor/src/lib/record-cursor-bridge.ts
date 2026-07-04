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
 * Detect the host's one-shot `ui/record-headless` push (sent once before the
 * frame loop when `PE_RECORD_HEADLESS` is set). In headless mode the per-frame
 * ack is posted SYNCHRONOUSLY (after a `flushSync` DOM commit) instead of after
 * a double-rAF, because the host's CapturePreview forces the paint — the rAF
 * "proof of paint" wait is redundant and stalls when the window is minimized.
 * Accepts the raw string or object form, like the other record parsers.
 */
export function isRecordHeadlessMessage(data: unknown): boolean {
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
  return !!m && m.type === "ui/record-headless";
}

/**
 * Apply a record frame's DOM mutations, then post its ack — the SINGLE decision
 * point shared by the target and legacy cursor paths (App.tsx).
 *
 * - **headless**: `flushSync(applyFrame)` forces the cursor/drag DOM commit, then
 *   `post()` runs SYNCHRONOUSLY (no rAF) — the host's CapturePreview forces the
 *   paint, so the double-rAF "proof of paint" wait (which stalls minimized) is
 *   dropped. If `flushSync` THROWS, the DOM did not commit, so we **fail loud**:
 *   log and DO NOT post (a false-good frame is worse than the host's short
 *   deadline timeout).
 * - **legacy** (foreground record): apply, then post after a double-rAF — the
 *   original behavior, byte-for-byte preserved for the golden-diff baseline.
 *
 * `flushSync` is injected (not imported) so this stays React-free + unit-testable.
 */
export function commitAndAck(opts: {
  headless: boolean;
  applyFrame: () => void;
  post: () => void;
  flushSync: (fn: () => void) => void;
}): void {
  const { headless, applyFrame, post, flushSync } = opts;
  if (headless) {
    try {
      flushSync(applyFrame);
    } catch (err) {
      console.error("[record] headless flushSync failed; withholding ack", err);
      return;
    }
    post();
    return;
  }
  applyFrame();
  requestAnimationFrame(() => requestAnimationFrame(() => post()));
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
