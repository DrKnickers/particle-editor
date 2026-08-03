// NativeBridge error/teardown-path tests — complements native.test.ts (G12
// request-lifecycle). This file drives the *incoming* message surface
// (onMessage) plus the teardown branches native.test.ts leaves uncovered:
// malformed/unparseable payloads, unknown/duplicate response ids, host error
// replies, event fan-out before/after (un)subscribe, beforeunload-driven
// dispose, double-dispose, and timer reclamation on dispose/error-reply.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { NativeBridge } from "../native";

type MessageHandler = (e: { data: unknown }) => void;

let messageHandler: MessageHandler | undefined;
let posted: string[];

// Install a fake chrome.webview that CAPTURES the "message" handler so tests
// can inject host→web traffic, and records outgoing postMessage payloads so
// tests can recover the auto-generated request id.
function installWebview(postMessage: (s: string) => void = (s) => posted.push(s)) {
  messageHandler = undefined;
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      postMessage,
      addEventListener: (ev: string, h: MessageHandler) => {
        if (ev === "message") messageHandler = h;
      },
    },
  };
}

/** Inject a host→web message exactly as WebView2 would deliver it. */
function deliver(data: unknown): void {
  expect(messageHandler).toBeTypeOf("function");
  messageHandler!({ data });
}

/** The wire id of the n-th request this test posted. */
function postedId(n = 0): string {
  return (JSON.parse(posted[n]!) as { id: string }).id;
}

function pendingSize(b: NativeBridge): number {
  return (b as unknown as { pending: Map<unknown, unknown> }).pending.size;
}

beforeEach(() => {
  posted = [];
  installWebview();
});

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
  vi.useRealTimers();
});

describe("NativeBridge construction", () => {
  it("throws a MockBridge-pointing error when chrome.webview is unavailable", () => {
    delete (window as unknown as { chrome?: unknown }).chrome;
    expect(() => new NativeBridge()).toThrow(/chrome\.webview unavailable.*MockBridge/);
    // chrome present but webview missing is the same branch.
    (window as unknown as { chrome: unknown }).chrome = {};
    expect(() => new NativeBridge()).toThrow(/chrome\.webview unavailable/);
  });
});

describe("NativeBridge incoming responses", () => {
  it("resolves a pending request from an already-parsed object (PostWebMessageAsJson)", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    deliver({ type: "res", id: postedId(), ok: true, data: { root: [] } });
    await expect(p).resolves.toEqual({ root: [] });
    expect(pendingSize(b)).toBe(0);
  });

  it("resolves a pending request from a JSON string (PostWebMessageAsString)", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    deliver(JSON.stringify({ type: "res", id: postedId(), ok: true, data: 42 }));
    await expect(p).resolves.toBe(42);
  });

  it("silently drops unparseable JSON strings — the request stays pending and a later good reply still lands", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    expect(() => deliver("{definitely not json")).not.toThrow();
    expect(pendingSize(b)).toBe(1); // untouched — the garbage did not consume it
    deliver({ type: "res", id: postedId(), ok: true, data: "late" });
    await expect(p).resolves.toBe("late");
  });

  it("ignores non-string non-object payloads (null / number / boolean)", () => {
    const b = new NativeBridge();
    void b.request({ kind: "emitters/list", params: {} } as never).catch(() => {});
    for (const junk of [null, undefined, 42, true]) {
      expect(() => deliver(junk)).not.toThrow();
    }
    expect(pendingSize(b)).toBe(1);
  });

  it("ignores a response whose id matches no pending request", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    expect(() => deliver({ type: "res", id: "r999-nope", ok: true, data: 1 })).not.toThrow();
    expect(pendingSize(b)).toBe(1); // the real request survived
    deliver({ type: "res", id: postedId(), ok: true, data: "real" });
    await expect(p).resolves.toBe("real");
  });

  it("ignores messages with an unknown type discriminator", () => {
    const b = new NativeBridge();
    void b.request({ kind: "emitters/list", params: {} } as never).catch(() => {});
    expect(() => deliver({ type: "telemetry", id: postedId() })).not.toThrow();
    expect(pendingSize(b)).toBe(1);
  });

  it("rejects the pending promise with the host's error string on an ok:false reply", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "file/save", params: {} } as never);
    deliver({ type: "res", id: postedId(), ok: false, error: "host exploded" });
    await expect(p).rejects.toThrow("host exploded");
    expect(pendingSize(b)).toBe(0);
  });

  it("an ok:false reply also clears an armed opt-in timeout timer (no zombie timer)", async () => {
    vi.useFakeTimers();
    const b = new NativeBridge({ requestTimeoutMs: 5_000 });
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    expect(vi.getTimerCount()).toBe(1);
    deliver({ type: "res", id: postedId(), ok: false, error: "boom" });
    await expect(p).rejects.toThrow("boom");
    expect(vi.getTimerCount()).toBe(0);
    // Advancing past the (cancelled) deadline must not throw or double-settle.
    vi.advanceTimersByTime(10_000);
    expect(pendingSize(b)).toBe(0);
  });

  it("a duplicate response for an already-settled id is a no-op (first reply wins)", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    const id = postedId();
    deliver({ type: "res", id, ok: true, data: "first" });
    expect(() => deliver({ type: "res", id, ok: false, error: "second" })).not.toThrow();
    await expect(p).resolves.toBe("first");
  });
});

