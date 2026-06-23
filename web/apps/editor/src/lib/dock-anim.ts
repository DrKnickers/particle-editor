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
  /** True exactly while the Atlas picker's cell grid is mounted in the DOM.
   *  AtlasPickerPanel sets it; PanelLayout's OPEN-slide effect (atlas dock only)
   *  gates the slide START on the false→true edge so a COLD first open waits for
   *  the grid to render before the tween, avoiding the mid-slide centre-column
   *  overlap. A max-timeout fallback in PanelLayout starts the slide regardless,
   *  so this can never hang the slide if the grid never mounts. */
  atlasReady: boolean;
  setAtlasReady: (v: boolean) => void;
};

export const useDockAnim = create<DockAnimStore>((set) => ({
  animating: false,
  setAnimating: (v) => set({ animating: v }),
  atlasReady: false,
  setAtlasReady: (v) => set({ atlasReady: v }),
}));
