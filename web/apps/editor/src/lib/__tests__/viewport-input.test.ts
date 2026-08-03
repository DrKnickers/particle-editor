// Unit tests for the viewport-input encoder helpers. The encoders are
// pure functions over DOM event shapes — testing them here lets the
// ViewportSlot.test.tsx integration suite focus on the wiring rather
// than the bitmask arithmetic.

import { describe, it, expect } from "vitest";
import {
  MK_LBUTTON,
  MK_RBUTTON,
  MK_MBUTTON,
  MK_SHIFT,
  MK_CONTROL,
  WHEEL_DELTA,
  encodeMkButtons,
  quantiseWheelDelta,
  toPopupClientCoords,
  isTypingTarget,
  makeMouseEvent,
  makeWheelEvent,
  makeKeyEvent,
  blurEvent,
} from "../viewport-input";

// Helper: build a partial MouseEvent shape that satisfies the
// encoder's read pattern (only `buttons`, `shiftKey`, `ctrlKey`,
// `button`).
function fakeMouse(opts: {
  buttons?: number;
  shiftKey?: boolean;
  ctrlKey?: boolean;
  button?: number;
}): MouseEvent {
  return {
    buttons: opts.buttons ?? 0,
    shiftKey: opts.shiftKey ?? false,
    ctrlKey: opts.ctrlKey ?? false,
    button: opts.button ?? 0,
  } as unknown as MouseEvent;
}

describe("encodeMkButtons", () => {
  it("returns 0 for no buttons + no modifiers", () => {
    expect(encodeMkButtons(fakeMouse({}))).toBe(0);
  });

  it("maps LMB (buttons=1) → MK_LBUTTON", () => {
    expect(encodeMkButtons(fakeMouse({ buttons: 1 }))).toBe(MK_LBUTTON);
  });

  it("maps RMB (buttons=2) → MK_RBUTTON", () => {
    expect(encodeMkButtons(fakeMouse({ buttons: 2 }))).toBe(MK_RBUTTON);
  });

  it("maps MMB (buttons=4) → MK_MBUTTON", () => {
    expect(encodeMkButtons(fakeMouse({ buttons: 4 }))).toBe(MK_MBUTTON);
  });

  it("combines LMB + Shift → MK_LBUTTON | MK_SHIFT (the cursor-bound-spawn combo)", () => {
    const m = encodeMkButtons(fakeMouse({ buttons: 1, shiftKey: true }));
    expect(m).toBe(MK_LBUTTON | MK_SHIFT);
  });

  it("combines LMB + Ctrl → MK_LBUTTON | MK_CONTROL (the LMB-zoom combo)", () => {
    const m = encodeMkButtons(fakeMouse({ buttons: 1, ctrlKey: true }));
    expect(m).toBe(MK_LBUTTON | MK_CONTROL);
  });

  it("combines all bits when all flags + buttons are set", () => {
    const m = encodeMkButtons(
      fakeMouse({ buttons: 7, shiftKey: true, ctrlKey: true }),
    );
    expect(m).toBe(MK_LBUTTON | MK_RBUTTON | MK_MBUTTON | MK_SHIFT | MK_CONTROL);
  });
});

describe("quantiseWheelDelta", () => {
  it("returns 0 when input is 0", () => {
    expect(quantiseWheelDelta(0)).toBe(0);
  });

  it("flips sign and clamps positive DOM delta to -WHEEL_DELTA (scroll-down → zoom-out)", () => {
    expect(quantiseWheelDelta(100)).toBe(-WHEEL_DELTA);
    expect(quantiseWheelDelta(1)).toBe(-WHEEL_DELTA);
    expect(quantiseWheelDelta(10_000)).toBe(-WHEEL_DELTA);
  });

  it("flips sign and clamps negative DOM delta to +WHEEL_DELTA (scroll-up → zoom-in)", () => {
    expect(quantiseWheelDelta(-100)).toBe(WHEEL_DELTA);
    expect(quantiseWheelDelta(-1)).toBe(WHEEL_DELTA);
    expect(quantiseWheelDelta(-10_000)).toBe(WHEEL_DELTA);
  });
});

