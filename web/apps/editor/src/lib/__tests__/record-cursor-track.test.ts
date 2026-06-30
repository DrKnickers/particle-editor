import { describe, expect, it } from "vitest";
import { parseCursorTickMessage, parseCursorTrackMessage } from "../record-cursor-track";

describe("record-cursor-track", () => {
  it("parses point-target cursor tracks", () => {
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [
          { t: 0, vis: true, press: false, target: { kind: "point", x: 10, y: 20 } },
        ],
      }),
    ).toEqual([
      { t: 0, vis: true, press: false, target: { kind: "point", x: 10, y: 20 } },
    ]);
  });

  it("parses element-target cursor tracks from JSON strings", () => {
    expect(
      parseCursorTrackMessage(
        JSON.stringify({
          type: "ui/cursor-track",
          keys: [
            { t: 0, vis: true, press: false, target: { kind: "element", ref: "curve-key:red:0" } },
            { t: 1, vis: true, press: true, target: { kind: "element", ref: "atlas-tile:3" } },
            { t: 2, vis: false, press: false, target: { kind: "element", ref: "channel-row:alpha" } },
          ],
        }),
      ),
    ).toEqual([
      { t: 0, vis: true, press: false, target: { kind: "element", ref: "curve-key:red:0" } },
      { t: 1, vis: true, press: true, target: { kind: "element", ref: "atlas-tile:3" } },
      { t: 2, vis: false, press: false, target: { kind: "element", ref: "channel-row:alpha" } },
    ]);
  });

  it("rejects malformed cursor tracks", () => {
    expect(parseCursorTrackMessage({ type: "ui/cursor-track", keys: [] })).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ vis: true, press: false, target: { kind: "point", x: 0, y: 0 } }],
      }),
    ).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, target: { kind: "element", ref: "button:ok" } }],
      }),
    ).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, target: { kind: "point", x: Infinity, y: 0 } }],
      }),
    ).toBeNull();
  });

  it("parses cursor tick messages", () => {
    expect(parseCursorTickMessage({ type: "ui/cursor-tick", t: 250, frame: 4 })).toEqual({
      t: 250,
      frame: 4,
    });
    expect(parseCursorTickMessage(JSON.stringify({ type: "ui/cursor-tick", t: 0, frame: 0 }))).toEqual({
      t: 0,
      frame: 0,
    });
    expect(parseCursorTickMessage({ type: "ui/cursor-tick", t: "0", frame: 0 })).toBeNull();
    expect(parseCursorTickMessage({ type: "ui/cursor-tick", t: 0 })).toBeNull();
  });
});
