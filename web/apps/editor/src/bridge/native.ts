import type { Bridge, Request, ResponseFor, Event, EventKind, EventOf, WireMessage, RequestId } from "@particle-editor/bridge-schema";

type Pending = {
  resolve: (data: unknown) => void;
  reject: (err: Error) => void;
  timer?: ReturnType<typeof setTimeout>;
};

declare global {
  interface Window {
    // Optional fields throughout — the editor also runs in pure-browser
    // dev (no WebView2) and in --test-host mode where postMessage is
    // dropped under CDP; consumers must defensively check before use.
    // The `hostObjects.hostBridge` slot is populated only under
    // --test-host (HostWindow.cpp's AddHostObjectToScript path).
    chrome?: {
      webview?: {
        postMessage?: (s: string) => void;
        // `e.data` is the parsed JS value when the host posts via
        // `PostWebMessageAsJson`, or the raw string when it posts via
        // `PostWebMessageAsString`. Listeners must accept both.
        addEventListener?: (ev: string, h: (e: { data: unknown }) => void) => void;
        hostObjects?: {
          hostBridge?: { dispatchRequest(jsonReq: string): Promise<string> };
        };
      };
    };
  }
}

export class NativeBridge implements Bridge {
  private pending = new Map<RequestId, Pending>();
  private listeners = new Map<EventKind, Set<(e: Event) => void>>();
  private idCounter = 0;
  private disposed = false;
  // Optional per-request timeout (G12). OFF by default: several requests are
  // interactive and legitimately block for a long time — the native file
  // dialog behind file/open, emitters/import-from-file reading a chosen file —
  // so a blanket timeout would reject valid slow operations. The teardown
  // path (dispose) already reclaims the common "response never comes" case
  // (page unload mid-flight); a caller in a non-interactive context can opt
  // into a timeout as a backstop for a silently-dropped response.
  private readonly requestTimeoutMs?: number;

  constructor(opts?: { requestTimeoutMs?: number }) {
    const wv = window.chrome?.webview;
    if (!wv) {
      throw new Error("NativeBridge: chrome.webview unavailable — running in browser? Use MockBridge.");
    }
    this.requestTimeoutMs = opts?.requestTimeoutMs;
    wv.addEventListener?.("message", (e) => this.onMessage(e.data));
    // Fail every outstanding request closed on page teardown rather than
    // leaving permanently-pending promises + leaked map entries (G12).
    window.addEventListener?.("beforeunload", () => this.dispose());
  }

  /** Reject and clear every outstanding request — call on host disconnect /
   *  page teardown so callers fail closed instead of hanging forever (G12). */
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    for (const [, p] of this.pending) {
      if (p.timer !== undefined) clearTimeout(p.timer);
      p.reject(new Error("NativeBridge: disposed (host disconnected)"));
    }
    this.pending.clear();
  }

  private nextId(): RequestId {
    this.idCounter += 1;
    return `r${this.idCounter}-${Date.now().toString(36)}`;
  }

  request<R extends Request>(req: R): Promise<ResponseFor<R>> {
    const id = this.nextId();
    const envelope: WireMessage = { type: "req", id, kind: req.kind, params: req.params } as WireMessage;
    return new Promise<ResponseFor<R>>((resolve, reject) => {
      if (this.disposed) {
        reject(new Error("NativeBridge: disposed (host disconnected)"));
        return;
      }
      let timer: ReturnType<typeof setTimeout> | undefined;
      if (this.requestTimeoutMs !== undefined) {
        timer = setTimeout(() => {
          if (this.pending.delete(id)) {
            reject(new Error(`bridge request "${req.kind}" timed out after ${this.requestTimeoutMs}ms`));
          }
        }, this.requestTimeoutMs);
      }
      this.pending.set(id, { resolve: resolve as (d: unknown) => void, reject, timer });
      try {
        window.chrome!.webview!.postMessage!(JSON.stringify(envelope));
      } catch (err) {
        // stringify or postMessage threw AFTER we registered the pending
        // entry — clean it up and reject rather than leak a forever-pending
        // promise + map entry (G12).
        if (timer !== undefined) clearTimeout(timer);
        this.pending.delete(id);
        reject(err instanceof Error ? err : new Error(String(err)));
      }
    });
  }

  on<K extends EventKind>(kind: K, handler: (e: EventOf<K>) => void): () => void {
    let bucket = this.listeners.get(kind);
    if (!bucket) {
      bucket = new Set();
      this.listeners.set(kind, bucket);
    }
    bucket.add(handler as (e: Event) => void);
    return () => { bucket?.delete(handler as (e: Event) => void); };
  }

  private onMessage(raw: unknown): void {
    // `raw` is either the already-parsed JS value (host used
    // PostWebMessageAsJson — current path) or a JSON-encoded string
    // (PostWebMessageAsString). Accept both.
    let msg: WireMessage;
    if (typeof raw === "string") {
      try { msg = JSON.parse(raw) as WireMessage; } catch { return; }
    } else if (raw && typeof raw === "object") {
      msg = raw as WireMessage;
    } else {
      return;
    }
    if (msg.type === "res") {
      const p = this.pending.get(msg.id);
      if (!p) return;
      this.pending.delete(msg.id);
      if (p.timer !== undefined) clearTimeout(p.timer);
      if (msg.ok) p.resolve(msg.data);
      else p.reject(new Error(msg.error));
    } else if (msg.type === "evt") {
      const bucket = this.listeners.get(msg.kind);
      bucket?.forEach((h) => h({ kind: msg.kind, payload: msg.payload } as Event));
    }
  }
}
