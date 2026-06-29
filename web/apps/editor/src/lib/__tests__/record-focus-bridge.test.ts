import { describe, it, expect } from "vitest";
import { parseFocusChannelMessage, parseHidePanelMessage } from "../record-focus-bridge";

describe("record-focus-bridge", () => {
  it("parses a ui/focus-channel message (object form)", () => {
    expect(parseFocusChannelMessage({ type: "ui/focus-channel", channel: "scale" })).toBe("scale");
  });
  it("parses a ui/focus-channel message (string form)", () => {
    expect(parseFocusChannelMessage(JSON.stringify({ type: "ui/focus-channel", channel: "alpha" }))).toBe("alpha");
  });
  it("ignores non-focus messages and bad payloads", () => {
    expect(parseFocusChannelMessage({ type: "ui/cursor", x: 1, y: 2 })).toBeNull();
    expect(parseFocusChannelMessage({ type: "ui/focus-channel" })).toBeNull(); // no channel
    expect(parseFocusChannelMessage({ type: "ui/focus-channel", channel: "" })).toBeNull(); // empty
    expect(parseFocusChannelMessage({ type: "ui/focus-channel", channel: 3 })).toBeNull(); // non-string
    expect(parseFocusChannelMessage(null)).toBeNull();
    expect(parseFocusChannelMessage("not json")).toBeNull();
  });

  it("parses a ui/hide-panel message (object + string form)", () => {
    expect(parseHidePanelMessage({ type: "ui/hide-panel" })).toBe(true);
    expect(parseHidePanelMessage(JSON.stringify({ type: "ui/hide-panel" }))).toBe(true);
  });
  it("ignores non-hide-panel messages and bad payloads", () => {
    expect(parseHidePanelMessage({ type: "ui/focus-channel", channel: "scale" })).toBe(false);
    expect(parseHidePanelMessage(null)).toBe(false);
    expect(parseHidePanelMessage("not json")).toBe(false);
  });
});
