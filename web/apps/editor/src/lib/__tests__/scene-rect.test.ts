import { afterEach, describe, expect, it } from "vitest";
import { computeSceneRect, dockSlideTarget, type SceneRect } from "@/lib/scene-rect";

function fakeEl(rect: Partial<DOMRect>): HTMLElement {
  const base = { left: 0, top: 0, width: 0, height: 0, right: 0, bottom: 0, x: 0, y: 0 };
  return {
    getBoundingClientRect: () => ({ ...base, ...rect }) as DOMRect,
  } as unknown as HTMLElement;
}

describe("computeSceneRect", () => {
  const orig = window.devicePixelRatio;
  function setDpr(v: number) {
    Object.defineProperty(window, "devicePixelRatio", { configurable: true, value: v });
  }
  afterEach(() => {
    Object.defineProperty(window, "devicePixelRatio", { configurable: true, value: orig });
  });

  it("at dpr=1 is the rounded client box", () => {
    setDpr(1);
    expect(
      computeSceneRect(fakeEl({ left: 335.4, top: 71.6, width: 658.2, height: 500.9 })),
    ).toEqual({ x: 335, y: 72, w: 658, h: 501 });
  });

  it("scales by dpr and rounds (dpr=2)", () => {
    setDpr(2);
    expect(
      computeSceneRect(fakeEl({ left: 100, top: 50, width: 300, height: 200 })),
    ).toEqual({ x: 200, y: 100, w: 600, h: 400 });
  });

  it("clamps negative width/height to 0", () => {
    setDpr(1);
    expect(
      computeSceneRect(fakeEl({ left: 10, top: 10, width: -5, height: -3 })),
    ).toEqual({ x: 10, y: 10, w: 0, h: 0 });
  });

  it("treats a missing/zero devicePixelRatio as 1", () => {
    setDpr(0);
    expect(computeSceneRect(fakeEl({ width: 640, height: 480 })).w).toBe(640);
  });
});

describe("dockSlideTarget", () => {
  // The measured test window: viewport 658 (dock open) ↔ 918 (dock closed),
  // Δ = 260 = the dock's min width (Phase-0).
  const from: SceneRect = { x: 100, y: 20, w: 658, h: 500 };

  it("OPEN shrinks width by the dock width; x/y/h fixed", () => {
    expect(dockSlideTarget(from, 260, true)).toEqual({ x: 100, y: 20, w: 398, h: 500 });
  });

  it("CLOSE grows width by the dock width", () => {
    expect(dockSlideTarget({ x: 100, y: 20, w: 658, h: 500 }, 260, false)).toEqual({
      x: 100,
      y: 20,
      w: 918,
      h: 500,
    });
  });

  it("clamps width to >= 0 when the dock is wider than the viewport", () => {
    expect(dockSlideTarget({ x: 0, y: 0, w: 100, h: 10 }, 260, true).w).toBe(0);
  });
});
