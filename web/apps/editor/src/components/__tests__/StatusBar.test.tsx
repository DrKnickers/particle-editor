// Vitest: StatusBar parity elements.
//   - always-on "⇧ Shift: spawn instance" hint (legacy main.cpp).
//   - "PAUSED" indicator shown ONLY while the preview is paused
//     (driven by engine/state/changed, same signal the Toolbar uses).
//   - cursor readout is 2 decimal places (legacy was 2dp).

import { describe, it, expect, vi } from "vitest";
import { render, screen, act } from "@testing-library/react";
import { StatusBar, __statsCellsRenderCount } from "../StatusBar";
import type { Bridge } from "@particle-editor/bridge-schema";

// A bridge mock that records `on` handlers by event name so the test can
// drive them, and resolves the engine-state snapshot request.
function makeBridge(snapshot: { paused: boolean } = { paused: false }) {
  const handlers = new Map<string, (e: { payload: unknown }) => void>();
  const request = vi.fn().mockImplementation((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
    return Promise.resolve({ ok: true });
  });
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

describe("StatusBar", () => {
  it("always shows the shift-to-spawn hint", () => {
    const { bridge } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    expect(screen.getByText("⇧ Shift: spawn instance")).toBeInTheDocument();
  });

  it("shows PAUSED only while paused", async () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    // Not paused initially.
    expect(screen.queryByText("PAUSED")).not.toBeInTheDocument();
    emit("engine/state/changed", { paused: true });
    expect(screen.getByText("PAUSED")).toBeInTheDocument();
    emit("engine/state/changed", { paused: false });
    expect(screen.queryByText("PAUSED")).not.toBeInTheDocument();
  });

  it("renders the cursor readout with 2 decimals", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    emit("cursor/position-3d", { x: 1, y: -2.5, z: 3.456 });
    expect(screen.getByText("1.00, -2.50, 3.46")).toBeInTheDocument();
  });

  it("rounds the FPS readout from stats ticks", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    emit("stats/tick", {
      fps: 59.6, emitters: 2, particles: 16384, instances: 3, overload: false,
    });

    const fpsLabel = screen.getByText("FPS");
    expect(fpsLabel.nextElementSibling).toHaveTextContent(/^60$/);
  });

  // Preview spawn-overload guard: while stats/tick
  // reports overload=true, the Particles readout tints amber; it reverts
  // when the overload clears. Feel test: the readout is a passive
  // non-button, so it carries NO tooltip — the OverloadBanner over the
  // viewport states the cause.
  it("tints the particle count amber while overloaded", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    const tick = (overload: boolean) => ({
      fps: 30, emitters: 2, particles: 16384, instances: 3, overload,
    });

    emit("stats/tick", tick(true));
    const value = screen.getByText("16384");
    expect(value.className).toContain("text-warning-fg");

    emit("stats/tick", tick(false));
    const cleared = screen.getByText("16384");
    expect(cleared.className).not.toContain("text-warning-fg");
  });

  // The #549 Profiler audit found StatusBar re-rendered all five cells on every
  // ~30 Hz cursor/position-3d event. The stats cells are now memoized: a cursor
  // move re-renders the parent (cursor cell only), NOT the stats cells.
  it("does not re-render the stats cells on a cursor move (memo)", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    emit("stats/tick", { fps: 60, emitters: 1, particles: 10, instances: 1, overload: false });
    const baseline = __statsCellsRenderCount();

    // A burst of cursor moves must not touch StatsCells' render count…
    emit("cursor/position-3d", { x: 1, y: 1, z: 1 });
    emit("cursor/position-3d", { x: 2, y: 2, z: 2 });
    emit("cursor/position-3d", { x: 3, y: 3, z: 3 });
    expect(__statsCellsRenderCount()).toBe(baseline);
    // …though the cursor readout itself did update (parent re-rendered).
    expect(screen.getByText("3.00, 3.00, 3.00")).toBeInTheDocument();

    // A real stats change DOES re-render the stats cells.
    emit("stats/tick", { fps: 30, emitters: 1, particles: 10, instances: 1, overload: false });
    expect(__statsCellsRenderCount()).toBeGreaterThan(baseline);
  });

  // Freeze clears BOTH stats and cursor to em-dash placeholders (deterministic
  // goldens). The stats clear flows through StatsCells via its `stats` prop.
  it("clears both stats and cursor to placeholders on freeze", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    emit("stats/tick", { fps: 60, emitters: 2, particles: 99, instances: 1, overload: false });
    emit("cursor/position-3d", { x: 5, y: 6, z: 7 });
    expect(screen.getByText("5.00, 6.00, 7.00")).toBeInTheDocument();
    expect(screen.getByText("99")).toBeInTheDocument();

    emit("stats/frozen-changed", { frozen: true });
    expect(screen.queryByText("5.00, 6.00, 7.00")).not.toBeInTheDocument();
    expect(screen.queryByText("99")).not.toBeInTheDocument();
    // The cursor cell specifically shows the em-dash placeholder.
    expect(screen.getByText("Cursor").nextElementSibling).toHaveTextContent("—");
  });
});
