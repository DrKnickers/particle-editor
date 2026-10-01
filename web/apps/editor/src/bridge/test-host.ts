// TestHostBridge — Bridge implementation that routes requests through
// WebView2's host-object IPC channel (`chrome.webview.hostObjects.hostBridge`)
// instead of `chrome.webview.postMessage`.
//
// Why: Playwright drives the editor over a CDP connection
// (--remote-debugging-port=9222). WebView2 silently drops
// `chrome.webview.postMessage` calls while a CDP debugger is attached;
// host-object dispatch is on a separate
// marshalling path and is unaffected, so test traffic uses it.
//
// Scope: requests use the host-object channel; events come back over
// the standard `chrome.webview.addEventListener("message", ...)`
// channel. The CDP drop only affects page → host postMessage, not
// host → page, so events delivered via `PostWebMessageAsJson` reach
// the page normally. The verification of this is in
// `tests/bridge-native.spec.ts` (engine/state/changed test).
//
// Production builds DO NOT construct TestHostBridge — `expose.ts` only
// installs it when `chrome.webview.hostObjects.hostBridge` is present,
// which only happens in --test-host mode (HostWindow.cpp gates the
// AddHostObjectToScript call behind useTestHost).

import type {
  Bridge,
  Request,
  ResponseFor,
  Event,
  EventKind,
  EventOf,
  RequestId,
} from "@particle-editor/bridge-schema";
import { traceBridgeRequestEnd, traceBridgeRequestStart } from "@/lib/perf-trace";
import { EventHub } from "./event-hub";
import { parseWireMessage, responseError } from "./wire";

// The `window.chrome.webview` / `window.chrome.webview.hostObjects`
// types are declared globally in `./native.ts` (single source of truth).
// We just consume them here.

export class TestHostBridge implements Bridge {
  private idCounter = 0;
  private events = new EventHub<{ [K in EventKind]: EventOf<K> }>("TestHostBridge");

  constructor() {
    // Also subscribe to postMessage-delivered events. WebView2 still
    // pushes `evt` envelopes via postMessage (host → page), and that
    // direction is NOT affected by the CDP-drops-postMessage issue
    // (the drop is page → host). So events keep working over the
    // standard channel even in --test-host mode.
    const wv = window.chrome?.webview;
    if (wv?.addEventListener) {
      // Host posts via PostWebMessageAsJson — the WebView2 runtime
      // delivers `e.data` as the parsed JSON value (object/array/string),
      // NOT the raw string. Forward both shapes through onEventMessage.
      wv.addEventListener("message", (e: { data: unknown }) => this.onEventMessage(e.data));
    }
  }

  private nextId(): RequestId {
    this.idCounter += 1;
    return `t${this.idCounter}-${Date.now().toString(36)}`;
  }

  async request<R extends Request>(req: R): Promise<ResponseFor<R>> {
    const hb = window.chrome?.webview?.hostObjects?.hostBridge;
    if (!hb) {
      throw new Error(
        "TestHostBridge: chrome.webview.hostObjects.hostBridge unavailable"
      );
    }
    const envelope = {
      type: "req" as const,
      id: this.nextId(),
      kind: req.kind,
      params: req.params,
    };
    const startMs = traceBridgeRequestStart(req.kind, envelope.id, "sync");
    try {
      const resStr = await hb.dispatchRequest(JSON.stringify(envelope));
      const res = parseWireMessage(resStr);
      if (res?.type !== "res") {
        throw new Error(`TestHostBridge: malformed reply to "${req.kind}"`);
      }
      if (!res.ok) throw responseError(req.kind, res.error);
      traceBridgeRequestEnd(req.kind, envelope.id, "sync", startMs, "ok");
      return res.data as ResponseFor<R>;
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      traceBridgeRequestEnd(req.kind, envelope.id, "sync", startMs, "error", message);
      throw err;
    }
  }

  on<K extends EventKind>(kind: K, handler: (e: EventOf<K>) => void): () => void {
    return this.events.on(kind, handler);
  }

  private onEventMessage(raw: unknown): void {
    // Only forward `evt` envelopes — responses are owned by the
    // host-object dispatchRequest promise.
    //
    // The data shape depends on the host emit path:
    //   - PostWebMessageAsJson → e.data is the parsed JS value
    //   - PostWebMessageAsString → e.data is a JSON-encoded string
    // The host currently uses PostWebMessageAsJson; we accept either.
    const m = parseWireMessage(raw);
    if (m?.type !== "evt") return;
    this.events.emit(m.kind, { kind: m.kind, payload: m.payload } as Event);
  }
}
