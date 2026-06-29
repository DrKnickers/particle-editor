import { useEffect, useRef } from "react";
import type { Bridge, ViewportInputEvent } from "@particle-editor/bridge-schema";
import {
  blurEvent,
  isTypingTarget,
  makeKeyEvent,
  makeMouseEvent,
  makeWheelEvent,
} from "../lib/viewport-input";
import { computeSceneRect } from "../lib/scene-rect";
import { useDockAnim } from "../lib/dock-anim";
import { useModalOpen } from "../lib/modal-open";
import { ManipulatorReadout } from "./ManipulatorReadout";
import { ViewportToggleOverlay } from "./ViewportToggleOverlay";

type Props = { bridge: Bridge };

// Engine pixels reach the screen via the DXGI swapchain → DComp engine
// visual UNDER the WebView2 visual; transparent regions in the React app
// show the engine through. ViewportSlot owns two viewport responsibilities:
// it dispatches the `layout/scene-rect` the host crops the engine visual
// to, and forwards DOM pointer/keyboard input to the engine via the
// bridge. The scene-rect math lives in a shared lib (lib/scene-rect) so
// PanelLayout's dock-slide anim computes from/to with the SAME geometry as
// the rect ViewportSlot reports at rest. (The <canvas> here is purely an
// input event target — transparent, never painted; engine pixels come from
// the DComp visual, not the DOM.)

