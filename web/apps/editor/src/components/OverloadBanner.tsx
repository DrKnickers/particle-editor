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
//
// motion: entrance/exit animate via .banner-animate
// (components.css, slow tier — fade + 6px slip from the top edge). The
// raw `overload ? <Body/> : null` mount unmounted instantly on clear,
// so the wrapper goes through the usePresence shim: on the falling
// edge the body stays mounted in data-state="closed" while banner-out
// plays, then unmounts on animationend (or the timeout fallback under
// reduced motion). Consequence: the viewport occlusion now outlives
// the latch by one ~150ms exit — invisible, and release still fires on
// unmount as before.

import { useEffect, useRef, useState } from "react";
import type { Bridge } from "@particle-editor/bridge-schema";
import { useViewportOcclusion } from "@/lib/viewport-occlusion";
import { usePresence } from "@/lib/use-presence";
import { fmtCount } from "@/lib/chain-load";

// EXIT_MS must equal --motion-slow-out (tokens.css). The +50ms slack
// lives inside usePresence.
const EXIT_MS = 150;

// REFUSAL_MS: how long the transient refusal banner stays visible.
// A second refusal event during this window restarts the timer.
const REFUSAL_MS = 5_000;

// Refusal state: the two numbers the copy needs.
// attemptedCount is on the wire but unused by the copy (spec §2.5).
type RefusalState = { estimated: number; cap: number };

function OverloadBannerBody({
  bridge,
  state,
  onAnimationEnd,
  refusal,
}: {
  bridge: Bridge;
  state: "open" | "closed";
  onAnimationEnd: () => void;
  refusal: RefusalState | null;
}) {
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
      data-state={state}
      data-variant={refusal !== null ? "refusal" : "latch"}
      // Filter on animationName: only banner-OUT may unmount — without
      // it the ENTRANCE animation's end would fire onAnimationEnd too
      // (harmless today since usePresence ignores it while visible, but
      // the filter keeps the contract explicit).
      onAnimationEnd={(e) => {
        if (e.animationName === "banner-out") onAnimationEnd();
      }}
      // pointer-events-none: purely informational — viewport input
      // (camera drags, wheel zoom) passes straight through to the
      // canvas underneath.
      // banner-animate also supplies the soft shadow (--shadow-soft)
      // that replaced shadow-xl ring-1 ring-black/15.
      className="banner-animate pointer-events-none absolute left-1/2 top-3 z-20 -translate-x-1/2 select-none rounded-md bg-warning px-3 py-1.5 text-xs font-medium text-[#1a1200]"
    >
      {refusal !== null
        ? `Spawn blocked — this effect is estimated at ~${fmtCount(refusal.estimated)} particles, over the ${fmtCount(refusal.cap)} preview limit. Preview cleared.`
        : <>Preview spawning limited — lower spawn rates to resume. ⚠ marks heavy emitters.</>}
    </div>
  );
}

export function OverloadBanner({ bridge }: { bridge: Bridge }) {
  const [overload, setOverload] = useState(false);
  const [refusal, setRefusal] = useState<RefusalState | null>(null);
  // Track the active refusal timeout so we can restart it on re-fire
  const refusalTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(
    () => bridge.on("stats/tick", (e) => setOverload(e.payload.overload)),
    [bridge],
  );

  useEffect(() => {
    const unsub = bridge.on("engine/overload/refused", (e) => {
      const { estimated, cap } = e.payload as { estimated: number; cap: number; attemptedCount: number };
      // Restart the window if a second refusal arrives mid-flight
      if (refusalTimerRef.current !== null) {
        clearTimeout(refusalTimerRef.current);
      }
      setRefusal({ estimated, cap });
      // A refusal means the engine Clear()'d the preview — its latch reset
      // with it (engine.cpp Clear()), so any web-held `overload` is stale.
      // Force-clear it; a GENUINELY still-latched engine re-asserts via the
      // next 4 Hz tick (≤250 ms). Kills the delivery-order race outright.
      setOverload(false);
      refusalTimerRef.current = setTimeout(() => {
        refusalTimerRef.current = null;
        setRefusal(null);
      }, REFUSAL_MS);
    });
    return () => {
      unsub();
      // Clean up any pending timer on unmount
      if (refusalTimerRef.current !== null) {
        clearTimeout(refusalTimerRef.current);
        refusalTimerRef.current = null;
      }
    };
  }, [bridge]);

  // The banner is visible when a refusal is active OR when the latch is set.
  // usePresence drives the exit animation for both cases off a single boolean.
  const visible = refusal !== null || overload;
  // [s37 bug] Freeze the rendered variant for the exit: when the refusal
  // window expires and visible drops, usePresence keeps the body mounted
  // for the 150 ms fade — and the raw `refusal ?? latch` ternary would
  // fall through to the LATCH copy for the whole fade. While visible,
  // track the live choice; while exiting, render what was shown last.
  // (Render-time ref write is safe: idempotent "last rendered value".)
  const lastShownRef = useRef<RefusalState | null>(null);
  if (visible) lastShownRef.current = refusal;
  // A null frozen value is valid: it means the latch copy (not a refusal)
  // was on screen when the exit began, so the body renders the latch copy.
  const shownRefusal = visible ? refusal : lastShownRef.current;
  const { mounted, state, onAnimationEnd } = usePresence(visible, EXIT_MS);
  if (!mounted) return null;
  return (
    <OverloadBannerBody
      bridge={bridge}
      state={state}
      onAnimationEnd={onAnimationEnd}
      refusal={shownRefusal}
    />
  );
}
