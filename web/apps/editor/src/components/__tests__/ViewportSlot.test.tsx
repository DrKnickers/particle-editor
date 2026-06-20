// Vitest tests for ViewportSlot.
//
// Two surfaces locked down here:
//
// 1. The render surface. The slot mounts a transparent <canvas
//    data-testid="viewport-canvas"> as the input target; engine pixels
//    reach the screen via the DComp engine visual UNDER the WebView2,
//    not the DOM, so the canvas itself is never painted.
//
// 2. The bridge contract. ViewportSlot dispatches `layout/scene-rect`
//    on mount and forwards DOM pointer/keyboard input as
//    `viewport/input`; it never subscribes to `viewport/frame-ready`
//    (there is no DOM-side engine-pixel consumer).

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { fireEvent, render, screen, cleanup } from "@testing-library/react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { ViewportSlot } from "../ViewportSlot";
import { MK_LBUTTON, MK_SHIFT } from "../../lib/viewport-input";
import { useDockAnim } from "../../lib/dock-anim";

function makeStubBridge(): Bridge & {
  request: ReturnType<typeof vi.fn>;
  on: ReturnType<typeof vi.fn>;
} {
  const request = vi.fn().mockResolvedValue({});
  const on = vi.fn().mockReturnValue(() => {});
  return { request, on } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
}

describe("ViewportSlot — render surface", () => {
  afterEach(() => {
    cleanup();
  });

  it("mounts a <canvas data-testid='viewport-canvas'> as the input target", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    expect(canvas).toBeInTheDocument();
    expect(canvas.tagName).toBe("CANVAS");
  });

  it("dispatches layout/scene-rect on mount with DPR-scaled w/h", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    expect(bridge.request).toHaveBeenCalled();
    const firstCall = bridge.request.mock.calls[0]?.[0];
    expect(firstCall?.kind).toBe("layout/scene-rect");
    expect(firstCall?.params).toMatchObject({
      x: expect.any(Number),
      y: expect.any(Number),
      w: expect.any(Number),
      h: expect.any(Number),
    });
  });

  it("never subscribes to viewport/frame-ready (engine pixels come from the DComp visual, not the DOM)", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const subscribedKinds = bridge.on.mock.calls.map((c) => c[0]);
    expect(subscribedKinds).not.toContain("viewport/frame-ready");
  });
});

// ─── Phase 2 input forwarding ────────────────────────────────
//
// These tests assert the DOM event → bridge.request wiring. The pure-
// function encoders are exercised in lib/__tests__/viewport-input.test.ts;
// here we lock down that the listeners get attached on mount, fire the
// right Request kind + payload shape, and detach on unmount.

function findViewportInputCalls(
  bridge: { request: ReturnType<typeof vi.fn> },
): Array<{ kind: string; params: Record<string, unknown> }> {
  return bridge.request.mock.calls
    .map((c) => c[0] as { kind: string; params: Record<string, unknown> })
    .filter((req) => req.kind === "viewport/input");
}

describe("ViewportSlot — input forwarding", () => {
  afterEach(() => {
    cleanup();
  });

  it("pointerdown on canvas dispatches viewport/input { type: 'mousedown' }", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    fireEvent.pointerDown(canvas, {
      clientX: 100,
      clientY: 200,
      button: 0,
      buttons: 1,
      pointerId: 1,
    });
    const inputs = findViewportInputCalls(bridge);
    expect(inputs.length).toBeGreaterThan(0);
    expect(inputs[0]?.params).toMatchObject({
      type: "mousedown",
      button: "left",
      x: 100,
      y: 200,
      buttons: MK_LBUTTON,
    });
  });

  it("pointerdown with shiftKey encodes MK_SHIFT in buttons bitmask", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    fireEvent.pointerDown(canvas, {
      clientX: 0,
      clientY: 0,
      button: 0,
      buttons: 1,
      shiftKey: true,
      pointerId: 1,
    });
    const inputs = findViewportInputCalls(bridge);
    expect(inputs[0]?.params.buttons).toBe(MK_LBUTTON | MK_SHIFT);
  });

  it("pointermove on canvas dispatches viewport/input { type: 'mousemove' }", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    fireEvent.pointerMove(canvas, { clientX: 50, clientY: 75, buttons: 0 });
    const inputs = findViewportInputCalls(bridge);
    expect(inputs.some((r) => r.params.type === "mousemove")).toBe(true);
  });

  it("wheel on canvas dispatches viewport/input { type: 'wheel' } with sign-flipped delta", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    fireEvent.wheel(canvas, { clientX: 10, clientY: 10, deltaY: 100 });
    const inputs = findViewportInputCalls(bridge);
    const wheel = inputs.find((r) => r.params.type === "wheel");
    expect(wheel).toBeTruthy();
    expect(wheel?.params.deltaY).toBe(-120);  // DOM +100 → Win32 -WHEEL_DELTA
  });

  it("window keydown of VK_SHIFT (keyCode=16) dispatches viewport/input { type: 'keydown', vk: 16 }", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    fireEvent.keyDown(window, { keyCode: 16, key: "Shift" });
    const inputs = findViewportInputCalls(bridge);
    const key = inputs.find((r) => r.params.type === "keydown");
    expect(key).toBeTruthy();
    expect(key?.params).toMatchObject({ type: "keydown", vk: 16, repeat: false });
  });

  it("TYPING_TAGS guard: keydown with target=INPUT does NOT dispatch", () => {
    const bridge = makeStubBridge();
    render(
      <div>
        <input data-testid="text-input" />
        <ViewportSlot bridge={bridge} />
      </div>,
    );
    const input = screen.getByTestId("text-input");
    fireEvent.keyDown(input, { keyCode: 16, key: "Shift" });
    const inputs = findViewportInputCalls(bridge);
    expect(inputs.find((r) => r.params.type === "keydown")).toBeUndefined();
  });

  it("window.blur dispatches viewport/input { type: 'blur' }", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    fireEvent.blur(window);
    const inputs = findViewportInputCalls(bridge);
    expect(inputs.some((r) => r.params.type === "blur")).toBe(true);
  });
});

