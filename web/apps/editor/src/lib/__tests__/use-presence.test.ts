// Vitest: usePresence () — the presence shim that keeps a
// custom-unmounted element (`cond ? <El/> : null`) mounted through its
// CSS exit animation. Unmount fires on animationend OR a timeout
// fallback (exitMs + 50ms slack) so reduced-motion (`animation: none`,
// no animationend) can never leak a mounted ghost.

import { describe, it, expect, vi, beforeEach, afterEach } from "vitest";
import { renderHook, act } from "@testing-library/react";
import { usePresence } from "../use-presence";

describe("usePresence", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("mounts immediately on a rising edge", () => {
    const { result, rerender } = renderHook(({ v }) => usePresence(v, 150), { initialProps: { v: false } });
    expect(result.current.mounted).toBe(false);
    rerender({ v: true });
    expect(result.current.mounted).toBe(true);
    expect(result.current.state).toBe("open");
  });

  it("stays mounted in state=closed during exit, unmounts on onAnimationEnd", () => {
    const { result, rerender } = renderHook(({ v }) => usePresence(v, 150), { initialProps: { v: true } });
    rerender({ v: false });
    expect(result.current.mounted).toBe(true);
    expect(result.current.state).toBe("closed");
    act(() => result.current.onAnimationEnd());
    expect(result.current.mounted).toBe(false);
  });

  it("unmounts via the timeout fallback when no animationend arrives (reduced motion)", () => {
    const { result, rerender } = renderHook(({ v }) => usePresence(v, 150), { initialProps: { v: true } });
    rerender({ v: false });
    expect(result.current.mounted).toBe(true);
    act(() => vi.advanceTimersByTime(150 + 50 + 1));
    expect(result.current.mounted).toBe(false);
  });

  it("re-latch mid-exit cancels the unmount", () => {
    const { result, rerender } = renderHook(({ v }) => usePresence(v, 150), { initialProps: { v: true } });
    rerender({ v: false });
    rerender({ v: true }); // overload flickers back during the exit
    act(() => vi.advanceTimersByTime(1000));
    expect(result.current.mounted).toBe(true);
    expect(result.current.state).toBe("open");
  });
});