export function ViewportSlot({ bridge }: Props) {
  const ref = useRef<HTMLDivElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);

  // Dock-slide suppression signal. While PanelLayout runs a host-
  // interpolated viewport rect for the dock open/close slide (in this
  // architecture only),
  // the ResizeObserver below must NOT also fire per-frame scene-rects — that
  // clumpy stream is the very judder the host interpolation replaces. Mirror
  // the zustand signal into a ref so the mount-time RO callback can read the
  // latest value without re-subscribing on every change.
  const animatingRef = useRef(useDockAnim.getState().animating);
  useEffect(
    () => useDockAnim.subscribe((s) => { animatingRef.current = s.animating; }),
    [],
  );

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    // Last-sent dedupe. The same rect is computed by
    // multiple sources (the RO and the window-resize listener BOTH fire
    // for every window-resize tick — a measured 2× send rate), and
    // scroll events frequently leave the rect unchanged. Key includes
    // the DPR: a monitor swap can change the backing size while the
    // CSS rect stays identical, and that send must NOT be dropped.
    let lastSent = "";
    const send = () => {
      const { x, y, w, h } = computeSceneRect(el);
      const key = `${x},${y},${w},${h},${window.devicePixelRatio || 1}`;
      if (key === lastSent) return;
      lastSent = key;
      // The centre-quadrant rect drives the SCENE rect (the
      // visible sub-rect inside the popup), not the popup HWND itself. The
      // compositor crops the engine visual to it each frame — UI panels behind
      // the cropped-away bands show through (and receive their own mouse events).
      void bridge.request({ kind: "layout/scene-rect", params: { x, y, w, h } }).catch(() => {});
    };

    send();  // mount — always raw (the dock-anim signal is irrelevant on first paint)
    // RO is the ONLY source suppressed during a dock slide: the host owns the
    // viewport rect for the slide's duration, so a flood of RO scene-rects would
    // fight its smooth interpolation. scroll / resize / DPR stay raw below so a
    // real resize or monitor swap mid-slide is never dropped (the dedupe above
    // makes the overlap free instead of removing the safety listeners).
    const ro = new ResizeObserver(() => {
      if (animatingRef.current) return;
      send();
    });
    ro.observe(el);
    window.addEventListener("scroll", send, { passive: true });
    window.addEventListener("resize", send);

    // Phase 1.3: matchMedia('(resolution)') fires on DPR
    // changes (monitor swap, browser zoom), which don't trigger
    // ResizeObserver because the CSS-pixel rect is unchanged. We
    // re-dispatch the scene-rect at the new DPR so the host can
    // re-allocate the RT at the correct backing-store size. The
    // listener chain handles each successive DPR by re-binding after
    // every fire (mediaMatch returns a MediaQueryList for the *current*
    // DPR; once it changes, we need a new query for the *new* current
    // DPR to keep getting fired).
    let mql: MediaQueryList | null = null;
    // Track the active onChange in outer scope so cleanup can remove
    // it. (Pre-fix the cleanup nulled `mql` but the
    // active `change` listener stayed subscribed — one leaked listener
    // per component unmount, each holding the stale closure including
    // `send` and `bridge`.)
    let onChange: (() => void) | null = null;
    const bindDprListener = () => {
      const dpr = window.devicePixelRatio || 1;
      mql = window.matchMedia(`(resolution: ${dpr}dppx)`);
      onChange = () => {
        send();
        // Re-bind to the new DPR so we keep getting fires.
        if (mql && onChange) mql.removeEventListener("change", onChange);
        bindDprListener();
      };
      mql.addEventListener("change", onChange);
    };
    bindDprListener();

    return () => {
      ro.disconnect();
      window.removeEventListener("scroll", send);
      window.removeEventListener("resize", send);
      // Explicitly remove the active DPR listener.
      if (mql && onChange) mql.removeEventListener("change", onChange);
      mql = null;
      onChange = null;
    };
  }, [bridge]);

  // DOM input forwarding. Pointer + wheel events on the canvas,
  // keyboard + blur events on window, all forwarded to the engine via
  // the bridge. (Engine pixels arrive via the DComp visual, not the
  // DOM — there is no DOM-side engine-pixel consumer.)
  //
  // Coordinate convention: popup-client physical pixels = clientX/Y *
  // devicePixelRatio. The popup spans the full main client so
  // canvas-relative offsets aren't needed — clientX/Y already aligns
  // with the popup's client origin.
  //
  // Pointer capture: setPointerCapture on pointerdown means drag
  // gestures (LMB-rotate, MMB-pan, etc.) keep firing pointermove even
  // when the cursor leaves the canvas — critical for fast camera
  // motions that overshoot the viewport bounds.
  //
  // Wheel: native addEventListener with { passive: false }
  // (React 18 attaches wheel listeners as passive at the root, which
  // blocks preventDefault — the FieldSpinner pattern). We preventDefault
  // so the viewport region doesn't double-handle wheels as page scroll.
  //
  // Keyboard: window-scoped with TYPING_TAGS guard so typing in
  // inspector fields doesn't drive engine input. Forwards all keys —
  // the engine's viewport WNDPROC default-cases unknown VKs (only
  // VK_SHIFT is consumed today; broader forward is safe + forward-
  // compat for future engine hotkeys).
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;

    const send = (params: ViewportInputEvent) => {
      void bridge.request({ kind: "viewport/input", params }).catch(() => {});
    };

    const onPointerDown = (e: PointerEvent) => {
      try { canvas.setPointerCapture(e.pointerId); } catch { /* not supported */ }
      send(makeMouseEvent("mousedown", e, e.clientX, e.clientY));
    };
    const onPointerMove = (e: PointerEvent) => {
      send(makeMouseEvent("mousemove", e, e.clientX, e.clientY));
    };
    const onPointerUp = (e: PointerEvent) => {
      try { canvas.releasePointerCapture(e.pointerId); } catch { /* not held */ }
      send(makeMouseEvent("mouseup", e, e.clientX, e.clientY));
    };
    const onPointerCancel = (e: PointerEvent) => {
      try { canvas.releasePointerCapture(e.pointerId); } catch { /* not held */ }
      // pointercancel → synthesize a mouseup so the engine's drag state
      // unwinds (matches the WM_CAPTURECHANGED defensive cleanup at
      // HostWindow.cpp:1169).
      send(makeMouseEvent("mouseup", e, e.clientX, e.clientY));
    };
    // Disable the default browser context menu over the canvas so RMB-
    // drag isn't interrupted by a popup. The right-click event still
    // dispatches as pointerdown / pointerup with button=right.
    const onContextMenu = (e: Event) => { e.preventDefault(); };

    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      send(makeWheelEvent(e, e.clientX, e.clientY));
    };

    // Suppress global viewport keys while any blocking modal is open — otherwise a
    // key pressed with focus on a modal button drives the viewport behind it
    // (release-audit #12).
    const onKeyDown = (e: KeyboardEvent) => {
      if (isTypingTarget(e.target) || useModalOpen.getState().count > 0) return;
      send(makeKeyEvent("keydown", e));
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (isTypingTarget(e.target) || useModalOpen.getState().count > 0) return;
      send(makeKeyEvent("keyup", e));
    };

    const onBlur = () => {
      send(blurEvent);
    };

    // When a modal OPENS (count 0→1), end any cursor-bound Shift spawn before the
    // key-suppression above engages — opening a modal doesn't fire window.blur, so
    // without this the spawn would survive (and its kill-keyup would be suppressed).
    // (#7↔#12 integration.)
    let prevModalCount = useModalOpen.getState().count;
    const unsubModalOpen = useModalOpen.subscribe((s) => {
      if (prevModalCount === 0 && s.count > 0) send(blurEvent);
      prevModalCount = s.count;
    });

    canvas.addEventListener("pointerdown", onPointerDown);
    canvas.addEventListener("pointermove", onPointerMove);
    canvas.addEventListener("pointerup", onPointerUp);
    canvas.addEventListener("pointercancel", onPointerCancel);
    canvas.addEventListener("contextmenu", onContextMenu);
    canvas.addEventListener("wheel", onWheel, { passive: false });
    window.addEventListener("keydown", onKeyDown);
    window.addEventListener("keyup", onKeyUp);
    window.addEventListener("blur", onBlur);

    return () => {
      canvas.removeEventListener("pointerdown", onPointerDown);
      canvas.removeEventListener("pointermove", onPointerMove);
      canvas.removeEventListener("pointerup", onPointerUp);
      canvas.removeEventListener("pointercancel", onPointerCancel);
      canvas.removeEventListener("contextmenu", onContextMenu);
      canvas.removeEventListener("wheel", onWheel);
      window.removeEventListener("keydown", onKeyDown);
      window.removeEventListener("keyup", onKeyUp);
      window.removeEventListener("blur", onBlur);
      unsubModalOpen();
    };
  }, [bridge]);

  return (
    <div
      ref={ref}
      className="absolute inset-0 overflow-hidden bg-transparent flex items-center justify-center text-text-3 text-sm"
    >
      {/* Input layer. Transparent canvas overlay covering the viewport
          slot. Receives all pointer / wheel events for the viewport —
          its drawing buffer is intentionally never painted (stays at the
          default 300×150 transparent black); engine pixels come from the
          DComp visual beneath the WebView2, not the DOM. The data-testid
          is preserved for backward compatibility with existing vitest +
          Playwright specs that look up the input target by this name. */}
      <canvas
        ref={canvasRef}
        data-testid="viewport-canvas"
        className="absolute inset-0 w-full h-full"
      />
      {/* In-drag readout pill. Floats up-right of the projected
          gizmo origin while a reference-object gizmo is being dragged.
          `pointer-events-none` (set on the pill itself) so it never steals
          input from the canvas overlay below it. `ref` is this root div —
          the overlay box the pill positions within. */}
      <ManipulatorReadout bridge={bridge} overlayRef={ref} />
      {/* Viewport display-options overlay (ground/grid/bloom/reference-lock),
          bottom-left. `pointer-events-auto` (on .vp-overlay) so its buttons are
          clickable over the canvas; it's diagonally opposite the readout pill. */}
      <ViewportToggleOverlay bridge={bridge} />
    </div>
  );
}
