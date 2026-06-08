// viewport-occlusion — React hook that tells the host "this DOM element
// overlaps the viewport popup, please cut it out so I show through."
//
// follow-up. The host (post-) renders the D3D9 viewport as a
// top-level WS_POPUP composited above WebView2's chrome. Without
// further work, any HTML element that drops into the viewport rect
// (tool panels, menu dropdowns, modals) appears BEHIND the popup.
// This hook sends `viewport/occlude` to the host with the element's
// current bounding rect; the host applies a SetWindowRgn cut-out on
// the popup so the HTML shows through.
//
// Usage:
//   const ref = useRef<HTMLDivElement>(null);
//   useViewportOcclusion(bridge, "tool-panel:background", ref);
//   return <div ref={ref}>…</div>;
//
// Lifecycle: on mount the hook starts observing the element's
// position via getBoundingClientRect + ResizeObserver. On unmount it
// sends a `rect: null` to release the occlusion. DPR multiplication
// matches ViewportSlot.tsx so the host receives physical pixels.
//
// Limitations: the element must be already mounted when the hook
// runs (use refs not state). If the element animates, the rect
// updates only when ResizeObserver fires — fine for Radix snapshots
// where animations are short but for long-running animations a
// per-frame poll would be needed.

import { useEffect, type RefObject } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";

export function useViewportOcclusion(
  bridge: Bridge | undefined,
  id: string,
  ref: RefObject<HTMLElement | null>,
  /** Extra CSS pixels added on each side of the bounding rect before
   *  sending.: pair this with the matching `featherPx` so the
   *  AlphaCompositor's smoothstep transitions across the padded ring
   *  (rather than leaving a region where alpha=0 exposes the parent
   *  HWND brush past the chrome's shadow extent). Default 0. */
  padPx: number = 0,
  /** Smoothstep feather width (CSS px) at the unclipped edges of the
   *  reported rect. Should typically equal `padPx` so the alpha
   *  smoothly transitions from viewport-opaque at the outer ring to
   *  full-cut at the chrome's actual outline. Default 0 (hard cut). */
  featherPx: number = 0,
) {
  useEffect(() => {
    const el = ref.current;
    if (!el || !bridge || !id) return;

    const send = () => {
      const r = el.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      void bridge
        .request({
          kind: "viewport/occlude",
          params: {
            id,
            rect: {
              x: Math.round((r.left - padPx) * dpr),
              y: Math.round((r.top  - padPx) * dpr),
              w: Math.round((r.width  + padPx * 2) * dpr),
              h: Math.round((r.height + padPx * 2) * dpr),
            },
            feather: Math.round(featherPx * dpr),
          },
        })
        .catch(() => {
          // Host may not yet have the dispatcher wired up (early in
          // boot). Ignore — the next ResizeObserver tick will retry.
        });
    };

    send();
    const ro = new ResizeObserver(send);
    ro.observe(el);
    window.addEventListener("scroll", send, { passive: true });
    window.addEventListener("resize", send);

    return () => {
      ro.disconnect();
      window.removeEventListener("scroll", send);
      window.removeEventListener("resize", send);
      // Release the occlusion on unmount.
      void bridge
        .request({
          kind: "viewport/occlude",
          params: { id, rect: null },
        })
        .catch(() => { /* ignore */ });
    };
  }, [bridge, id, ref, padPx, featherPx]);
}
