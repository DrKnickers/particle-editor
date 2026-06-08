// Vitest tests for SpawnerPanel (Phase 3 Screen 8 Batch 4).
//
// Coverage:
//   1. Renders the burst-size + spacing + position×3 + lifetime Spinners
//      (verify at least 5 by-label spinners exist).
//   2. Switching Mode from Manual → Auto reveals the Interval Spinner +
//      Enabled checkbox. Initial mount uses Manual so Interval is hidden.
//   3. Changing burstSize via the Spinner fires `spawner/start` with the
//      new value embedded in the params.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { SpawnerPanel } from "../SpawnerPanel";
import { makeDefaultEngineState, makeDefaultSpawnerParams } from "@/bridge/mock-state";
import {
  useRightDock,
  __resetRightDockForTests,
} from "@/lib/right-dock";
import { renderHook } from "@testing-library/react";

beforeEach(() => {
  localStorage.removeItem("alo:right-dock");
  localStorage.removeItem("alo:spawner-visible");
  __resetRightDockForTests();
});

function makeStubBridge(modeOverride: "manual" | "auto" = "manual"): Bridge & {
  request: ReturnType<typeof vi.fn>;
  on: ReturnType<typeof vi.fn>;
} {
  const snapshot: EngineStateDto = {
    ...makeDefaultEngineState(),
    spawner: { ...makeDefaultSpawnerParams(), mode: modeOverride },
  };
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & {
    request: ReturnType<typeof vi.fn>;
    on: ReturnType<typeof vi.fn>;
  };
}

describe("SpawnerPanel", () => {
  it("renders at least 5 Spinners (burst size, spacing, position xyz, lifetime)", async () => {
    const bridge = makeStubBridge("manual");
    render(<SpawnerPanel bridge={bridge} />);

    expect(screen.getByLabelText("Burst size")).toBeInTheDocument();
    expect(screen.getByLabelText("Burst spacing")).toBeInTheDocument();
    expect(screen.getByLabelText("Position X")).toBeInTheDocument();
    expect(screen.getByLabelText("Position Y")).toBeInTheDocument();
    expect(screen.getByLabelText("Position Z")).toBeInTheDocument();
    expect(screen.getByLabelText("Max lifetime")).toBeInTheDocument();
  });

  it("switching Mode from Manual to Auto reveals the Interval Spinner + Enabled checkbox", async () => {
    // Start in manual; snapshot is async so wait for the panel to
    // settle into the snapshot's mode before asserting the initial
    // "no interval" state.
    const bridge = makeStubBridge("manual");
    render(<SpawnerPanel bridge={bridge} />);

    await waitFor(() => {
      // Manual radio is checked once the snapshot lands.
      expect(
        (screen.getByLabelText("Manual mode") as HTMLInputElement).checked,
      ).toBe(true);
    });

    expect(screen.queryByLabelText("Burst interval")).toBeNull();
    expect(screen.queryByLabelText("Enable spawner")).toBeNull();

    // Flip to Auto via the radio. The mode setter commits the new
    // config synchronously through setConfig + the bridge stub.
    fireEvent.click(screen.getByLabelText("Auto mode"));

    await waitFor(() => {
      expect(screen.getByLabelText("Burst interval")).toBeInTheDocument();
    });
    expect(screen.getByLabelText("Enable spawner")).toBeInTheDocument();
  });

  it("changing burstSize fires spawner/start with the new value embedded", async () => {
    const bridge = makeStubBridge("manual");
    render(<SpawnerPanel bridge={bridge} />);

    const input = screen.getByLabelText("Burst size") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "7" } });
    fireEvent.blur(input);

    await waitFor(() => {
      const calls = bridge.request.mock.calls
        .map((c) => c[0] as { kind: string; params: { burstSize?: number } })
        .filter((c) => c.kind === "spawner/start");
      expect(calls.length).toBeGreaterThanOrEqual(1);
      const last = calls[calls.length - 1];
      expect(last.params.burstSize).toBe(7);
    });
  });

  it("Close Spawner button closes the right-dock", async () => {
    const bridge = makeStubBridge("manual");
    render(<SpawnerPanel bridge={bridge} />);
    const { result } = renderHook(() => useRightDock());
    expect(result.current).toBe("spawner"); // default dock = spawner

    fireEvent.click(screen.getByRole("button", { name: "Close Spawner" }));

    // After the click, the dock closes (setDock(null)).
    await waitFor(() => {
      expect(result.current).toBeNull();
    });
    expect(localStorage.getItem("alo:right-dock")).toBe("none");
  });
});
