import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  applyRecordActivation,
  createRecordActivateState,
  resetRecordActivation,
} from "../record-cursor-activate";

function listen(target: EventTarget, type: string, sink: Event[]): () => void {
  const h = (e: Event) => sink.push(e);
  target.addEventListener(type, h);
  return () => target.removeEventListener(type, h);
}

describe("record-cursor-activate", () => {
  let button: HTMLButtonElement;
  let input: HTMLInputElement;
  let origEFP: typeof document.elementFromPoint;

  beforeEach(() => {
    button = document.createElement("button");
    input = document.createElement("input");
    document.body.appendChild(button);
    document.body.appendChild(input);
    origEFP = document.elementFromPoint;
    document.elementFromPoint = (() => button) as typeof document.elementFromPoint;
  });

  afterEach(() => {
    document.elementFromPoint = origEFP;
    button.remove();
    input.remove();
  });

  it("press+release on an activate key dispatches a real click on the element", () => {
    const clicks: Event[] = [];
    const off = listen(button, "click", clicks);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 200, y: 100, press: true, ok: true, activate: true }, state, { dpr: 2 });
    expect(clicks).toHaveLength(0); // click fires on RELEASE, not press
    applyRecordActivation({ x: 200, y: 100, press: false, ok: true, activate: true }, state, { dpr: 2 });
    off();

    expect(clicks).toHaveLength(1);
    expect((clicks[0] as MouseEvent).clientX).toBe(100); // device/dpr
    expect(clicks[0].bubbles).toBe(true);
  });

  it("dispatches the mouse triple: mousedown at press, mouseup+click at release (Radix Tabs listen on mousedown)", () => {
    const downs: Event[] = [];
    const ups: Event[] = [];
    const offD = listen(button, "mousedown", downs);
    const offU = listen(button, "mouseup", ups);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    expect(downs).toHaveLength(1);
    expect(ups).toHaveLength(0);
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    offD();
    offU();

    expect(ups).toHaveLength(1);
  });

  it("a press WITHOUT activate never clicks or focuses (legacy clips untouched)", () => {
    const clicks: Event[] = [];
    const off = listen(button, "click", clicks);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 200, y: 100, press: true, ok: true, activate: false }, state, { dpr: 2 });
    applyRecordActivation({ x: 200, y: 100, press: false, ok: true, activate: false }, state, { dpr: 2 });
    off();

    expect(clicks).toHaveLength(0);
    expect(document.activeElement).not.toBe(button);
  });

  it("a ctrl modifier drives ctrlKey AND metaKey on the click (cross-platform toggle multi-select)", () => {
    const clicks: MouseEvent[] = [];
    const off = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, mods: { ctrl: true, shift: false } }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true, mods: { ctrl: true, shift: false } }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(1);
    expect(clicks[0].ctrlKey).toBe(true);
    expect(clicks[0].metaKey).toBe(true); // Cmd on macOS == Ctrl on Win/Linux for toggle
    expect(clicks[0].shiftKey).toBe(false);
  });

  it("a shift modifier drives shiftKey on the click (range multi-select)", () => {
    const clicks: MouseEvent[] = [];
    const off = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, mods: { ctrl: false, shift: true } }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true, mods: { ctrl: false, shift: true } }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(1);
    expect(clicks[0].shiftKey).toBe(true);
    expect(clicks[0].ctrlKey).toBe(false);
    expect(clicks[0].metaKey).toBe(false);
  });

  it("the click carries the PRESS-time modifiers even if the release key omits them", () => {
    const clicks: MouseEvent[] = [];
    const off = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    // press with ctrl; release without mods — the click must still be a ctrl-click.
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, mods: { ctrl: true, shift: false } }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(1);
    expect(clicks[0].ctrlKey).toBe(true);
  });

  it("no mods → an unmodified click (existing clips untouched)", () => {
    const clicks: MouseEvent[] = [];
    const off = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(1);
    expect(clicks[0].ctrlKey).toBe(false);
    expect(clicks[0].metaKey).toBe(false);
    expect(clicks[0].shiftKey).toBe(false);
  });

  it("a right button press dispatches contextmenu (the only way to open a context menu)", () => {
    const ctx: MouseEvent[] = [];
    const clicks: MouseEvent[] = [];
    const offC = listen(button, "contextmenu", ctx as Event[]);
    const offK = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, button: "right" }, state, { dpr: 1 });
    expect(ctx).toHaveLength(1);            // fires at press-down, like a real browser
    expect(ctx[0].button).toBe(2);
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true, button: "right" }, state, { dpr: 1 });
    offC();
    offK();

    // A right release fires NO click — real browsers only click for button 0.
    expect(clicks).toHaveLength(0);
  });

  it("a right press releases with button:2 on pointerup/mouseup and still no click", () => {
    const ups: MouseEvent[] = [];
    const off = listen(button, "mouseup", ups as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, button: "right" }, state, { dpr: 1 });
    // release key omits button — the press-time button must still be replayed
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(ups).toHaveLength(1);
    expect(ups[0].button).toBe(2);
  });

  it("release events report buttons:0 (nothing held) while still naming the changed button", () => {
    // `buttons` is the HELD mask, not the changed button — a non-zero mask on a
    // release tells viewport-input the button is still down (native mouseup payload).
    const downs: MouseEvent[] = [];
    const ups: MouseEvent[] = [];
    const clicks: MouseEvent[] = [];
    const offD = listen(button, "mousedown", downs as Event[]);
    const offU = listen(button, "mouseup", ups as Event[]);
    const offK = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    offD();
    offU();
    offK();

    expect(downs[0].buttons).toBe(1);   // left held during the press
    expect(ups[0].buttons).toBe(0);     // released — nothing held
    expect(ups[0].button).toBe(0);      // ...but still names the button that changed
    expect(clicks[0].buttons).toBe(0);
  });

  it("a right press holds buttons:2 down and releases with buttons:0", () => {
    const downs: MouseEvent[] = [];
    const ups: MouseEvent[] = [];
    const offD = listen(button, "mousedown", downs as Event[]);
    const offU = listen(button, "mouseup", ups as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true, button: "right" }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true, button: "right" }, state, { dpr: 1 });
    offD();
    offU();

    expect(downs[0].buttons).toBe(2);
    expect(ups[0].buttons).toBe(0);
    expect(ups[0].button).toBe(2);
  });

  it("resetRecordActivation clears the armed modifiers/button, not just the armed element", () => {
    const state = createRecordActivateState();
    applyRecordActivation(
      { x: 10, y: 10, press: true, ok: true, activate: true, button: "right", mods: { ctrl: true, shift: true } },
      state,
      { dpr: 1 },
    );
    expect(state.armedButton).toBe("right");
    expect(state.armedMods.ctrlKey).toBe(true);

    resetRecordActivation(state);

    expect(state.prevPress).toBe(false);
    expect(state.armed).toBeNull();
    expect(state.armedButton).toBeUndefined();
    expect(state.armedMods).toEqual({ ctrlKey: false, shiftKey: false, metaKey: false });
  });

  it("a LEFT (default) press fires no contextmenu and still clicks (existing clips untouched)", () => {
    const ctx: Event[] = [];
    const clicks: MouseEvent[] = [];
    const offC = listen(button, "contextmenu", ctx);
    const offK = listen(button, "click", clicks as Event[]);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    offC();
    offK();

    expect(ctx).toHaveLength(0);
    expect(clicks).toHaveLength(1);
    expect(clicks[0].button).toBe(0);
  });

  it("focuses the nearest focusable without a keyboard-visible ring on the activating press", () => {
    document.elementFromPoint = (() => input) as typeof document.elementFromPoint;
    const focus = vi.spyOn(input, "focus");
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });

    expect(document.activeElement).toBe(input);
    expect(focus).toHaveBeenCalledWith({ focusVisible: false });
  });

  it("blurs a focused text input when an activating press lands elsewhere (FieldText commit)", () => {
    input.focus();
    expect(document.activeElement).toBe(input);
    const state = createRecordActivateState();

    // press lands on the button (elementFromPoint stub), outside the input
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });

    expect(document.activeElement).not.toBe(input);
  });

  it("an unresolved or non-finite activating press never activates the corner", () => {
    const clicks: Event[] = [];
    const off = listen(button, "click", clicks);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 0, y: 0, press: true, ok: false, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: 0, y: 0, press: false, ok: false, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: Number.NaN, y: Number.NaN, press: true, ok: true, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: Number.NaN, y: Number.NaN, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(0);
  });

  it("a same-frame elementFromPoint miss retries on the next still-pressed frame (mirrors drag's retry semantics)", () => {
    const clicks: Event[] = [];
    const off = listen(button, "click", clicks);
    const state = createRecordActivateState();

    // First pressed frame: target not yet hit-testable (e.g. still mounting).
    document.elementFromPoint = (() => null) as typeof document.elementFromPoint;
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    expect(state.prevPress).toBe(false); // edge must NOT be consumed by the miss

    // Next still-pressed frame: the target resolves — the down should still arm.
    document.elementFromPoint = (() => button) as typeof document.elementFromPoint;
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    expect(state.prevPress).toBe(true);
    expect(state.armed).toBe(button);

    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(1);
  });

  it("a release off the armed element cancels the click (real-mouse semantics)", () => {
    const clicks: Event[] = [];
    const off = listen(document.body, "click", clicks);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    // release lands on the input instead of the armed button
    document.elementFromPoint = (() => input) as typeof document.elementFromPoint;
    applyRecordActivation({ x: 500, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(0);
  });

  it("resetRecordActivation drops an armed click (track swap)", () => {
    const clicks: Event[] = [];
    const off = listen(button, "click", clicks);
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    resetRecordActivation(state);
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    off();

    expect(clicks).toHaveLength(0);
    expect(state.armed).toBeNull();
  });
});

