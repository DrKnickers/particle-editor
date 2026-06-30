import { describe, it, expect } from "vitest";
import { parseOpenPickerMessage, parseSelectKeyMessage, parseShowPanelMessage } from "../record-focus-bridge";

describe("record-ui-bridge", () => {
  it("parses ui/show-panel messages", () => {
    expect(parseShowPanelMessage({ type: "ui/show-panel", panel: "spawner" })).toEqual({
      panel: "spawner",
    });
    expect(parseShowPanelMessage(JSON.stringify({ type: "ui/show-panel", panel: "lighting" }))).toEqual({
      panel: "lighting",
    });
    expect(parseShowPanelMessage({ type: "ui/show-panel", panel: null })).toEqual({ panel: null });
  });

  it("ignores invalid ui/show-panel messages", () => {
    expect(parseShowPanelMessage({ type: "ui/open-picker", panel: "spawner" })).toBeNull();
    expect(parseShowPanelMessage({ type: "ui/show-panel" })).toBeNull();
    expect(parseShowPanelMessage({ type: "ui/show-panel", panel: "mods" })).toBeNull();
    expect(parseShowPanelMessage({ type: "ui/show-panel", panel: 1 })).toBeNull();
  });

  it("parses ui/open-picker messages", () => {
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "reference", open: false })).toEqual({
      which: "reference",
      open: false,
    });
    expect(parseOpenPickerMessage(JSON.stringify({ type: "ui/open-picker", which: "background" }))).toEqual({
      which: "background",
      open: true,
    });
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "mods" })).toEqual({
      which: "mods",
      open: true,
    });
  });

  it("ignores invalid ui/open-picker messages", () => {
    expect(parseOpenPickerMessage({ type: "ui/show-panel", which: "reference" })).toBeNull();
    expect(parseOpenPickerMessage({ type: "ui/open-picker" })).toBeNull();
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "spawner" })).toBeNull();
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: 1 })).toBeNull();
    // `open`, when present, must be a real boolean — a stringy/nullish value rejects.
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "reference", open: "false" })).toBeNull();
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "reference", open: null })).toBeNull();
    expect(parseOpenPickerMessage({ type: "ui/open-picker", which: "reference", open: 0 })).toBeNull();
  });

  it("parses a ui/select-key message (object + JSON string)", () => {
    expect(parseSelectKeyMessage({ type: "ui/select-key", track: "index", time: 0 })).toEqual({
      track: "index",
      time: 0,
    });
    expect(parseSelectKeyMessage(JSON.stringify({ type: "ui/select-key", track: "red", time: 0.5 }))).toEqual({
      track: "red",
      time: 0.5,
    });
  });

  it("rejects malformed ui/select-key messages", () => {
    expect(parseSelectKeyMessage({ type: "ui/focus-channel", track: "index", time: 0 })).toBeNull();
    expect(parseSelectKeyMessage({ type: "ui/select-key", time: 0 })).toBeNull();
    expect(parseSelectKeyMessage({ type: "ui/select-key", track: "index" })).toBeNull();
    expect(parseSelectKeyMessage({ type: "ui/select-key", track: 1, time: 0 })).toBeNull();
    expect(parseSelectKeyMessage({ type: "ui/select-key", track: "index", time: "0" })).toBeNull();
    expect(parseSelectKeyMessage({ type: "ui/select-key", track: "index", time: NaN })).toBeNull();
    expect(parseSelectKeyMessage("not json")).toBeNull();
  });
});
