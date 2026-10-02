import { describe, expect, it } from "vitest";
import { parseCursorTickMessage, parseCursorTrackMessage } from "../record-cursor-track";

describe("record-cursor-track", () => {
  it("parses point-target cursor tracks", () => {
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [
          { t: 0, vis: true, press: false, activate: false, target: { kind: "point", x: 10, y: 20 } },
        ],
      }),
    ).toEqual([
      { t: 0, vis: true, press: false, activate: false, target: { kind: "point", x: 10, y: 20 } },
    ]);
  });

  it("parses element-target cursor tracks from JSON strings", () => {
    expect(
      parseCursorTrackMessage(
        JSON.stringify({
          type: "ui/cursor-track",
          keys: [
            { t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "curve-key:red:0" } },
            { t: 1, vis: true, press: true, activate: false, target: { kind: "element", ref: "atlas-tile:3" } },
            { t: 2, vis: false, press: false, activate: false, target: { kind: "element", ref: "channel-row:alpha" } },
          ],
        }),
      ),
    ).toEqual([
      { t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "curve-key:red:0" } },
      { t: 1, vis: true, press: true, activate: false, target: { kind: "element", ref: "atlas-tile:3" } },
      { t: 2, vis: false, press: false, activate: false, target: { kind: "element", ref: "channel-row:alpha" } },
    ]);
  });

  it("parses an optional mods object (Ctrl/Shift multi-select) and normalizes missing flags to false", () => {
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [
          { t: 0, vis: true, press: true, activate: true, mods: { ctrl: true }, target: { kind: "element", ref: "testid:emitter-row:3" } },
          { t: 1, vis: true, press: true, activate: true, mods: { shift: true }, target: { kind: "element", ref: "testid:emitter-row:4" } },
        ],
      }),
    ).toEqual([
      { t: 0, vis: true, press: true, activate: true, mods: { ctrl: true, shift: false }, target: { kind: "element", ref: "testid:emitter-row:3" } },
      { t: 1, vis: true, press: true, activate: true, mods: { ctrl: false, shift: true }, target: { kind: "element", ref: "testid:emitter-row:4" } },
    ]);
  });

  it("omits mods when absent (existing clips parse identically — no mods key)", () => {
    const parsed = parseCursorTrackMessage({
      type: "ui/cursor-track",
      keys: [{ t: 0, vis: true, press: false, activate: false, target: { kind: "point", x: 1, y: 2 } }],
    });
    expect(parsed).not.toBeNull();
    expect(parsed![0]).not.toHaveProperty("mods");
  });

  it("rejects a malformed mods (non-object, or non-boolean flag) rather than dropping it silently", () => {
    for (const bad of [
      { t: 0, vis: true, press: true, activate: true, mods: "ctrl", target: { kind: "point", x: 1, y: 2 } },
      { t: 0, vis: true, press: true, activate: true, mods: { ctrl: 1 }, target: { kind: "point", x: 1, y: 2 } },
    ]) {
      expect(parseCursorTrackMessage({ type: "ui/cursor-track", keys: [bad] })).toBeNull();
    }
  });

  it("parses an optional button and omits it when absent (left is the default)", () => {
    const parsed = parseCursorTrackMessage({
      type: "ui/cursor-track",
      keys: [
        { t: 0, vis: true, press: true, activate: true, button: "right", target: { kind: "element", ref: "testid:emitter-row:2" } },
        { t: 1, vis: true, press: true, activate: true, target: { kind: "point", x: 1, y: 2 } },
      ],
    });
    expect(parsed).not.toBeNull();
    expect(parsed![0].button).toBe("right");
    expect(parsed![1]).not.toHaveProperty("button");
  });

  it("rejects an unknown button rather than silently degrading to a left-click", () => {
    for (const bad of ["middle", "Right", 2, true]) {
      expect(
        parseCursorTrackMessage({
          type: "ui/cursor-track",
          keys: [{ t: 0, vis: true, press: true, activate: true, button: bad, target: { kind: "point", x: 1, y: 2 } }],
        }),
      ).toBeNull();
    }
  });

  it("accepts a testid ref whose id contains colons (free-form data-testid)", () => {
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "testid:a:b:c" } }],
      }),
    ).toEqual([{ t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "testid:a:b:c" } }]);
    // a bare "testid:" with no id is still rejected
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "testid:" } }],
      }),
    ).toBeNull();
  });

  it("rejects malformed cursor tracks", () => {
    expect(parseCursorTrackMessage({ type: "ui/cursor-track", keys: [] })).toBeNull();
    // activate is opt-in per key: parses through when true, rejects non-boolean
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: true, activate: true, target: { kind: "point", x: 1, y: 2 } }],
      }),
    ).toEqual([{ t: 0, vis: true, press: true, activate: true, target: { kind: "point", x: 1, y: 2 } }]);
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: true, activate: "yes", target: { kind: "point", x: 1, y: 2 } }],
      }),
    ).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ vis: true, press: false, target: { kind: "point", x: 0, y: 0 } }],
      }),
    ).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, activate: false, target: { kind: "element", ref: "button:ok" } }],
      }),
    ).toBeNull();
    expect(
      parseCursorTrackMessage({
        type: "ui/cursor-track",
        keys: [{ t: 0, vis: true, press: false, activate: false, target: { kind: "point", x: Infinity, y: 0 } }],
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
