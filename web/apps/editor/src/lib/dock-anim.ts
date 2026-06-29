// dock-anim.ts — a tiny zustand signal channel for the right-dock slide.
//
// Item 3 (dock-slide viewport stutter). PanelLayout drives a
// host-side time-interpolated viewport rect during the open/close slide (the
// host re-renders at a wall-clock-lerped rect each frame, synced to the CSS
// flex-grow tween). While that interpolation is in flight, ViewportSlot's
// ResizeObserver would otherwise fire a clumpy stream of `layout/scene-rect`
// messages — the exact multi-clock judder the fix removes — so they must be
// SUPPRESSED for the slide's duration. The host also self-defends (ignores
// stray scene-rects mid-anim), but suppressing at the source keeps the IPC
// quiet and the intent legible.
//
// This store is the cross-component signal: PanelLayout sets `animating` true
// for the slide, ViewportSlot reads it (via a ref synced through subscribe) to
// gate ONLY its ResizeObserver callback. Suppression is RO-ONLY — scroll /
// window-resize / DPR-change sends stay live so a concurrent real resize or
// monitor swap mid-slide is not dropped. The signal is set whenever a dock
// slide animates (any bridge — it's a pure CSS/React tween); when no slide is
// animating ViewportSlot keeps its per-frame sends. The host-side rect
// interpolation the signal coordinates is itself a no-op when no DComp
// compositor is attached.
import { create } from "zustand";

type DockAnimStore = {
  /** True while a host-interpolated dock slide is in flight. */
  animating: boolean;
  setAnimating: (v: boolean) => void;
  /** Atlas readiness, SPLIT (perf-audit P1b first-open):
   *  - atlasTerminalFirstPaint: the picker has reached ANY terminal first-paint
   *    state — the cell grid OR a placeholder (missing / broken / off-index /
   *    too-large / no-texture). PanelLayout's OPEN-slide effect (atlas dock only)
   *    gates the slide START on the false→true edge of THIS, so a placeholder/error
   *    open no longer waits for a full grid mount before the tween (the old
   *    behavior blocked the slide until the grid was up). A max-timeout fallback in
   *    PanelLayout still starts the slide regardless, so this can never hang it.
   *  - atlasGridMounted: true exactly while the cell grid is mounted in the DOM
   *    (the old atlasReady semantics), kept as a distinct grid-timing signal. */
  atlasTerminalFirstPaint: boolean;
  setAtlasTerminalFirstPaint: (v: boolean) => void;
  atlasGridMounted: boolean;
  setAtlasGridMounted: (v: boolean) => void;
};

export const useDockAnim = create<DockAnimStore>((set) => ({
  animating: false,
  setAnimating: (v) => set({ animating: v }),
  atlasTerminalFirstPaint: false,
  setAtlasTerminalFirstPaint: (v) => set({ atlasTerminalFirstPaint: v }),
  atlasGridMounted: false,
  setAtlasGridMounted: (v) => set({ atlasGridMounted: v }),
}));
