// OverloadBanner — preview spawn-overload guard (plan part 2 §3).
//
// The engine suppresses particle spawning when the live preview would
// exceed its hard budgets (global particle/instance budget OR a single
// emitter pinning its per-instance render cap) and latches an overload
// flag onto the 4 Hz `stats/tick` bridge event. While that flag is
// true, this banner floats over the viewport's top-center telling the
// user spawning is limited and how to recover; it auto-clears when the
// latch drops (population decays under budget after the user lowers
// the rate).
//
// Wording: the latch ALSO fires when one emitter pins its per-instance
// render cap — not only the global budget — so the copy says "spawning
// limited", never "budget exceeded".
//
// Styling: a FILLED amber pill (bg-warning is theme-independent #e0a14b)
// with fixed near-black text. The first cut used `text-amber-400` on the
// panel background, which was near-invisible in light mode (light yellow
// on off-white). A solid fill with dark text reads clearly in BOTH
// themes because the fill colour doesn't flip.
//
// Mount point: inside PanelLayout's `quadrant-viewport` container, as
// a sibling of ViewportSlot. That div is `relative` and exactly spans
// the viewport rect, so `absolute top-3 left-1/2` pins the banner to
// the viewport's top-center with zero extra geometry plumbing, and the
// PanelLayout render site already has `bridge` in scope.
//
// Plan risk 4: the banner overlaps the D3D-composited viewport popup —
// without registering a viewport occlusion the popup OVERPAINTS the
// DOM. The visible body therefore registers `useViewportOcclusion`
// (the OccludingContextMenuContent precedent in EmitterTree.tsx). The
// hook requires the element to exist when its effect runs (refs, not
// state), so the occluding body is a child component that mounts only
// while overloaded — mount registers the rect, unmount releases it.
//
// The browser MockBridge emits no stats/tick at all, so in browser dev
// the banner never appears; component tests drive it with a stub
// bridge.

import { useEffect, useRef, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useViewportOcclusion } from "@/lib/viewport-occlusion";

function OverloadBannerBody({ bridge }: { bridge: Bridge }) {
  const ref = useRef<HTMLDivElement | null>(null);
  // pad=12 / feather=12 — modest ring (smaller than the context menu's
  // 24: the banner has no shadow-xl spread to feather across).
  // observeParent: the banner is content-sized but centered with
  // left-1/2, so a splitter drag resizes the CONTAINER and moves the
  // banner without resizing it — without this the alpha cut-out goes
  // stale at the old coordinates and the viewport overpaints us.
  useViewportOcclusion(bridge, "banner:preview-overload", ref, 12, 12, true);
  return (
    <div
      ref={ref}
      role="status"
      aria-live="polite"
      data-testid="preview-overload-banner"
      // pointer-events-none: purely informational — viewport input
      // (camera drags, wheel zoom) passes straight through to the
      // canvas underneath.
      className="pointer-events-none absolute left-1/2 top-3 z-20 -translate-x-1/2 select-none rounded-md bg-warning px-3 py-1.5 text-xs font-medium text-[#1a1200] shadow-xl ring-1 ring-black/15"
    >
      Preview spawning limited — lower spawn rates to resume. ⚠ marks
      heavy emitters.
    </div>
  );
}

export function OverloadBanner({ bridge }: { bridge: Bridge }) {
  const [overload, setOverload] = useState(false);
  useEffect(
    () => bridge.on("stats/tick", (e) => setOverload(e.payload.overload)),
    [bridge],
  );
  if (!overload) return null;
  return <OverloadBannerBody bridge={bridge} />;
}
