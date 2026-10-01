// The host ui/* message hub: one webview listener, parse once, route by type.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { onUiMessage, parseUiMessage } from "../ui-message";

type Listener = (e: { data: unknown }) => void;
let listeners: Listener[];

function installWebview() {
  listeners = [];
  (window as unknown as { chrome: unknown }).chrome = {
    webview: {
      addEventListener: (ev: string, h: Listener) => { if (ev === "message") listeners.push(h); },
      removeEventListener: (ev: string, h: Listener) => {
        if (ev === "message") listeners = listeners.filter((l) => l !== h);
      },
    },
  };
}

const deliver = (data: unknown) => listeners.forEach((l) => l({ data }));

beforeEach(installWebview);
afterEach(() => {
  delete (window as unknown as { chrome?: unknown }).chrome;
});

describe("parseUiMessage", () => {
  it("parses object and string forms into a typed message", () => {
    expect(parseUiMessage({ type: "ui/show-panel", panel: "atlas" })).toEqual({ type: "ui/show-panel", panel: "atlas" });
    expect(parseUiMessage(JSON.stringify({ type: "ui/hide-panel" }))).toEqual({ type: "ui/hide-panel" });
    expect(parseUiMessage({ type: "ui/cursor", x: 1, y: 2, visible: true, pressed: false, frame: 7 })).toEqual({
      type: "ui/cursor", x: 1, y: 2, visible: true, pressed: false, frame: 7,
    });
  });

  it("rejects bridge envelopes, unknown ui/* types and malformed pushes", () => {
    expect(parseUiMessage({ type: "evt", kind: "dirty/changed", payload: {} })).toBeNull();
    expect(parseUiMessage({ type: "ui/frame-acked", frame: 1 })).toBeNull();
    expect(parseUiMessage({ type: "toString" })).toBeNull();
    expect(parseUiMessage({ type: "ui/open-picker", which: "nope" })).toBeNull();
    expect(parseUiMessage("{not json")).toBeNull();
    expect(parseUiMessage(null)).toBeNull();
  });
});

describe("onUiMessage", () => {
  it("routes each message to subscribers of its type only, over ONE webview listener", () => {
    const shows: unknown[] = [];
    const hides: unknown[] = [];
    const offA = onUiMessage("ui/show-panel", (m) => shows.push(m.panel));
    const offB = onUiMessage("ui/hide-panel", (m) => hides.push(m.type));
    expect(listeners).toHaveLength(1);
    deliver({ type: "ui/show-panel", panel: null });
    deliver({ type: "ui/hide-panel" });
    expect(shows).toEqual([null]);
    expect(hides).toEqual(["ui/hide-panel"]);
    offA();
    offA(); // idempotent
    expect(listeners).toHaveLength(1);
    offB();
    expect(listeners).toHaveLength(0); // last unsubscribe detaches
  });

  it("a throwing handler is logged and does not starve the next one", () => {
    const err = vi.spyOn(console, "error").mockImplementation(() => {});
    const seen: string[] = [];
    const offA = onUiMessage("ui/set-picker-search", () => { throw new Error("handler bug"); });
    const offB = onUiMessage("ui/set-picker-search", (m) => seen.push(m.text));
    expect(() => deliver({ type: "ui/set-picker-search", text: "AT-AT" })).not.toThrow();
    expect(seen).toEqual(["AT-AT"]);
    expect(err).toHaveBeenCalled();
    offA();
    offB();
    err.mockRestore();
  });

  it("re-attaches when chrome.webview is replaced", () => {
    const seen: string[] = [];
    const offA = onUiMessage("ui/focus-channel", (m) => seen.push(m.channel));
    installWebview(); // a fresh webview object, no listeners yet
    const offB = onUiMessage("ui/focus-channel", (m) => seen.push(`b:${m.channel}`));
    expect(listeners).toHaveLength(1);
    deliver({ type: "ui/focus-channel", channel: "scale" });
    expect(seen).toEqual(["scale", "b:scale"]);
    offA();
    offB();
  });

  it("is a no-op without a webview", () => {
    delete (window as unknown as { chrome?: unknown }).chrome;
    const off = onUiMessage("ui/hide-panel", () => {});
    expect(() => off()).not.toThrow();
  });
});