describe("toPopupClientCoords", () => {
  it("multiplies by devicePixelRatio at the call site (DPR=1 default)", () => {
    const { x, y } = toPopupClientCoords(150, 200);
    expect(x).toBe(150);
    expect(y).toBe(200);
  });

  it("re-reads DPR per call (no caching)", () => {
    const originalDpr = window.devicePixelRatio;
    try {
      Object.defineProperty(window, "devicePixelRatio", {
        configurable: true,
        value: 2,
      });
      const { x, y } = toPopupClientCoords(150, 200);
      expect(x).toBe(300);
      expect(y).toBe(400);
    } finally {
      Object.defineProperty(window, "devicePixelRatio", {
        configurable: true,
        value: originalDpr,
      });
    }
  });

  it("rounds to integers (Win32 LPARAM 16-bit packing expects ints)", () => {
    const originalDpr = window.devicePixelRatio;
    try {
      Object.defineProperty(window, "devicePixelRatio", {
        configurable: true,
        value: 1.5,
      });
      const { x } = toPopupClientCoords(100, 100);
      expect(Number.isInteger(x)).toBe(true);
    } finally {
      Object.defineProperty(window, "devicePixelRatio", {
        configurable: true,
        value: originalDpr,
      });
    }
  });
});

describe("isTypingTarget", () => {
  it("returns false for null target", () => {
    expect(isTypingTarget(null)).toBe(false);
  });

  it("returns false for non-Element targets (Document, Window)", () => {
    expect(isTypingTarget(document)).toBe(false);
    expect(isTypingTarget(window)).toBe(false);
  });

  it("returns true for INPUT", () => {
    const el = document.createElement("input");
    expect(isTypingTarget(el)).toBe(true);
  });

  it("returns true for TEXTAREA", () => {
    const el = document.createElement("textarea");
    expect(isTypingTarget(el)).toBe(true);
  });

  it("returns true for SELECT", () => {
    const el = document.createElement("select");
    expect(isTypingTarget(el)).toBe(true);
  });

  it("returns true for contenteditable='true' DIV", () => {
    const el = document.createElement("div");
    el.setAttribute("contenteditable", "true");
    expect(isTypingTarget(el)).toBe(true);
  });

  it("returns false for plain DIV / BUTTON / CANVAS", () => {
    expect(isTypingTarget(document.createElement("div"))).toBe(false);
    expect(isTypingTarget(document.createElement("button"))).toBe(false);
    expect(isTypingTarget(document.createElement("canvas"))).toBe(false);
  });
});

describe("makeMouseEvent", () => {
  it("encodes mousemove with no button discriminator", () => {
    const evt = makeMouseEvent("mousemove", fakeMouse({ buttons: 1 }), 100, 200);
    expect(evt).toEqual({
      type: "mousemove",
      x: 100,
      y: 200,
      buttons: MK_LBUTTON,
    });
  });

  it("encodes mousedown with left/right/middle button discriminator", () => {
    const left = makeMouseEvent("mousedown", fakeMouse({ button: 0 }), 1, 2);
    expect(left).toMatchObject({ type: "mousedown", button: "left" });
    const middle = makeMouseEvent("mousedown", fakeMouse({ button: 1 }), 1, 2);
    expect(middle).toMatchObject({ type: "mousedown", button: "middle" });
    const right = makeMouseEvent("mousedown", fakeMouse({ button: 2 }), 1, 2);
    expect(right).toMatchObject({ type: "mousedown", button: "right" });
  });

  it("carries MK_SHIFT through for Shift+LMB (cursor-bound-spawn combo)", () => {
    const evt = makeMouseEvent(
      "mousedown",
      fakeMouse({ button: 0, buttons: 1, shiftKey: true }),
      50,
      60,
    );
    expect(evt).toMatchObject({
      type: "mousedown",
      button: "left",
      buttons: MK_LBUTTON | MK_SHIFT,
    });
  });
});

describe("makeWheelEvent", () => {
  it("packs quantised deltaY + buttons + popup-client coords", () => {
    const fake = {
      deltaY: 100,
      buttons: 0,
      shiftKey: false,
      ctrlKey: false,
    } as unknown as WheelEvent;
    const evt = makeWheelEvent(fake, 10, 20);
    expect(evt).toEqual({
      type: "wheel",
      x: 10,
      y: 20,
      deltaY: -WHEEL_DELTA,  // sign-flipped per the convention
      buttons: 0,
    });
  });
});

describe("makeKeyEvent", () => {
  it("packs vk + repeat", () => {
    const fake = { keyCode: 16, repeat: false } as KeyboardEvent;
    expect(makeKeyEvent("keydown", fake)).toEqual({
      type: "keydown",
      vk: 16,
      repeat: false,
    });
  });

  it("preserves repeat=true for held keys", () => {
    const fake = { keyCode: 16, repeat: true } as KeyboardEvent;
    expect(makeKeyEvent("keydown", fake)).toMatchObject({ repeat: true });
  });
});

describe("blurEvent", () => {
  it("is a singleton constant of shape { type: 'blur' }", () => {
    expect(blurEvent).toEqual({ type: "blur" });
  });
});
