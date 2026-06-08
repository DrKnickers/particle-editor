// Vitest: StatusBar parity elements (/7/8).
//   — always-on "⇧ Shift: spawn instance" hint (legacy main.cpp:2036).
//   — "PAUSED" indicator shown ONLY while the preview is paused
//           (driven by engine/state/changed, same signal the Toolbar uses).
//   — cursor readout is 2 decimal places (legacy was 2dp).

import { describe, it, expect, vi } from "vitest";
import { render, screen, act } from "@testing-library/react";
import { StatusBar } from "../StatusBar";
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
  it("always shows the shift-to-spawn hint ()", () => {
    const { bridge } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    expect(screen.getByText("⇧ Shift: spawn instance")).toBeInTheDocument();
  });

  it("shows PAUSED only while paused ()", async () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    // Not paused initially.
    expect(screen.queryByText("PAUSED")).not.toBeInTheDocument();
    emit("engine/state/changed", { paused: true });
    expect(screen.getByText("PAUSED")).toBeInTheDocument();
    emit("engine/state/changed", { paused: false });
    expect(screen.queryByText("PAUSED")).not.toBeInTheDocument();
  });

  it("renders the cursor readout with 2 decimals ()", () => {
    const { bridge, emit } = makeBridge();
    render(<StatusBar bridge={bridge} />);
    emit("cursor/position-3d", { x: 1, y: -2.5, z: 3.456 });
    expect(screen.getByText("1.00, -2.50, 3.46")).toBeInTheDocument();
  });
});
