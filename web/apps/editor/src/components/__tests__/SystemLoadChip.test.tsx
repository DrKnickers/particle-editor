// Vitest: SystemLoadChip (overload-indicator-consistency spec, Part 2).
// Predictive system-total warning: visible exactly when the NEXT spawn
// attempt would be refused by the #138 gate —
// (instances + 1) × systemLoad > cap, guard enabled.
import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, act } from "@testing-library/react";
import { SystemLoadChip } from "../SystemLoadChip";
import { writeOverloadGuard } from "@/lib/overload-guard";
import type { Bridge } from "@particle-editor/bridge-schema";

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
  return { bridge: { request, on } as unknown as Bridge, emit };
}

const tick = (instances: number) => ({
  fps: 30, emitters: 1, particles: 0, instances, overload: false,
});

describe("SystemLoadChip", () => {
  beforeEach(() => {
    localStorage.clear();
  });

  it("hidden while the next placement fits the cap", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={400} />);
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });

  it("hidden for a zero-load effect", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={0} />);
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });

  it("warns with the effect-too-big copy at zero instances", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={2_000} />);
    const chip = screen.getByTestId("system-load-chip");
    expect(chip).toHaveAttribute("role", "status");
    expect(chip.textContent).toContain("This effect ≈ 2,000 particles");
    expect(chip.textContent).toContain("1,000 preview limit");
  });

  it("switches to the prospective copy once an instance is placed", () => {
    const { bridge, emit } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={600} />);
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
    emit("stats/tick", tick(1));
    const chip = screen.getByTestId("system-load-chip");
    expect(chip.textContent).toContain("Another instance would exceed");
    expect(chip.textContent).toContain("1,200");
    expect(chip.textContent).toContain("1,000");
  });

  it("hidden when the guard is disabled, regardless of load", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: false, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={1_000_000_000} />);
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });

  it("reacts live to a cap change", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={2_000} />);
    expect(screen.getByTestId("system-load-chip")).toBeInTheDocument();
    act(() => {
      writeOverloadGuard({ enabled: true, maxParticles: 10_000 });
    });
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });

  it("respects the strictly-greater boundary (systemLoad === cap is hidden, +1 shows)", () => {
    const { bridge } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    const { rerender } = render(<SystemLoadChip bridge={bridge} systemLoad={1_000} />);
    // (0+1) × 1000 = 1000, NOT > 1000 → hidden (gate refuses only on strictly-greater).
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
    rerender(<SystemLoadChip bridge={bridge} systemLoad={1_001} />);
    expect(screen.getByTestId("system-load-chip")).toBeInTheDocument();
  });

  it("clears back below the cap when instances drop (preview cleared)", () => {
    const { bridge, emit } = makeBridge();
    writeOverloadGuard({ enabled: true, maxParticles: 1_000 });
    render(<SystemLoadChip bridge={bridge} systemLoad={600} />);
    emit("stats/tick", tick(1));
    expect(screen.getByTestId("system-load-chip")).toBeInTheDocument();
    emit("stats/tick", tick(0));
    expect(screen.queryByTestId("system-load-chip")).not.toBeInTheDocument();
  });
});