describe("NativeBridge events", () => {
  const evt = (kind: string, payload: unknown) => ({ type: "evt", kind, payload });

  it("delivers events to a subscriber and stops after unsubscribe", () => {
    const b = new NativeBridge();
    const seen: unknown[] = [];
    const off = b.on("engine/state/changed" as never, (e) => seen.push(e));
    deliver(evt("engine/state/changed", { paused: true }));
    expect(seen).toEqual([{ kind: "engine/state/changed", payload: { paused: true } }]);
    off();
    deliver(evt("engine/state/changed", { paused: false }));
    expect(seen).toHaveLength(1);
    // Unsubscribing twice is harmless.
    expect(() => off()).not.toThrow();
  });

  it("an event arriving before any subscriber (or for an unknown kind) is dropped without throwing", () => {
    const b = new NativeBridge();
    expect(() => deliver(evt("engine/state/changed", { paused: true }))).not.toThrow();
    expect(() => deliver(evt("totally/unknown-kind", null))).not.toThrow();
    // A subscriber added afterwards only sees LATER events.
    const seen: unknown[] = [];
    b.on("engine/state/changed" as never, (e) => seen.push(e));
    expect(seen).toHaveLength(0);
    deliver(evt("engine/state/changed", { paused: false }));
    expect(seen).toHaveLength(1);
  });

  it("fans an event out to multiple subscribers of the same kind; removing one leaves the other live", () => {
    const b = new NativeBridge();
    const a: unknown[] = [];
    const c: unknown[] = [];
    const offA = b.on("dirty/changed" as never, (e) => a.push(e));
    b.on("dirty/changed" as never, (e) => c.push(e)); // reuses the existing bucket
    deliver(evt("dirty/changed", { dirty: true }));
    expect(a).toHaveLength(1);
    expect(c).toHaveLength(1);
    offA();
    deliver(evt("dirty/changed", { dirty: false }));
    expect(a).toHaveLength(1);
    expect(c).toHaveLength(2);
  });

  it("parses string-encoded events too", () => {
    const b = new NativeBridge();
    const seen: unknown[] = [];
    b.on("recent/changed" as never, (e) => seen.push(e));
    deliver(JSON.stringify(evt("recent/changed", { paths: ["a.alo"] })));
    expect(seen).toEqual([{ kind: "recent/changed", payload: { paths: ["a.alo"] } }]);
  });
});

describe("NativeBridge teardown", () => {
  it("dispose() is idempotent — a second call is a no-op, not a double-reject", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    b.dispose();
    expect(() => b.dispose()).not.toThrow();
    await expect(p).rejects.toThrow(/disposed/);
    expect(pendingSize(b)).toBe(0);
  });

  it("beforeunload disposes the bridge: in-flight requests reject cleanly, later sends fail closed", async () => {
    const b = new NativeBridge();
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    window.dispatchEvent(new Event("beforeunload"));
    await expect(p).rejects.toThrow(/disposed|disconnect/i);
    await expect(b.request({ kind: "emitters/list", params: {} } as never)).rejects.toThrow(
      /disposed|disconnect/i,
    );
    expect(pendingSize(b)).toBe(0);
  });

  it("dispose() clears armed timeout timers so no timeout fires after teardown", async () => {
    vi.useFakeTimers();
    const b = new NativeBridge({ requestTimeoutMs: 5_000 });
    const p = b.request({ kind: "emitters/list", params: {} } as never);
    expect(vi.getTimerCount()).toBe(1);
    b.dispose();
    expect(vi.getTimerCount()).toBe(0);
    // Rejected by DISPOSE, not by the (cancelled) timeout.
    await expect(p).rejects.toThrow(/disposed/);
    vi.advanceTimersByTime(60_000); // nothing left to fire
  });

  it("teardown with in-flight requests produces no unhandled rejection when callers attached handlers", async () => {
    const unhandled: unknown[] = [];
    const onUnhandled = (reason: unknown) => unhandled.push(reason);
    process.on("unhandledRejection", onUnhandled);
    try {
      const b = new NativeBridge();
      const settled = b
        .request({ kind: "emitters/list", params: {} } as never)
        .then(() => "resolved", (e: Error) => e.message);
      b.dispose();
      await expect(settled).resolves.toMatch(/disposed/);
      // A stale host response for the torn-down request is silently dropped.
      expect(() => deliver({ type: "res", id: postedId(), ok: true, data: 1 })).not.toThrow();
      // Give the microtask queue a beat so any unhandled rejection would surface.
      await new Promise((r) => setTimeout(r, 0));
      expect(unhandled).toEqual([]);
    } finally {
      process.off("unhandledRejection", onUnhandled);
    }
  });
});
