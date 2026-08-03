// Vitest: OverloadBanner (preview spawn-overload guard).
// The banner subscribes to the 4 Hz stats/tick bridge event and shows a
// fixed warning over the viewport while `overload` is latched.

import { describe, it, expect, vi } from "vitest";
import { render, screen, act } from "@testing-library/react";
import { OverloadBanner } from "../OverloadBanner";
import type { Bridge } from "@particle-editor/bridge-schema";

// Stub bridge mirroring StatusBar.test.tsx's makeBridge: records `on`
// handlers by event name so the test can drive them; `request` records
// calls (the occlusion assertions read them) and resolves ok.
function makeBridge() {
  const handlers = new Map<string, (e: { payload: unknown }) => void>();
  const request = vi.fn().mockResolvedValue({ ok: true });
  const on = vi.fn().mockImplementation(
    (event: string, cb: (e: { payload: unknown }) => void) => {
      handlers.set(event, cb);
      return () => handlers.delete(event);
    },
  );
  const emit = (event: string, payload: unknown) => {
    act(() => handlers.get(event)?.({ payload }));
  };
  return { bridge: { request, on } as unknown as Bridge, emit, request, handlers };
}

const tick = (overload: boolean) => ({
  fps: 30, emitters: 1, particles: 16384, instances: 1, overload,
});

