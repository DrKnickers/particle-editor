// usePresence — keeps an element mounted through its CSS exit animation
// (built for OverloadBanner, generic for any custom-unmount
// surface). Radix components get this from Presence for free; this is
// the shim for `cond ? <El/> : null` mounts.
//
// The unmount fires on animationend OR a timeout fallback (exitMs +
// 50ms slack) — reduced-motion sets `animation: none`, which fires NO
// animationend, and a dropped event must never leak a mounted ghost.

import { useEffect, useRef, useState } from "react";

export function usePresence(visible: boolean, exitMs: number): {
  mounted: boolean;
  state: "open" | "closed";
  onAnimationEnd: () => void;
} {
  const [mounted, setMounted] = useState(visible);
  const timer = useRef<number | null>(null);

  useEffect(() => {
    if (visible) {
      // Rising edge (or re-latch mid-exit): cancel any pending unmount.
      if (timer.current !== null) {
        window.clearTimeout(timer.current);
        timer.current = null;
      }
      setMounted(true);
      return;
    }
    // Falling edge: let the exit animation play, then force-unmount.
    timer.current = window.setTimeout(() => {
      timer.current = null;
      setMounted(false);
    }, exitMs + 50);
    return () => {
      if (timer.current !== null) {
        window.clearTimeout(timer.current);
        timer.current = null;
      }
    };
  }, [visible, exitMs]);

  const onAnimationEnd = () => {
    if (!visible) {
      if (timer.current !== null) {
        window.clearTimeout(timer.current);
        timer.current = null;
      }
      setMounted(false);
    }
  };

  return { mounted, state: visible ? "open" : "closed", onAnimationEnd };
}
