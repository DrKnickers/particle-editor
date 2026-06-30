import { describe, it, expect } from "vitest";
import {
  parseFocusChannelMessage,
  parseHidePanelMessage,
  parseSetPickerCollapseMessage,
  parseSetPickerSearchMessage,
} from "../record-focus-bridge";

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

  it("parses a ui/set-picker-search message (object + string form)", () => {
    expect(parseSetPickerSearchMessage({ type: "ui/set-picker-search", text: "AT_" })).toEqual({ text: "AT_" });
    expect(parseSetPickerSearchMessage(JSON.stringify({ type: "ui/set-picker-search", text: "" }))).toEqual({ text: "" });
  });
  it("ignores non-search messages and bad payloads", () => {
    expect(parseSetPickerSearchMessage({ type: "ui/open-picker", which: "reference" })).toBeNull();
    expect(parseSetPickerSearchMessage({ type: "ui/set-picker-search" })).toBeNull(); // no text
    expect(parseSetPickerSearchMessage({ type: "ui/set-picker-search", text: 3 })).toBeNull(); // non-string
    expect(parseSetPickerSearchMessage(null)).toBeNull();
    expect(parseSetPickerSearchMessage("not json")).toBeNull();
  });
  it("parses a ui/picker-collapse message (object + string form)", () => {
    expect(parseSetPickerCollapseMessage({ type: "ui/picker-collapse", keys: ["Heroes"] })).toEqual({ keys: ["Heroes"] });
    expect(parseSetPickerCollapseMessage(JSON.stringify({ type: "ui/picker-collapse", keys: [] }))).toEqual({ keys: [] });
  });
  it("ignores non-collapse messages and bad payloads", () => {
    expect(parseSetPickerCollapseMessage({ type: "ui/set-picker-search", text: "AT_" })).toBeNull();
    expect(parseSetPickerCollapseMessage({ type: "ui/picker-collapse" })).toBeNull(); // no keys
    expect(parseSetPickerCollapseMessage({ type: "ui/picker-collapse", keys: "Heroes" })).toBeNull(); // not array
    expect(parseSetPickerCollapseMessage({ type: "ui/picker-collapse", keys: [1, 2] })).toBeNull(); // non-string items
    expect(parseSetPickerCollapseMessage(null)).toBeNull();
  });
});