describe("OverloadBanner", () => {
  it("renders nothing before any stats/tick arrives", () => {
    const { bridge } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  });

  it("shows the banner with the spawn-limited copy when overload latches", () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner).toBeInTheDocument();
    expect(banner).toHaveAttribute("role", "status");
    expect(banner).toHaveAttribute("aria-live", "polite");
    // Wording nuance (review): the latch also fires when a single emitter
    // pins its per-instance render cap — so the copy says "spawning
    // limited", never "budget exceeded".
    expect(banner.textContent).toContain("Preview spawning limited");
    expect(banner.textContent).toContain("lower spawn rates");
    expect(banner.textContent).toContain("marks heavy emitters");
  });

  it("clears the banner when a later tick reports overload=false", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    expect(screen.getByTestId("preview-overload-banner")).toBeInTheDocument();
    emit("stats/tick", tick(false));
    // presence shim: the banner stays mounted in
    // data-state="closed" while banner-out plays, then unmounts. jsdom
    // runs no CSS animations, so the unmount path here is usePresence's
    // timeout fallback (EXIT_MS 150 + 50ms slack).
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner).toHaveAttribute("data-state", "closed");
    await act(async () => {
      await new Promise((r) => setTimeout(r, 250));
    });
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  });

  it("wears the soft-shadow motion class instead of shadow-xl", () => {
    // .banner-animate carries box-shadow: var(--shadow-soft)
    // (components.css) and the entrance/exit keyframes; the old
    // shadow-xl ring-1 ring-black/15 Tailwind stack is retired.
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner.className).toContain("banner-animate");
    expect(banner.className).not.toContain("shadow-xl");
    expect(banner.className).not.toContain("ring-1");
  });

  it("unsubscribes from stats/tick on unmount", () => {
    const { bridge, handlers } = makeBridge();
    const { unmount } = render(<OverloadBanner bridge={bridge} />);
    expect(handlers.has("stats/tick")).toBe(true);
    unmount();
    expect(handlers.has("stats/tick")).toBe(false);
  });

  // ── Refusal banner ──────────────────────────────────────────────

  it("mounts the banner with refusal copy when engine/overload/refused fires", () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("engine/overload/refused", { estimated: 24000, cap: 10000, attemptedCount: 3 });
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner).toBeInTheDocument();
    expect(banner.textContent).toContain("Spawn blocked");
    expect(banner.textContent).toContain("24,000");
    expect(banner.textContent).toContain("10,000");
  });

  it("auto-dismisses the refusal banner after ~5s", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("engine/overload/refused", { estimated: 24000, cap: 10000, attemptedCount: 3 });
    expect(screen.getByTestId("preview-overload-banner")).toBeInTheDocument();
    // First act: wait past REFUSAL_MS (5000ms) so setRefusal(null) fires and
    // the banner transitions to data-state="closed".
    await act(async () => {
      await new Promise((r) => setTimeout(r, 5050));
    });
    // Banner is now in data-state="closed" (usePresence exit window started).
    // Second act: wait for the usePresence timeout fallback (EXIT_MS+50 = 200ms).
    await act(async () => {
      await new Promise((r) => setTimeout(r, 250));
    });
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  }, 10_000);

  it("latch banner unchanged: stats/tick overload=true shows latch copy, overload=false hides it", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner.textContent).toContain("Preview spawning limited");
    expect(banner.textContent).toContain("lower spawn rates");
    // Clear latch
    emit("stats/tick", tick(false));
    expect(screen.getByTestId("preview-overload-banner")).toHaveAttribute("data-state", "closed");
    await act(async () => {
      await new Promise((r) => setTimeout(r, 250));
    });
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  });

  it("refusal takes precedence over latch copy while the refusal window is active", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    // Activate latch
    emit("stats/tick", tick(true));
    expect(screen.getByTestId("preview-overload-banner").textContent).toContain("Preview spawning limited");
    // Fire refusal — should show refusal copy
    emit("engine/overload/refused", { estimated: 24000, cap: 10000, attemptedCount: 1 });
    expect(screen.getByTestId("preview-overload-banner").textContent).toContain("Spawn blocked");
    expect(screen.getByTestId("preview-overload-banner").textContent).toContain("24,000");
    // The engine is GENUINELY still latched (estimate undercount case):
    // the 4 Hz stream keeps reporting overload=true after the refusal.
    // (The refusal handler force-clears the web latch — this re-assert is
    // how a real latch survives it.)
    emit("stats/tick", tick(true));
    // After 5s window with latch still active → banner returns to latch copy.
    // Wait past REFUSAL_MS so setRefusal(null) fires.
    await act(async () => {
      await new Promise((r) => setTimeout(r, 5050));
    });
    // Latch is still active (overload=true was never cleared) → latch copy visible.
    // visible = false || true = true, so usePresence stays mounted (no exit).
    const bannerAfter = screen.queryByTestId("preview-overload-banner");
    expect(bannerAfter).toBeInTheDocument();
    expect(bannerAfter!.textContent).toContain("Preview spawning limited");
  }, 10_000);

  it("keeps the refusal copy through the exit fade — no stale latch flash", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("engine/overload/refused", { estimated: 2000, cap: 1000, attemptedCount: 1 });
    await act(async () => {
      await new Promise((r) => setTimeout(r, 5050));
    });
    const banner = screen.getByTestId("preview-overload-banner");
    expect(banner).toHaveAttribute("data-state", "closed");
    expect(banner.textContent).toContain("Spawn blocked");
    expect(banner.textContent).not.toContain("Preview spawning limited");
  }, 10_000);

  it("a refusal clears a stale web-side latch (engine cleared the preview)", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    emit("engine/overload/refused", { estimated: 2000, cap: 1000, attemptedCount: 1 });
    await act(async () => {
      await new Promise((r) => setTimeout(r, 5050));
    });
    await act(async () => {
      await new Promise((r) => setTimeout(r, 250));
    });
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  }, 10_000);

  it("re-firing a refusal restarts the 5s dismiss window", async () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("engine/overload/refused", { estimated: 24000, cap: 10000, attemptedCount: 1 });
    // After ~3s, fire a second refusal (restarts the window)
    await act(async () => {
      await new Promise((r) => setTimeout(r, 3000));
    });
    emit("engine/overload/refused", { estimated: 24000, cap: 10000, attemptedCount: 1 });
    // At ~4s from first refusal (1s after second), still visible
    await act(async () => {
      await new Promise((r) => setTimeout(r, 1000));
    });
    expect(screen.queryByTestId("preview-overload-banner")).toBeInTheDocument();
  }, 15_000);
});
