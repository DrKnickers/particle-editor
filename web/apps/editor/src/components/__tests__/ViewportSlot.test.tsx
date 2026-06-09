// Vitest tests for ViewportSlot — Phase 1.4 + flip.
//
// Two surfaces locked down here:
//
// 1. The dual render path. Under default (VITE_HOSTING_MODE unset or
//    anything other than "legacy"), the slot mounts a <canvas
//    data-testid="viewport-canvas"> ready for architecture-C engine
//    pixels (DXGI via DComp; the canvas overlay is the input target,
//    not the paint surface). Under VITE_HOSTING_MODE="legacy" the
//    slot renders the placeholder span (architecture A — pre-
//    default; legacy WS_EX_LAYERED popup paints engine pixels above
//    the WebView).
//
// 2. The bridge contract. ViewportSlot dispatches `layout/scene-rect`
//    on mount; under architecture C it also subscribes to
//    `viewport/frame-ready` and tears the subscription down on
//    unmount. (Architecture A skips the subscription — popup HWND
//    is the engine-pixel source, not the JPEG path.)
//
// The actual paint loop (Image() decode + drawImage) is exercised at
// runtime via the C++ host — see Phase 1.5 smoke notes in the
// archived todo. jsdom has neither a real canvas backing-store
// nor a real Image decoder, so unit-testing the paint path here
// would be all mocks and no real signal.
//
// The mode flag is read inside the component (via `isLegacyMode()`)
// so `vi.stubEnv` controls it per-test without any module-reset dance.

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

describe("ViewportSlot — legacy path (VITE_HOSTING_MODE='legacy')", () => {
  beforeEach(() => {
    vi.stubEnv("VITE_HOSTING_MODE", "legacy");
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllEnvs();
  });

  it("renders the 'D3D9 viewport' placeholder span, no canvas", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    expect(screen.getByText("D3D9 viewport")).toBeInTheDocument();
    expect(screen.queryByTestId("viewport-canvas")).not.toBeInTheDocument();
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

  it("does NOT subscribe to viewport/frame-ready in legacy mode", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const subscribedKinds = bridge.on.mock.calls.map((c) => c[0]);
    expect(subscribedKinds).not.toContain("viewport/frame-ready");
  });
});

describe("ViewportSlot — default path (VITE_HOSTING_MODE unset, architecture C)", () => {
  beforeEach(() => {
    vi.stubEnv("VITE_HOSTING_MODE", "");
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllEnvs();
  });

  it("mounts a <canvas data-testid='viewport-canvas'> instead of the placeholder", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const canvas = screen.getByTestId("viewport-canvas");
    expect(canvas).toBeInTheDocument();
    expect(canvas.tagName).toBe("CANVAS");
    expect(screen.queryByText("D3D9 viewport")).not.toBeInTheDocument();
  });

  it("does NOT subscribe to viewport/frame-ready under architecture C (DXGI is the engine-pixel source, not the JPEG path)", () => {
    // Pre-flip, this describe block stubbed
    // VITE_VIEWPORT_TRANSPORT="canvas-jpeg" without setting the
    // WEBVIEW2_HOSTING twin — that produced the intermediate
    // architecture-B state where ViewportSlot DID subscribe to
    // frame-ready and JPEG-decoded engine frames into the <img>.
    // Post-the single VITE_HOSTING_MODE env var collapses that
    // matrix: unset / non-"legacy" → full architecture C, where
    // engine pixels reach the screen via DXGI swapchain → DComp
    // engine visual UNDER the WebView2 visual, and the frame-ready
    // subscription is skipped (see ViewportSlot.tsx isLegacyMode +
    // the compositionMode early-return at the frame-ready effect).
    // The host-side FramePublisher continues publishing frames
    // (wasted work, kept until a future architecture-A deletion
    // dispatch); we just don't consume them.
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

describe("ViewportSlot — Phase 2 input forwarding (architecture C only)", () => {
  beforeEach(() => {
    vi.stubEnv("VITE_HOSTING_MODE", "");
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllEnvs();
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

  it("does NOT attach listeners in legacy mode (VITE_HOSTING_MODE='legacy')", () => {
    vi.stubEnv("VITE_HOSTING_MODE", "legacy");
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    // Canvas isn't even rendered; window-level keydown should be a no-op.
    fireEvent.keyDown(window, { keyCode: 16, key: "Shift" });
    const inputs = findViewportInputCalls(bridge);
    expect(inputs.length).toBe(0);
  });
});

describe("ViewportSlot — dock-slide RO suppression (Item 3, architecture C)", () => {
  // Capture the ResizeObserver callback so we can fire it manually — jsdom's
  // setup stub has a no-op observe(), so the RO never fires on its own. The
  // real RO mechanics aren't under test; the suppression GATE on the callback
  // is. scroll/resize stay on real window listeners (RO-only suppression).
  const RealRO = globalThis.ResizeObserver;
  let roCallbacks: Array<() => void>;

  beforeEach(() => {
    vi.stubEnv("VITE_HOSTING_MODE", ""); // architecture C
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
    vi.unstubAllEnvs();
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

  it("runs the ResizeObserver callback normally when not animating", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    const before = sceneRectCalls(bridge);
    useDockAnim.setState({ animating: false });
    roCallbacks.forEach((cb) => cb());
    expect(sceneRectCalls(bridge)).toBe(before + roCallbacks.length);
  });

  it("keeps scroll + resize sends live while animating (RO-only suppression)", () => {
    const bridge = makeStubBridge();
    render(<ViewportSlot bridge={bridge} />);
    useDockAnim.setState({ animating: true });
    const before = sceneRectCalls(bridge);
    window.dispatchEvent(new Event("scroll"));
    window.dispatchEvent(new Event("resize"));
    expect(sceneRectCalls(bridge)).toBe(before + 2);
  });
});
