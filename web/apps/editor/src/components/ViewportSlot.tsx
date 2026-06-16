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
import { isLegacyMode } from "../lib/hosting-mode";
import { useDockAnim } from "../lib/dock-anim";
import { ManipulatorReadout } from "./ManipulatorReadout";

type Props = { bridge: Bridge };

// Default rendering path is architecture C (DXGI composition
// + DComp engine visual + WebView2 composition hosting). Engine
// pixels reach the screen via DXGI swapchain → DComp engine visual
// UNDER the WebView2 visual; transparent regions in the React app
// show engine through. The frame-ready / <img>-decode pipeline is
// skipped (FramePublisher still publishes host-side — wasted work,
// kept until architecture A is deleted in a future cleanup).
//
// Opt out via VITE_HOSTING_MODE=legacy at build time → architecture A
// (legacy AlphaCompositor popup + HWND-hosted WebView2 + JPEG decode
// into <img>). Mirrors the runtime ALO_HOSTING_MODE check in
// HostWindow.cpp; a mismatch between build-time and runtime modes
// triggers the boot-time consistency banner (see App.tsx mode-claim).
// `isLegacyMode()` + the scene-rect math now live in shared libs
// (lib/hosting-mode, lib/scene-rect) so PanelLayout's dock-slide anim
// gates on the SAME hosting check and computes from/to with the SAME
// geometry as the rect ViewportSlot reports at rest.

