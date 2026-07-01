import { afterEach, beforeEach, describe, expect, it } from "vitest";
import { applyRecordDrag, createRecordDragState, resetRecordDrag } from "../record-cursor-drag";

// Capture every event of `type` dispatched at `target` into `sink`.
function listen(target: EventTarget, type: string, sink: PointerEvent[]): () => void {
  const h = (e: Event) => sink.push(e as PointerEvent);
  target.addEventListener(type, h);
  return () => target.removeEventListener(type, h);
}

describe("record-cursor-drag", () => {
  let row: HTMLElement;
  let origEFP: typeof document.elementFromPoint;

  beforeEach(() => {
    row = document.createElement("div");
    row.setAttribute("data-flip-key", "modA");
    document.body.appendChild(row);
    // jsdom does no layout, so elementFromPoint always returns null - stub it to the row.
    origEFP = document.elementFromPoint;
    document.elementFromPoint = (() => row) as typeof document.elementFromPoint;
  });

  afterEach(() => {
    document.elementFromPoint = origEFP;
    row.remove();
  });

  it("dispatches pointerdown on the element under the cursor when press goes false to true", () => {
    const downs: PointerEvent[] = [];
    const off = listen(row, "pointerdown", downs);
    const state = createRecordDragState();

    applyRecordDrag({ x: 200, y: 100, press: true, ok: true }, state, { dpr: 2, doc: document });
    off();

    expect(downs).toHaveLength(1);
    // device 200,100 / dpr 2 -> CSS client 100,50.
    expect(downs[0]).toMatchObject({ pointerId: 1, button: 0, buttons: 1, clientX: 100, clientY: 50 });
    expect(downs[0]!.target).toBe(row);
    expect(downs[0]!.bubbles).toBe(true);
    expect(state.prevPress).toBe(true);
  });

  it("redirects the pointerdown to the reorderable row when the cursor lands on an inner control", () => {
    // The stack gesture bails if the pointerdown target is inside an inner button (the
    // row's remove control), which would strand the drag (armed here, no gesture there).
    // We dispatch on the row (closest data-flip-key/data-emitter-id) so the gesture
    // accepts. Verify the down's target is the ROW, not the inner child.
    const inner = document.createElement("button");
    row.appendChild(inner);
    document.elementFromPoint = (() => inner) as typeof document.elementFromPoint; // hit the inner control
    const downs: PointerEvent[] = [];
    const off = listen(row, "pointerdown", downs);
    const state = createRecordDragState();

    applyRecordDrag({ x: 10, y: 10, press: true, ok: true }, state, { dpr: 1, doc: document });
    off();

    expect(downs).toHaveLength(1);
    expect(downs[0]!.target).toBe(row); // redirected up to the row, not the inner child
    expect(state.prevPress).toBe(true);
  });

  it("does not dispatch a down on non-finite (NaN) coords even when pressing (empty-track sentinel)", () => {
    // evalRecordCursor returns NaN coords for an empty track; a NaN down would
    // elementFromPoint(NaN) / dispatch at a garbage point. The finite guard blocks it.
    const downs: PointerEvent[] = [];
    const off = listen(document, "pointerdown", downs);
    const state = createRecordDragState();

    applyRecordDrag({ x: Number.NaN, y: Number.NaN, press: true, ok: true }, state, { dpr: 1, doc: document });
    off();

    expect(downs).toHaveLength(0);
    expect(state.prevPress).toBe(false);
  });

  it("dispatches pointermove on document (not the row) while the press is held", () => {
    const onDoc: PointerEvent[] = [];
    const onRow: PointerEvent[] = [];
    const offDoc = listen(document, "pointermove", onDoc);
    const offRow = listen(row, "pointermove", onRow);
    const state = createRecordDragState();

    applyRecordDrag({ x: 200, y: 100, press: true, ok: true }, state, { dpr: 2, doc: document }); // down
    applyRecordDrag({ x: 240, y: 160, press: true, ok: true }, state, { dpr: 2, doc: document }); // move
    offDoc();
    offRow();

    expect(onDoc).toHaveLength(1);
    expect(onDoc[0]).toMatchObject({ pointerId: 1, buttons: 1, clientX: 120, clientY: 80 });
    expect(onRow).toHaveLength(0); // never dispatched on the element
  });

  it("dispatches pointerup on document and clears state when press goes true to false", () => {
    const ups: PointerEvent[] = [];
    const off = listen(document, "pointerup", ups);
    const state = createRecordDragState();

    applyRecordDrag({ x: 200, y: 100, press: true, ok: true }, state, { dpr: 2, doc: document }); // down
    applyRecordDrag({ x: 200, y: 100, press: false, ok: true }, state, { dpr: 2, doc: document }); // up
    off();

    expect(ups).toHaveLength(1);
    expect(ups[0]).toMatchObject({ pointerId: 1, buttons: 0, clientX: 100, clientY: 50 });
    expect(state.prevPress).toBe(false);
  });

  it("releases even when the release frame is unresolved (ok:false) - the release is unconditional", () => {
    // Guards the design choice: the down/move abort on !ok, but the UP always fires
    // (finish() commits from the last resolved gap, not this frame's coords). A
    // regression that added an ok-guard to the release path would break this.
    const ups: PointerEvent[] = [];
    const off = listen(document, "pointerup", ups);
    const state = createRecordDragState();

    applyRecordDrag({ x: 200, y: 100, press: true, ok: true }, state, { dpr: 1, doc: document }); // down (armed)
    applyRecordDrag({ x: 0, y: 0, press: false, ok: false }, state, { dpr: 1, doc: document }); // release, unresolved
    off();

    expect(ups).toHaveLength(1);
    expect(state.prevPress).toBe(false);
  });

  it("drives a full down-move-move-up sequence in order", () => {
    const seq: string[] = [];
    const offD = listen(row, "pointerdown", [] as PointerEvent[]);
    document.addEventListener("pointerdown", (e) => seq.push(e.type));
    document.addEventListener("pointermove", (e) => seq.push(e.type));
    document.addEventListener("pointerup", (e) => seq.push(e.type));
    const state = createRecordDragState();

    applyRecordDrag({ x: 0, y: 0, press: true, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 0, y: 10, press: true, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 0, y: 40, press: true, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 0, y: 40, press: false, ok: true }, state, { dpr: 1, doc: document });
    offD();

    expect(seq).toEqual(["pointerdown", "pointermove", "pointermove", "pointerup"]);
  });

  it("emits nothing while the cursor is never pressed", () => {
    const events: PointerEvent[] = [];
    const offs = [
      listen(document, "pointerdown", events),
      listen(document, "pointermove", events),
      listen(document, "pointerup", events),
    ];
    const state = createRecordDragState();

    applyRecordDrag({ x: 10, y: 10, press: false, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 20, y: 20, press: false, ok: true }, state, { dpr: 1, doc: document });
    offs.forEach((off) => off());

    expect(events).toHaveLength(0);
    expect(state.prevPress).toBe(false);
  });

  it("skips the pointerdown (and stays unpressed) when nothing is under the cursor", () => {
    document.elementFromPoint = (() => null) as typeof document.elementFromPoint;
    const downs: PointerEvent[] = [];
    const off = listen(document, "pointerdown", downs);
    const state = createRecordDragState();

    applyRecordDrag({ x: 10, y: 10, press: true, ok: true }, state, { dpr: 1, doc: document });
    off();

    expect(downs).toHaveLength(0);
    expect(state.prevPress).toBe(false); // retry next frame
  });

  // Gap 2: an unresolved press must NOT click the corner.
  it("does not arm a pointerdown on an unresolved press (ok:false) even with an element under the cursor", () => {
    // The eval keeps press:true but reports x=y=0, ok:false for an unresolvable ref.
    // elementFromPoint(0,0) here still returns `row` (stubbed) - the ok guard, not the
    // EFP null check, is what must prevent the click.
    const downs: PointerEvent[] = [];
    const off = listen(row, "pointerdown", downs);
    const state = createRecordDragState();

    applyRecordDrag({ x: 0, y: 0, press: true, ok: false }, state, { dpr: 1, doc: document });
    off();

    expect(downs).toHaveLength(0);
    expect(state.prevPress).toBe(false); // not armed -> retry when the target resolves
  });

  it("holds (no pointermove) on an unresolved frame mid-drag, then resumes when it resolves again", () => {
    const moves: PointerEvent[] = [];
    const off = listen(document, "pointermove", moves);
    const state = createRecordDragState();

    applyRecordDrag({ x: 100, y: 100, press: true, ok: true }, state, { dpr: 1, doc: document }); // down
    applyRecordDrag({ x: 0, y: 0, press: true, ok: false }, state, { dpr: 1, doc: document }); // unresolved -> hold
    applyRecordDrag({ x: 140, y: 140, press: true, ok: true }, state, { dpr: 1, doc: document }); // resolved -> move
    off();

    expect(moves).toHaveLength(1);
    expect(moves[0]).toMatchObject({ clientX: 140, clientY: 140 });
    expect(state.prevPress).toBe(true); // still armed across the held frame
  });

  // Gap 3: literal (point) tracks drive the drag exactly like element refs.
  it("drives a full drag from literal point coords (ok:true, no ref)", () => {
    // A point target resolves to ok:true with raw coords and no ref; the dispatcher is
    // coord-driven (elementFromPoint for the down), so it works identically.
    const seq: string[] = [];
    document.addEventListener("pointerdown", (e) => seq.push(e.type));
    document.addEventListener("pointermove", (e) => seq.push(e.type));
    document.addEventListener("pointerup", (e) => seq.push(e.type));
    const state = createRecordDragState();

    applyRecordDrag({ x: 308, y: 59, press: true, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 320, y: 80, press: true, ok: true }, state, { dpr: 1, doc: document });
    applyRecordDrag({ x: 320, y: 80, press: false, ok: true }, state, { dpr: 1, doc: document });

    expect(seq).toEqual(["pointerdown", "pointermove", "pointerup"]);
    expect(state.prevPress).toBe(false);
  });

  // Gap 1: a track swap mid-drag must abort (not commit) the armed gesture.
  it("resetRecordDrag aborts an armed drag with pointercancel and clears state", () => {
    const cancels: PointerEvent[] = [];
    const ups: PointerEvent[] = [];
    const offCancel = listen(document, "pointercancel", cancels);
    const offUp = listen(document, "pointerup", ups);
    const state = createRecordDragState();

    applyRecordDrag({ x: 100, y: 100, press: true, ok: true }, state, { dpr: 1, doc: document }); // arm a drag
    resetRecordDrag(state, { doc: document }); // track swapped mid-drag
    offCancel();
    offUp();

    expect(cancels).toHaveLength(1);
    expect(cancels[0]).toMatchObject({ pointerId: 1 });
    expect(ups).toHaveLength(0); // abort, NOT commit - no pointerup
    expect(state.prevPress).toBe(false);
  });

  it("resetRecordDrag is a no-op when no drag is armed", () => {
    const cancels: PointerEvent[] = [];
    const off = listen(document, "pointercancel", cancels);
    const state = createRecordDragState();

    resetRecordDrag(state, { doc: document });
    off();

    expect(cancels).toHaveLength(0);
    expect(state.prevPress).toBe(false);
  });

  // Gap 4: dpr defaults to window.devicePixelRatio (and doc to document) when opts omitted.
  it("falls back to window.devicePixelRatio and the global document when opts are omitted", () => {
    const downs: PointerEvent[] = [];
    const off = listen(row, "pointerdown", downs); // row is in the global document
    const state = createRecordDragState();
    const origDpr = window.devicePixelRatio;
    Object.defineProperty(window, "devicePixelRatio", { value: 2, configurable: true });

    applyRecordDrag({ x: 200, y: 100, press: true, ok: true }, state); // no opts -> default doc + dpr
    off();
    Object.defineProperty(window, "devicePixelRatio", { value: origDpr, configurable: true });

    expect(downs).toHaveLength(1);
    // device 200,100 / window.devicePixelRatio 2 -> CSS 100,50, on the global document's row.
    expect(downs[0]).toMatchObject({ clientX: 100, clientY: 50 });
    expect(downs[0]!.target).toBe(row);
  });

  it("resetRecordDrag uses the global document when opts.doc is omitted", () => {
    const cancels: PointerEvent[] = [];
    const off = listen(document, "pointercancel", cancels);
    const state = createRecordDragState();

    applyRecordDrag({ x: 10, y: 10, press: true, ok: true }, state); // arm (default doc)
    resetRecordDrag(state); // no opts -> default document
    off();

    expect(cancels).toHaveLength(1);
    expect(state.prevPress).toBe(false);
  });
});
