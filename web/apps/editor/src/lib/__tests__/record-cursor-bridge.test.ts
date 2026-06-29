import { describe, it, expect, vi } from "vitest";
import { parseCursorMessage, postFrameAcked } from "../record-cursor-bridge";

describe("record-cursor-bridge", () => {
  it("parses a ui/cursor message (object form)", () => {
    expect(parseCursorMessage({ type: "ui/cursor", x: 10, y: 20, visible: true, pressed: false }))
      .toEqual({ x: 10, y: 20, visible: true, pressed: false });
  });
  it("parses a ui/cursor message (string form)", () => {
    expect(parseCursorMessage(JSON.stringify({ type: "ui/cursor", x: 1, y: 2, visible: false, pressed: true })))
      .toEqual({ x: 1, y: 2, visible: false, pressed: true });
  });
  it("ignores non-cursor messages", () => {
    expect(parseCursorMessage({ type: "engine/state/changed" })).toBeNull();
    expect(parseCursorMessage(null)).toBeNull();
    expect(parseCursorMessage("not json")).toBeNull();
  });
  it("posts a frame-acked message (JSON string) with the frame id", () => {
    const postMessage = vi.fn();
    (window as unknown as { chrome: { webview: { postMessage: typeof postMessage } } }).chrome = {
      webview: { postMessage },
    };
    postFrameAcked(7);
    expect(postMessage).toHaveBeenCalledWith(JSON.stringify({ type: "ui/frame-acked", frame: 7 }));
  });
});