export function ViewportSlot({ bridge }: Props) {
  const ref = useRef<HTMLDivElement | null>(null);
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  // Phase 2: an <img> element is the actual visual surface
  // (paints engine pixels via .src). The canvas above it is now purely
  // an input event target — transparent, no buffer painting. Splitting
  // these two responsibilities lets the browser handle the resize-
  // friendly atomic-decode-swap that canvas drawImage can't match.
  const imgRef = useRef<HTMLImageElement | null>(null);
  // Both flags derive from a single legacy-mode check. Under
  // default (architecture C), legacyMode=false → archCEnabled=true +
  // compositionMode=true. Under VITE_HOSTING_MODE=legacy, both go
  // false and the frame-ready subscription / canvas-jpeg path becomes
  // the active engine-pixel pipeline (architecture A). Kept as
  // distinct named aliases because each gates a conceptually
  // different thing (archCEnabled = JPEG transport active;
  // compositionMode = WebView2 composition hosting active); a future
  //-style cleanup that deletes architecture A can collapse
  // them.
  const legacyMode = isLegacyMode();
  const archCEnabled = !legacyMode;
  const compositionMode = !legacyMode;

  // [Item 3] Dock-slide suppression signal. While PanelLayout runs a host-
  // interpolated viewport rect for the dock open/close slide (only),
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

    // [resize-perf C1] Last-sent dedupe. The same rect is computed by
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
      // B1.4 T4c: the centre-quadrant rect drives the SCENE rect (the
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
    // it. (G6: pre-fix the cleanup nulled `mql` but the
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
      // G6: explicitly remove the active DPR listener.
      if (mql && onChange) mql.removeEventListener("change", onChange);
      mql = null;
      onChange = null;
    };
  }, [bridge]);

  // Phase 2 — paint engine frames into an <img> element via
  // .src assignment, rather than canvas drawImage. The <img> approach
  // has three properties that the canvas approach lacked:
  //
  //   1. NO BUFFER-CLEAR GAP. Canvas resizes (canvas.width=N) clear
  //      the drawing buffer to transparent before the next paint,
  //      revealing the page background as flicker. Setting img.src
  //      doesn't touch any persistent buffer — the browser keeps the
  //      previous decoded frame painted until the new one is ready,
  //      then atomically swaps.
  //
  //   2. NO ASPECT-DISTORTION WOBBLE. With a fixed-aspect canvas
  //      buffer being CSS-stretched to a slot whose aspect changes
  //      during pane drag, the content visibly squishes between
  //      frames. The <img> element's natural sizing model (intrinsic
  //      from the source data, CSS-fit via object-fit) handles
  //      aspect changes smoothly.
  //
  //   3. LIGHTER PER-FRAME COST. We avoid allocating a fresh Image()
  //      per frame (the previous pattern) AND the canvas drawImage
  //      step. The browser's image-decode pipeline handles the
  //      base64 → bitmap conversion on a background thread.
  //
  // The canvas overlay (still rendered above this <img>) is kept for
  // its DOM input event handling; its drawing buffer is never touched
  // and remains transparent forever. Skip entirely if the transport
  // flag isn't set; MockBridge (browser dev) never emits this event
  // so the subscription is a benign no-op.
  useEffect(() => {
    if (!archCEnabled) return;
    // Phase 3 Stage 4c.1 — skip frame-ready under composition
    // mode so DXGI engine pixels show through the transparent <img>.
    // FramePublisher continues publishing JPEG frames host-side (per
    // sub-plan §1 — wasted work, harmless until Stage 7 removal); we
    // just stop CONSUMING them. The canvas overlay's input listeners
    // (third useEffect below) stay active for the engine input path.
    if (compositionMode) {
      // eslint-disable-next-line no-console
      console.log("[ArchC] composition mode active — skipping viewport/frame-ready subscription (DXGI engine visual is the source)");
      return;
    }

    let cancelled = false;
    let lastLoggedAt = 0;
    let framesSinceLastLog = 0;

    const unsubscribe = bridge.on("viewport/frame-ready", (e) => {
      if (cancelled) return;
      const p = e.payload;
      if (!p?.jpegBase64) return;

      const img = imgRef.current;
      if (!img) return;  // jsdom or pre-mount — skip

      // Atomic swap. The browser keeps the previous frame painted
      // through the decode of this one, eliminating the buffer-clear
      // gap that was visible as resize flicker / wobble.
      img.src = `data:image/jpeg;base64,${p.jpegBase64}`;

      framesSinceLastLog += 1;
      const now = performance.now();
      if (now - lastLoggedAt > 1000) {
        // eslint-disable-next-line no-console
        console.log(`[ArchC] <img> painted ${framesSinceLastLog} frames in ${(now - lastLoggedAt).toFixed(0)}ms (size ${p.w}x${p.h}, b64 ${p.jpegBase64.length} chars)`);
        framesSinceLastLog = 0;
        lastLoggedAt = now;
      }
    });

    return () => {
      cancelled = true;
      unsubscribe();
    };
  }, [bridge, archCEnabled, compositionMode]);

  // Phase 2 — DOM input forwarding. Pointer + wheel events on
  // the canvas, keyboard + blur events on window. All gated on
  // archCEnabled: in legacy-popup mode the visible popup HWND receives
  // input directly from the OS, so the canvas must NOT intercept it.
  //
  // Coordinate convention: popup-client physical pixels = clientX/Y *
  // devicePixelRatio. The popup spans the full main client (T4c.4) so
  // canvas-relative offsets aren't needed — clientX/Y already aligns
  // with the popup's client origin.
  //
  // Pointer capture: setPointerCapture on pointerdown means drag
  // gestures (LMB-rotate, MMB-pan, etc.) keep firing pointermove even
  // when the cursor leaves the canvas — critical for fast camera
  // motions that overshoot the viewport bounds.
  //
  // Wheel: native addEventListener with { passive: false } per
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
    if (!archCEnabled) return;
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

    const onKeyDown = (e: KeyboardEvent) => {
      if (isTypingTarget(e.target)) return;
      send(makeKeyEvent("keydown", e));
    };
    const onKeyUp = (e: KeyboardEvent) => {
      if (isTypingTarget(e.target)) return;
      send(makeKeyEvent("keyup", e));
    };

    const onBlur = () => {
      send(blurEvent);
    };

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
    };
  }, [bridge, archCEnabled]);

  return (
    <div
      ref={ref}
      className="absolute inset-0 bg-transparent flex items-center justify-center text-text-3 text-sm"
    >
      {archCEnabled ? (
        <>
          {/* Phase 2: visual layer. Engine pixels arrive as
              base64 JPEG via the viewport/frame-ready bridge event and
              paint into this <img> via .src assignment. The browser
              handles atomic-decode-swap, so resize and aspect changes
              don't flicker. `pointer-events: none` so the canvas overlay
              above receives DOM input. `object-fit: fill` to stretch
              the image to the slot's CSS box (the source data already
              matches the scene rect dimensions emitted by the host).
              `draggable={false}` to suppress the default HTML5 drag-
              image behaviour on mousedown.

              Rendered ONLY in the canvas-jpeg path (`!compositionMode`).
              Under composition mode (the default) engine pixels
              reach the screen via the DComp engine visual UNDER the
              transparent WebView2 — the frame-ready → img.src consumer
              early-returns (above), so the <img> would never be painted.
              Worse, leaving it in the tree painted a 1px light-grey
              (#C0C0C0) hairline framing the viewport: the empty element's
              box edge sits at the fractional sub-pixel scene-rect origin
              (e.g. x=335.05 at dpr=1) and Chromium antialiased that edge
              against its white compositor base, producing a neutral,
              theme-independent ~50%-coverage grey at the viewport's first
              row/column on all four sides. Gating render on
              `!compositionMode` removes the dead element (and the seam)
              from the default path while preserving it for the
              canvas-jpeg transport. Proven by host-side engine-RT readback
              (engine clean at the edge) + a live elimination sweep
              (recolouring backing/engine-bg/page-bg left it unchanged;
              hiding this <img> removed it with the viewport interior
              pixel-identical). */}
          {!compositionMode && (
            <img
              ref={imgRef}
              data-testid="viewport-img"
              alt=""
              draggable={false}
              className="absolute inset-0 w-full h-full pointer-events-none"
              style={{ imageRendering: "pixelated", objectFit: "fill" }}
            />
          )}
          {/* Phase 2: input layer. Transparent canvas overlay
              on top of the <img>. Receives all pointer / wheel events
              for the viewport — its drawing buffer is intentionally
              never painted (stays at the default 300×150 transparent
              black). The data-testid is preserved for backward
              compatibility with existing vitest + Playwright specs
              that look up the input target by this name. */}
          <canvas
            ref={canvasRef}
            data-testid="viewport-canvas"
            className="absolute inset-0 w-full h-full"
          />
        </>
      ) : (
        /* The native D3D9 viewport sibling renders here. In browser-mode
           (MockBridge), the underlying body bg shows through.
           `absolute inset-0` fills the positioned parent — the
           quadrant-viewport <div> has `relative` so this stretches to
           its full rect. Without this, `flex-1` did nothing (parent
           isn't a flex container vertically) and the slot collapsed
           to the height of its content. */
        <span className="select-none pointer-events-none">D3D9 viewport</span>
      )}
      {/* In-drag readout pill. Floats up-right of the projected
          gizmo origin while a reference-object gizmo is being dragged.
          `pointer-events-none` (set on the pill itself) so it never steals
          input from the canvas overlay below it. `ref` is this root div —
          the overlay box the pill positions within. */}
      <ManipulatorReadout bridge={bridge} overlayRef={ref} />
    </div>
  );
}