// 2026-07-10 regression guard: the activation gesture must include the POINTER
// pair in real-event order (pointerdown -> mousedown at press; pointerup ->
// mouseup -> click at release). Radix Menubar triggers (the emitter tree's +
// menu) open only on pointerdown, and the drag module deliberately skips
// activate presses — so a mouse-only activation focuses the trigger but never
// opens the menu (the tutorial-03 muzzle-flash add-emitter regression).
describe("record-cursor-activate pointer pair", () => {
  let button: HTMLButtonElement;
  let origEFP: typeof document.elementFromPoint;

  beforeEach(() => {
    button = document.createElement("button");
    document.body.appendChild(button);
    origEFP = document.elementFromPoint;
    document.elementFromPoint = (() => button) as typeof document.elementFromPoint;
  });

  afterEach(() => {
    document.elementFromPoint = origEFP;
    button.remove();
  });

  it("dispatches pointerdown before mousedown, and pointerup before mouseup/click", () => {
    const sink: Event[] = [];
    const offs = ["pointerdown", "mousedown", "pointerup", "mouseup", "click"].map((t) =>
      listen(button, t, sink),
    );
    const state = createRecordActivateState();
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    offs.forEach((off) => off());

    expect(sink.map((e) => e.type)).toEqual(["pointerdown", "mousedown", "pointerup", "mouseup", "click"]);
    // jsdom's PointerEvent drops pointerType/isPrimary — assert only what the
    // environment carries; the real browser gets pointerType "mouse" (see module).
    expect(sink[0].bubbles).toBe(true);
  });

  it("an off-target release still fires pointerup on the armed element (gesture cleanup) but no click", () => {
    const other = document.createElement("div");
    document.body.appendChild(other);
    const sink: Event[] = [];
    const offs = ["pointerdown", "pointerup", "mouseup", "click"].map((t) => listen(button, t, sink));
    const state = createRecordActivateState();
    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });
    // release lands on a different, non-containing element
    document.elementFromPoint = (() => other) as typeof document.elementFromPoint;
    applyRecordActivation({ x: 10, y: 10, press: false, ok: true, activate: true }, state, { dpr: 1 });
    offs.forEach((off) => off());
    other.remove();

    expect(sink.map((e) => e.type)).toEqual(["pointerdown", "pointerup"]);
  });
});