describe("ViewportSlot — dock-slide RO suppression (Item 3)", () => {
  // Capture the ResizeObserver callback so we can fire it manually — jsdom's
  // setup stub has a no-op observe(), so the RO never fires on its own. The
  // real RO mechanics aren't under test; the suppression GATE on the callback
  // is. scroll/resize stay on real window listeners (RO-only suppression).
  const RealRO = globalThis.ResizeObserver;
  let roCallbacks: Array<() => void>;

  beforeEach(() => {
    roCallbacks = [];
    globalThis.ResizeObserver = class {
      constructor(cb: () => void) {
        roCallbacks.push(cb);
      }
      observe() {}
      unobserve() {}
      disconnect() {}
    } as unknown as typeof ResizeObserver;
    useDockAnim.setState({ animating: false });
  });
  afterEach(() => {
    cleanup();
    globalThis.ResizeObserver = RealRO;
    useDockAnim.setState({ animating: false });
  });

  function sceneRectCalls(bridge: ReturnType<typeof makeStubBridge>): number {
    return bridge.request.mock.calls.filter((c) => c[0]?.kind === "layout/scene-rect").length;
  }

  it("the mount send fires even while a slide is animating (mount is never gated)", () => {
    useDockAnim.setState({ animating: true });
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    expect(sceneRectCalls(bridge)).toBeGreaterThanOrEqual(1);
  });

  it("suppresses the ResizeObserver callback while animating", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const before = sceneRectCalls(bridge);
    expect(roCallbacks.length).toBeGreaterThan(0);
    useDockAnim.setState({ animating: true }); // a resize lands mid-slide…
    roCallbacks.forEach((cb) => cb());
    expect(sceneRectCalls(bridge)).toBe(before); // …and is dropped
  });

  // [resize-perf C1] `send` dedupes on (rect, DPR) — in jsdom every
  // getBoundingClientRect is zeros, so the rect never changes and a repeat
  // fire is (correctly) dropped. Tests that assert a fire RESULTS in a send
  // must make the send unique; bumping devicePixelRatio (part of the dedupe
  // key by design — a monitor swap changes the backing size at an identical
  // CSS rect) is the lever jsdom lets us pull.
  let dprCounter = 1;
  function bumpDpr() {
    Object.defineProperty(window, "devicePixelRatio", {
      configurable: true,
      value: ++dprCounter,
    });
  }

  it("runs the ResizeObserver callback normally when not animating", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const before = sceneRectCalls(bridge);
    useDockAnim.setState({ animating: false });
    roCallbacks.forEach((cb) => {
      bumpDpr();
      cb();
    });
    expect(sceneRectCalls(bridge)).toBe(before + roCallbacks.length);
  });

  it("keeps scroll + resize sends live while animating (RO-only suppression)", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    useDockAnim.setState({ animating: true });
    const before = sceneRectCalls(bridge);
    bumpDpr();
    window.dispatchEvent(new Event("scroll"));
    bumpDpr();
    window.dispatchEvent(new Event("resize"));
    expect(sceneRectCalls(bridge)).toBe(before + 2);
  });

  it("dedupes repeat sends for an unchanged rect+DPR (C1)", () => {
    // The RO and the window-resize listener BOTH fire per window-resize
    // tick (a measured 2× send rate); with an unchanged rect the repeats
    // must be dropped, not re-sent.
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const before = sceneRectCalls(bridge);
    roCallbacks.forEach((cb) => cb());            // same rect as mount
    window.dispatchEvent(new Event("resize"));    // same rect again
    window.dispatchEvent(new Event("scroll"));    // and again
    expect(sceneRectCalls(bridge)).toBe(before);  // all deduped
  });
});
