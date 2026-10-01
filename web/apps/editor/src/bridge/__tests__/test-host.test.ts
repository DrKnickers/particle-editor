// TestHostBridge — the --test-host bridge: requests go through the host-object
// channel (dispatchRequest), events still arrive over the webview "message"
// channel. Covers the shared envelope parsing + event fan-out it now takes from
// bridge/wire.ts and bridge/event-hub.ts.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { TestHostBridge } from "../test-host";

type MessageHandler = (e: { data: unknown }) => void;

let messageHandler: MessageHandler | undefined;
let dispatchRequest: ReturnType<typeof vi.fn>;

beforeEach(() => {
  messageHandler = undefined;
  dispatchRequest = vi.fn();
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      addEventListener: (ev: string, h: MessageHandler) => {
        if (ev === "message") messageHandler = h;
      },
      hostObjects: { hostBridge: { dispatchRequest } },
    },
  };
});

afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
});

describe("TestHostBridge requests", () => {
  it("resolves with the response data", async () => {
    dispatchRequest.mockResolvedValue(JSON.stringify({ type: "res", ok: true, data: { applied: true } }));
    const b = new TestHostBridge();
    await expect(b.request({ kind: "undo/perform", params: { direction: "undo" } })).resolves.toEqual({
      applied: true,
    });
  });

  it("rejects with the host's error string", async () => {
    dispatchRequest.mockResolvedValue(JSON.stringify({ type: "res", ok: false, error: "nope" }));
    const b = new TestHostBridge();
    await expect(b.request({ kind: "undo/perform", params: { direction: "undo" } })).rejects.toThrow("nope");
  });

  it("an ok:false reply with no error string rejects with a message naming the request", async () => {
    dispatchRequest.mockResolvedValue(JSON.stringify({ type: "res", ok: false }));
    const b = new TestHostBridge();
    await expect(b.request({ kind: "undo/perform", params: { direction: "undo" } })).rejects.toThrow(
      /undo\/perform/,
    );
  });

  it("surfaces the host's diagnostic from an id:null error envelope", async () => {
    // BridgeDispatcher's parse-error / dispatch-exception envelopes carry id: null.
    dispatchRequest.mockResolvedValue(JSON.stringify({ type: "res", id: null, ok: false, error: "parse error: x" }));
    const b = new TestHostBridge();
    await expect(b.request({ kind: "undo/perform", params: { direction: "undo" } })).rejects.toThrow(
      "parse error: x",
    );
  });

  it("rejects a reply that is not a response envelope", async () => {
    dispatchRequest.mockResolvedValue(JSON.stringify({ hello: "world" }));
    const b = new TestHostBridge();
    await expect(b.request({ kind: "undo/perform", params: { direction: "undo" } })).rejects.toThrow(
      /undo\/perform/,
    );
  });
});

describe("TestHostBridge events", () => {
  it("a throwing subscriber is logged and does not stop the remaining subscribers", () => {
    const b = new TestHostBridge();
    const err = vi.spyOn(console, "error").mockImplementation(() => {});
    const seen: unknown[] = [];
    b.on("dirty/changed", () => { throw new Error("subscriber bug"); });
    b.on("dirty/changed", (e) => seen.push(e));
    expect(() => messageHandler!({ data: { type: "evt", kind: "dirty/changed", payload: { dirty: true } } })).not.toThrow();
    expect(seen).toEqual([{ kind: "dirty/changed", payload: { dirty: true } }]);
    expect(err).toHaveBeenCalled();
    err.mockRestore();
  });
});
