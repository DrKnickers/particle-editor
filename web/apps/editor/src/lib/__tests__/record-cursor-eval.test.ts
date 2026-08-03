import { afterEach, describe, expect, it } from "vitest";
import type { CursorElementRef, RecordCursorKey } from "../record-cursor-track";
import { evalRecordCursor, resolveTargetCenter } from "../record-cursor-eval";
import { useDockAnim } from "../dock-anim";

function pointKey(t: number, x: number, y: number, vis: boolean, press: boolean): RecordCursorKey {
  return { t, vis, press, activate: false, target: { kind: "point", x, y } };
}

function elementKey(t: number, ref: string, vis: boolean, press: boolean): RecordCursorKey {
  return { t, vis, press, activate: false, target: { kind: "element", ref: ref as CursorElementRef } };
}

/** Mount a div with the given attributes and a fixed client rect (jsdom's
 *  default getBoundingClientRect is all-zeros, so stub it per element). */
function addEl(
  attrs: Record<string, string>,
  rect: { left: number; top: number; width: number; height: number },
): HTMLElement {
  const el = document.createElement("div");
  for (const [k, v] of Object.entries(attrs)) el.setAttribute(k, v);
  el.getBoundingClientRect = () =>
    ({
      left: rect.left,
      top: rect.top,
      width: rect.width,
      height: rect.height,
      right: rect.left + rect.width,
      bottom: rect.top + rect.height,
      x: rect.left,
      y: rect.top,
      toJSON: () => ({}),
    }) as DOMRect;
  document.body.appendChild(el);
  return el;
}

describe("record-cursor-eval", () => {
  const keys = [
    pointKey(0, 0, 0, true, false),
    pointKey(1000, 100, 200, false, true),
  ];

  it("smoothstep-eases between point targets", () => {
    expect(evalRecordCursor(keys, 500)).toMatchObject({
      x: 50,
      y: 100,
      vis: false,
      press: true,
      ok: true,
      resolved: [],
    });
    expect(evalRecordCursor(keys, 750).x).toBeCloseTo(84.375);
    expect(evalRecordCursor(keys, 750).y).toBeCloseTo(168.75);
  });

  it("steps visibility and press from the upcoming key", () => {
    expect(evalRecordCursor(keys, 1)).toMatchObject({ vis: false, press: true });
  });

  it("carries mods/button on the UNRESOLVED-endpoint fallbacks, not just the resolved return", () => {
    // The `ar.ok` fallback stays ok:true, so it forwards a pressed, activating
    // cursor. Dropping the authored fields there silently degrades a right-click or
    // a Ctrl-click into a plain left click for as long as the upcoming endpoint is
    // unresolved — a context menu would never open and the clip records a no-op.
    const from = pointKey(0, 0, 0, true, false);
    const to: RecordCursorKey = {
      t: 1000,
      vis: true,
      press: true,
      activate: true,
      mods: { ctrl: true, shift: false },
      button: "right",
      target: { kind: "element", ref: "testid:not-mounted" as CursorElementRef },
    };

    // mid-transit: `to` cannot resolve, so this takes the ar.ok fallback (ok:true).
    const mid = evalRecordCursor([from, to], 500);
    expect(mid.ok).toBe(true);
    expect(mid.press).toBe(true);
    expect(mid.activate).toBe(true);
    expect(mid.button).toBe("right");
    expect(mid.mods).toEqual({ ctrl: true, shift: false });

    // and the both-unresolved fallback keeps them too
    const bothBad: RecordCursorKey = { ...from, target: { kind: "element", ref: "testid:also-missing" as CursorElementRef } };
    const none = evalRecordCursor([bothBad, to], 500);
    expect(none.ok).toBe(false);
    expect(none.button).toBe("right");
    expect(none.mods).toEqual({ ctrl: true, shift: false });
  });

  it("clamps at both ends", () => {
    expect(evalRecordCursor(keys, -1)).toMatchObject({ x: 0, y: 0, vis: true, press: false, ok: true });
    expect(evalRecordCursor(keys, 1001)).toMatchObject({ x: 100, y: 200, vis: false, press: true, ok: true });
  });

  it("uses a single-key track as a held cursor", () => {
    expect(evalRecordCursor([pointKey(42, 12, 34, true, true)], 1000)).toMatchObject({
      x: 12,
      y: 34,
      vis: true,
      press: true,
      ok: true,
      resolved: [],
    });
  });

  // SHARED PARITY VECTOR — must stay identical to the C++ EvalCursor vector in
  // tests/test_clip_timeline.cpp (the "cursor easing parity vector" block). If the
  // smoothstep / vis-press-stepping ever diverges between the host and the web, one
  // of these two tests fails. Keys: (0,0,0,f,f),(1000,100,200,t,f),(2000,200,100,t,t).
  it("matches the C++ EvalCursor parity vector", () => {
    const v = [
      pointKey(0, 0, 0, false, false),
      pointKey(1000, 100, 200, true, false),
      pointKey(2000, 200, 100, true, true),
    ];
    const cases: Array<[number, number, number, boolean, boolean]> = [
      [0, 0, 0, false, false],
      [250, 15.625, 31.25, true, false],
      [500, 50, 100, true, false],
      [1250, 115.625, 184.375, true, true],
      [2500, 200, 100, true, true],
    ];
    for (const [t, x, y, vis, press] of cases) {
      const c = evalRecordCursor(v, t);
      expect(c.x).toBeCloseTo(x);
      expect(c.y).toBeCloseTo(y);
      expect(c.vis).toBe(vis);
      expect(c.press).toBe(press);
    }
  });

  it("returns the hidden NaN sentinel for an empty key list", () => {
    const c = evalRecordCursor([], 500);
    expect(c.x).toBeNaN();
    expect(c.y).toBeNaN();
    expect(c).toMatchObject({ ok: false, vis: false, press: false, resolved: [] });
  });

  it("sorts unsorted keys by time before evaluating", () => {
    const unsorted = [pointKey(1000, 100, 200, false, true), pointKey(0, 0, 0, true, false)];
    expect(evalRecordCursor(unsorted, 500)).toMatchObject({ x: 50, y: 100, ok: true });
    expect(evalRecordCursor(unsorted, -1)).toMatchObject({ x: 0, y: 0, vis: true });
  });
});

