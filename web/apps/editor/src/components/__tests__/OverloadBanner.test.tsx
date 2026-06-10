// Vitest: OverloadBanner (preview spawn-overload guard, plan part 2 §3).
// The banner subscribes to the 4 Hz stats/tick bridge event and shows a
// fixed warning over the viewport while `overload` is latched. It must
// also register a viewport occlusion (plan risk 4) — without it the
// D3D-composited viewport popup overpaints the DOM banner.

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

  it("clears the banner when a later tick reports overload=false", () => {
    const { bridge, emit } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    emit("stats/tick", tick(true));
    expect(screen.getByTestId("preview-overload-banner")).toBeInTheDocument();
    emit("stats/tick", tick(false));
    expect(screen.queryByTestId("preview-overload-banner")).not.toBeInTheDocument();
  });

  it("registers a viewport occlusion while visible and releases it on clear (plan risk 4)", () => {
    const { bridge, emit, request } = makeBridge();
    render(<OverloadBanner bridge={bridge} />);
    expect(request).not.toHaveBeenCalled();

    emit("stats/tick", tick(true));
    const occlude = request.mock.calls
      .map((c) => c[0] as { kind: string; params: { id: string; rect: unknown } })
      .filter((r) => r.kind === "viewport/occlude");
    expect(occlude.length).toBeGreaterThan(0);
    expect(occlude[0]!.params.id).toBe("banner:preview-overload");
    expect(occlude[0]!.params.rect).not.toBeNull();

    request.mockClear();
    emit("stats/tick", tick(false));
    const release = request.mock.calls
      .map((c) => c[0] as { kind: string; params: { id: string; rect: unknown } })
      .filter((r) => r.kind === "viewport/occlude" && r.params.rect === null);
    expect(release.length).toBe(1);
    expect(release[0]!.params.id).toBe("banner:preview-overload");
  });

  it("unsubscribes from stats/tick on unmount", () => {
    const { bridge, handlers } = makeBridge();
    const { unmount } = render(<OverloadBanner bridge={bridge} />);
    expect(handlers.has("stats/tick")).toBe(true);
    unmount();
    expect(handlers.has("stats/tick")).toBe(false);
  });
});
