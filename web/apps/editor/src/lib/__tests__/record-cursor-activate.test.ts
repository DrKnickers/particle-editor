import { afterEach, beforeEach, describe, expect, it } from "vitest";
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

  it("focuses the nearest focusable on the activating press (mousedown default action)", () => {
    document.elementFromPoint = (() => input) as typeof document.elementFromPoint;
    const state = createRecordActivateState();

    applyRecordActivation({ x: 10, y: 10, press: true, ok: true, activate: true }, state, { dpr: 1 });

    expect(document.activeElement).toBe(input);
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