describe("record-cursor-eval — element target resolution", () => {
  afterEach(() => {
    document.body.innerHTML = "";
    useDockAnim.setState({ animating: false, atlasGridMounted: false });
  });

  it("resolves a point target as-is", () => {
    expect(resolveTargetCenter({ kind: "point", x: 3, y: 4 })).toEqual({ x: 3, y: 4, ok: true });
  });

  it("resolves testid refs to the element's center, including ids that contain ':'", () => {
    addEl({ "data-testid": "toolbar-save" }, { left: 10, top: 20, width: 30, height: 40 });
    addEl({ "data-testid": "panel:atlas:open" }, { left: 100, top: 0, width: 10, height: 10 });
    expect(resolveTargetCenter({ kind: "element", ref: "testid:toolbar-save" })).toEqual({
      x: 25,
      y: 40,
      ok: true,
    });
    expect(resolveTargetCenter({ kind: "element", ref: "testid:panel:atlas:open" })).toEqual({
      x: 105,
      y: 5,
      ok: true,
    });
  });

  it("resolves curve-key and channel-row refs via their data-attribute selectors", () => {
    addEl(
      { "data-testid": "curve-key", "data-channel-id": "red", "data-key-time": "0.5" },
      { left: 0, top: 0, width: 8, height: 8 },
    );
    addEl({ "data-testid": "curve-channel-row-alpha" }, { left: 50, top: 60, width: 20, height: 10 });
    expect(resolveTargetCenter({ kind: "element", ref: "curve-key:red:0.5" })).toEqual({
      x: 4,
      y: 4,
      ok: true,
    });
    expect(resolveTargetCenter({ kind: "element", ref: "channel-row:alpha" })).toEqual({
      x: 60,
      y: 65,
      ok: true,
    });
  });

  it("rejects an element whose center is clipped outside a scroll container", () => {
    const scroller = addEl(
      { "data-testid": "curve-channel-list" },
      { left: 20, top: 20, width: 100, height: 80 },
    );
    scroller.style.overflowY = "auto";
    const clippedRow = addEl(
      { "data-testid": "curve-channel-row-scale" },
      { left: 20, top: 120, width: 80, height: 20 },
    );
    scroller.appendChild(clippedRow);

    const resolved = resolveTargetCenter({ kind: "element", ref: "channel-row:scale" });
    expect(resolved.ok).toBe(false);
    expect(resolved.x).toBeNaN();
    expect(resolved.y).toBeNaN();
  });

  it("targets the visible slice of a partially clipped element", () => {
    const scroller = addEl(
      { "data-testid": "curve-channel-list" },
      { left: 20, top: 20, width: 100, height: 80 },
    );
    scroller.style.overflowY = "auto";
    const partialRow = addEl(
      { "data-testid": "curve-channel-row-scale" },
      { left: 20, top: 90, width: 80, height: 20 },
    );
    scroller.appendChild(partialRow);

    expect(resolveTargetCenter({ kind: "element", ref: "channel-row:scale" })).toEqual({
      x: 60,
      y: 95,
      ok: true,
    });
  });

  it("returns unresolved for malformed refs and for elements missing from the DOM", () => {
    for (const ref of ["bogus:thing", "curve-key:only-channel", "testid:", "no-colon-at-all"]) {
      const r = resolveTargetCenter({ kind: "element", ref: ref as CursorElementRef });
      expect(r.ok).toBe(false);
      expect(r.x).toBeNaN();
      expect(r.y).toBeNaN();
    }
    // Well-formed ref, but nothing mounted.
    expect(resolveTargetCenter({ kind: "element", ref: "testid:not-mounted" }).ok).toBe(false);
  });

  it("gates atlas-tile refs on the dock being settled (grid mounted AND not animating)", () => {
    // Post-#572 the grid is a single <canvas>; the resolver reads its geometry
    // from the data-attrs (no per-cell DOM) and computes frame N's center.
    addEl(
      {
        "data-testid": "atlas-canvas",
        "data-atlas-cols": "4",
        "data-atlas-cell": "40",
        "data-atlas-gap": "4",
        "data-atlas-total": "16",
      },
      { left: 200, top: 100, width: 172, height: 40 }, // 4*40 + 3*4 = 172
    );
    const ref = { kind: "element", ref: "atlas-tile:3" } as const;
    // Grid not mounted → unresolved even though the canvas exists.
    useDockAnim.setState({ animating: false, atlasGridMounted: false });
    expect(resolveTargetCenter(ref).ok).toBe(false);
    // Mounted but the dock slide is still animating → unresolved.
    useDockAnim.setState({ animating: true, atlasGridMounted: true });
    expect(resolveTargetCenter(ref).ok).toBe(false);
    // Settled → resolves to frame 3's center. col=3, row=0, step=44 →
    // client (200 + 3*44 + 40/2, 100 + 0 + 40/2) = (352, 120), ×dpr(=1).
    useDockAnim.setState({ animating: false, atlasGridMounted: true });
    expect(resolveTargetCenter(ref)).toEqual({ x: 352, y: 120, ok: true });
  });

  it("holds a resolved element key at its center and reports it in `resolved`", () => {
    addEl({ "data-testid": "target-a" }, { left: 10, top: 10, width: 10, height: 10 });
    const c = evalRecordCursor([elementKey(100, "testid:target-a", true, true)], 500);
    expect(c).toMatchObject({ x: 15, y: 15, ok: true, vis: true, press: true });
    expect(c.resolved).toEqual([{ ref: "testid:target-a", x: 15, y: 15, ok: true }]);
  });

  it("FORCES vis:false when a held key's element can't resolve, reporting the miss", () => {
    const c = evalRecordCursor([elementKey(100, "testid:vanished", true, true)], 0);
    expect(c).toMatchObject({ x: 0, y: 0, ok: false, vis: false, press: true });
    expect(c.resolved).toHaveLength(1);
    expect(c.resolved[0]).toMatchObject({ ref: "testid:vanished", ok: false });
  });

  it("interpolates between two resolved element keys and reports only the upcoming key", () => {
    addEl({ "data-testid": "from" }, { left: 0, top: 0, width: 20, height: 20 }); // center (10,10)
    addEl({ "data-testid": "to" }, { left: 100, top: 100, width: 20, height: 20 }); // center (110,110)
    const keys = [elementKey(0, "testid:from", true, false), elementKey(1000, "testid:to", true, true)];
    const c = evalRecordCursor(keys, 500); // smoothstep(0.5) = 0.5
    expect(c.x).toBeCloseTo(60);
    expect(c.y).toBeCloseTo(60);
    expect(c).toMatchObject({ ok: true, vis: true, press: true });
    expect(c.resolved).toEqual([{ ref: "testid:to", x: 110, y: 110, ok: true }]);
  });

  it("holds at the resolved departure end when the upcoming target isn't ready yet", () => {
    addEl({ "data-testid": "from" }, { left: 0, top: 0, width: 20, height: 20 });
    const keys = [
      elementKey(0, "testid:from", true, false),
      elementKey(1000, "testid:not-yet-mounted", true, false),
    ];
    const c = evalRecordCursor(keys, 500);
    expect(c).toMatchObject({ x: 10, y: 10, ok: true, vis: true, press: false });
    expect(c.resolved).toEqual([expect.objectContaining({ ref: "testid:not-yet-mounted", ok: false })]);
  });

  // Pins current behavior: `resolved` carries the DESTINATION entry (ok:true);
  // the departure's failure surfaces only as the frame's top-level ok:false.
  // (The source comment at record-cursor-eval.ts:136-141 was corrected to
  // match — it used to claim a per-entry ok:false report that never existed.)
  it("heads to the destination (ok:false) when the departure element vanished mid-transit", () => {
    addEl({ "data-testid": "to" }, { left: 100, top: 100, width: 20, height: 20 });
    const keys = [
      elementKey(0, "testid:vanished", true, false),
      elementKey(1000, "testid:to", true, false),
    ];
    const c = evalRecordCursor(keys, 500);
    expect(c).toMatchObject({ x: 110, y: 110, ok: false, vis: true, press: false });
    expect(c.resolved).toEqual([{ ref: "testid:to", x: 110, y: 110, ok: true }]);
  });

  it("hides the cursor when neither transit end resolves", () => {
    const keys = [
      elementKey(0, "testid:gone-a", true, false),
      elementKey(1000, "testid:gone-b", true, true),
    ];
    const c = evalRecordCursor(keys, 500);
    expect(c).toMatchObject({ x: 0, y: 0, ok: false, vis: false, press: true });
    expect(c.resolved).toEqual([expect.objectContaining({ ref: "testid:gone-b", ok: false })]);
  });

  it("skips already-passed spans in a multi-key track (loop `continue` path)", () => {
    addEl({ "data-testid": "mid" }, { left: 40, top: 40, width: 20, height: 20 }); // center (50,50)
    const keys = [
      pointKey(0, 0, 0, true, false),
      elementKey(1000, "testid:mid", true, false),
      pointKey(2000, 200, 200, true, false),
    ];
    // t=1500 sits in the SECOND span; the first (0→1000) must be skipped.
    const c = evalRecordCursor(keys, 1500);
    expect(c.x).toBeCloseTo(50 + (200 - 50) * 0.5);
    expect(c.y).toBeCloseTo(50 + (200 - 50) * 0.5);
    expect(c.ok).toBe(true);
    expect(c.resolved).toEqual([]); // upcoming key is a point → no element report
  });
});
