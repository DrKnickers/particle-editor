// Host → page wire-envelope parsing, shared by NativeBridge (responses + events
// over chrome.webview "message"), TestHostBridge (events over the same channel;
// responses from the host-object dispatchRequest string) and the ui/* message
// hub (which skips anything this recognises).

import type { EventKind, RequestId } from "@particle-editor/bridge-schema";

export type ParsedWireMessage =
  // `id` is undefined when the host had no request id to echo: HostBridgeProxy's
  // own error envelopes omit it, and the dispatcher's parse-error / exception
  // envelopes send `id: null`. Only the --test-host sync channel, which needs no
  // id to match its reply, can use those.
  | { type: "res"; id: RequestId | undefined; ok: true; data: unknown }
  | { type: "res"; id: RequestId | undefined; ok: false; error: string | undefined }
  | { type: "evt"; kind: EventKind; payload: unknown };

/** Coerce a host message to a plain object. The host posts with
 *  PostWebMessageAsJson (e.data is the parsed value) or PostWebMessageAsString
 *  (e.data is a JSON string); accept both. Null for anything else. */
export function coerceMessage(data: unknown): Record<string, unknown> | null {
  if (typeof data === "string") {
    try {
      data = JSON.parse(data);
    } catch {
      return null;
    }
  }
  return data !== null && typeof data === "object" && !Array.isArray(data)
    ? (data as Record<string, unknown>)
    : null;
}

/** Validate a bridge `res` / `evt` envelope. Returns null for anything else —
 *  unparseable data, a ui/* push, or an envelope with a malformed `id`/`kind`.
 *  A `res` whose `ok` is not exactly `true` is a failure, so a malformed reply
 *  fails its request instead of resolving it with garbage. */
export function parseWireMessage(raw: unknown): ParsedWireMessage | null {
  const m = coerceMessage(raw);
  if (!m) return null;
  if (m.type === "res") {
    if (m.id !== undefined && m.id !== null && typeof m.id !== "string") return null;
    const id = typeof m.id === "string" ? (m.id as RequestId) : undefined;
    if (m.ok === true) return { type: "res", id, ok: true, data: m.data };
    return { type: "res", id, ok: false, error: typeof m.error === "string" && m.error ? m.error : undefined };
  }
  if (m.type === "evt") {
    if (typeof m.kind !== "string" || m.kind === "") return null;
    return { type: "evt", kind: m.kind as EventKind, payload: m.payload };
  }
  return null;
}

/** The Error a failed request rejects with — the host's message when it sent
 *  one, otherwise one naming the request so the failure is still traceable. */
export function responseError(kind: string, error: string | undefined): Error {
  return new Error(error ?? `bridge request "${kind}" failed (the host sent no error message)`);
}
