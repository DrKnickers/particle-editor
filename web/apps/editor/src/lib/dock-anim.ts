// dock-anim.ts — a tiny zustand signal channel for the right-dock slide.
//
// Item 3 (dock-slide viewport stutter). Under, PanelLayout drives a
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
// monitor swap mid-slide is not dropped. The signal is set ONLY under;
// under --legacy it stays false and ViewportSlot keeps its per-frame sends.
import { create } from "zustand";

type DockAnimStore = {
  /** True while a host-interpolated dock slide is in flight (only). */
  animating: boolean;
  setAnimating: (v: boolean) => void;
};

export const useDockAnim = create<DockAnimStore>((set) => ({
  animating: false,
  setAnimating: (v) => set({ animating: v }),
}));
