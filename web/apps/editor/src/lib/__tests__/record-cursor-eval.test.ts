import { describe, expect, it } from "vitest";
import type { RecordCursorKey } from "../record-cursor-track";
import { evalRecordCursor } from "../record-cursor-eval";

function pointKey(t: number, x: number, y: number, vis: boolean, press: boolean): RecordCursorKey {
  return { t, vis, press, target: { kind: "point", x, y } };
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
});
