// Vitest tests for SpawnerPane.
//
// Coverage:
//   1. Renders the burst-size + spacing + position×3 + lifetime Spinners
//      (verify at least 5 by-label spinners exist).
//   2. Switching Mode from Manual → Auto reveals the Interval Spinner +
//      Enabled checkbox. Initial mount uses Manual so Interval is hidden.
//   3. Changing burstSize via the Spinner fires `spawner/start` with the
//      new value embedded in the params.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor, act } from "@testing-library/react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { SpawnerPane } from "../SpawnerPane";
import { makeDefaultEngineState } from "@/bridge/mock-state";
import { makeDefaultSpawnerParams } from "@/lib/spawner-defaults";
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

describe("SpawnerPane", () => {
  it("a late engine/state/snapshot does not overwrite a newer engine/state/changed", async () => {
    const bridge = makeStubBridge("manual");
    const base: EngineStateDto = {
      ...makeDefaultEngineState(),
      spawner: { ...makeDefaultSpawnerParams(), mode: "manual", burstSize: 2 },
    };
    // Every snapshot request and state/changed listener (the pane's own, plus
    // the shared engine-field store's) is held so the test can drive them all.
    const snapshotResolvers: Array<(s: unknown) => void> = [];
    const changedHandlers: Array<(e: { payload: unknown }) => void> = [];
    bridge.request.mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return new Promise((resolve) => { snapshotResolvers.push(resolve); });
      }
      return Promise.resolve({});
    });
    bridge.on.mockImplementation((kind: string, h: (e: { payload: unknown }) => void) => {
      if (kind === "engine/state/changed") changedHandlers.push(h);
      return () => {};
    });
    render(<SpawnerPane bridge={bridge} />);

    // The newer broadcast (burst 7) lands first, then the stale mount
    // snapshot (burst 2) resolves — the panel must keep the broadcast.
    const newer = { payload: { ...base, spawner: { ...base.spawner, burstSize: 7 } } };
    act(() => changedHandlers.forEach((h) => h(newer)));
    await act(async () => {
      snapshotResolvers.forEach((r) => r(base));
      await Promise.resolve();
    });
    expect(Number((screen.getByLabelText("Burst size") as HTMLInputElement).value)).toBe(7);
  });

  it("a late engine/state/snapshot does not overwrite a local commit made before it resolved", async () => {
    const bridge = makeStubBridge("manual");
    const base: EngineStateDto = {
      ...makeDefaultEngineState(),
      spawner: { ...makeDefaultSpawnerParams(), mode: "manual", burstSize: 2 },
    };
    let resolveSnapshot: (s: unknown) => void = () => {};
    bridge.request.mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return new Promise((resolve) => { resolveSnapshot = resolve; });
      }
      return Promise.resolve({});
    });
    render(<SpawnerPane bridge={bridge} />);

    // The user commits burst 5 before the mount snapshot (taken before the
    // commit) resolves; its echo is skipped by lastCommitted, so a stale
    // snapshot applied now would stick.
    const burst = screen.getByLabelText("Burst size") as HTMLInputElement;
    fireEvent.change(burst, { target: { value: "5" } });
    fireEvent.blur(burst);
    await act(async () => {
      resolveSnapshot(base);
      await Promise.resolve();
    });
    expect(Number((screen.getByLabelText("Burst size") as HTMLInputElement).value)).toBe(5);
  });

  it("renders at least 5 Spinners (burst size, spacing, position xyz, lifetime)", async () => {
    const bridge = makeStubBridge("manual");
    render(<SpawnerPane bridge={bridge} />);

    expect(screen.getByLabelText("Burst size")).toBeInTheDocument();
    expect(screen.getByLabelText("Burst spacing")).toBeInTheDocument();
    expect(screen.getByLabelText("Position X")).toBeInTheDocument();
    expect(screen.getByLabelText("Position Y")).toBeInTheDocument();
    expect(screen.getByLabelText("Position Z")).toBeInTheDocument();
    expect(screen.getByLabelText("Max lifetime")).toBeInTheDocument();
  });

  it("labels the distance/velocity/accel section headers with units", () => {
    const bridge = makeStubBridge("manual");
    render(<SpawnerPane bridge={bridge} />);
    // Velocity + acceleration carry the compound units (each unique);
    // the distance sections (Position / Jitter position / Squiggle amplitude)
    // each show the bare "units" word.
    expect(screen.getByText("units/s")).toBeInTheDocument();   // Velocity
    expect(screen.getByText("units/s²")).toBeInTheDocument();  // Acceleration (arc)
    expect(screen.getAllByText("units").length).toBeGreaterThanOrEqual(3);
  });

  it("switching Mode from Manual to Auto reveals the Interval Spinner + Enabled checkbox", async () => {
    // Start in manual; snapshot is async so wait for the panel to
    // settle into the snapshot's mode before asserting the initial
    // "no interval" state.
    const bridge = makeStubBridge("manual");
    render(<SpawnerPane bridge={bridge} />);

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
    render(<SpawnerPane bridge={bridge} />);

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
    render(<SpawnerPane bridge={bridge} />);
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

describe("SpawnerPane — fly like the projectile", () => {
  function bridgeWith(
    referenceProjectile: EngineStateDto["referenceProjectile"],
    matchProjectile = false,
    projectileOutOfReach = false,
  ) {
    const snapshot: EngineStateDto = {
      ...makeDefaultEngineState(),
      referenceObjectName: referenceProjectile ? "Proj_Laser" : "",
      referenceProjectile,
      spawner: { ...makeDefaultSpawnerParams(), mode: "manual", matchProjectile, projectileOutOfReach },
    };
    return {
      request: vi.fn().mockImplementation((req: { kind: string }) =>
        Promise.resolve(req.kind === "engine/state/snapshot" ? snapshot : {})),
      on: vi.fn().mockReturnValue(() => {}),
    } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
  }
  const laser = {
    flyable: true, speed: 330, range: 500, flightSec: 46 / 30, category: "Laser", pathEmulated: true, usesTarget: false,
  };
  const missile = { ...laser, category: "MISSILE", usesTarget: true };

  it("hides the section when the reference object is not a projectile", async () => {
    render(<SpawnerPane bridge={bridgeWith(null)} />);
    await waitFor(() => expect(screen.getByLabelText("Burst size")).toBeTruthy());
    expect(screen.queryByLabelText("Fly like the projectile")).toBeNull();
  });

  it("shows the game flight and commits matchProjectile", async () => {
    const bridge = bridgeWith(laser);
    render(<SpawnerPane bridge={bridge} />);
    const box = await screen.findByLabelText("Fly like the projectile");
    expect(screen.getByTestId("spawner-projectile-flight").textContent).toContain("330 units/s for 500 units (1.53 s)");
    expect(screen.queryByTestId("spawner-projectile-curved")).toBeNull();
    fireEvent.click(box);
    const start = bridge.request.mock.calls.map((c) => c[0]).find((r) => r.kind === "spawner/start");
    expect(start?.params.matchProjectile).toBe(true);
  });

  it("stands down the lifetime and path controls while matching, but not jitter", async () => {
    render(<SpawnerPane bridge={bridgeWith(laser, true)} />);
    await waitFor(() => expect(screen.getByLabelText("Max lifetime").matches(":disabled")).toBe(true));
    expect(screen.getByLabelText("Acceleration X").matches(":disabled")).toBe(true);
    expect(screen.getByLabelText("Squiggle frequency").matches(":disabled")).toBe(true);
    expect(screen.getByLabelText("Jitter position X").matches(":disabled")).toBe(false);
    expect(screen.getByLabelText("Velocity X").matches(":disabled")).toBe(false);
  });

  it("shows and commits the target only for shots that aim at it, while matching", async () => {
    const { unmount } = render(<SpawnerPane bridge={bridgeWith(laser, true)} />);
    await screen.findByLabelText("Fly like the projectile");
    expect(screen.queryByLabelText("Target X")).toBeNull();
    unmount();

    const bridge = bridgeWith(missile, true);
    render(<SpawnerPane bridge={bridge} />);
    const targetX = await screen.findByLabelText("Target X");
    expect(screen.getByTestId("spawner-projectile-target").textContent).toContain("Missiles home on the target");
    expect(screen.queryByTestId("spawner-projectile-curved")).toBeNull();
    expect(screen.queryByTestId("spawner-projectile-out-of-reach")).toBeNull();
    fireEvent.change(targetX, { target: { value: "300" } });
    fireEvent.blur(targetX);
    await waitFor(() => {
      const starts = bridge.request.mock.calls.map((c) => c[0]).filter((r) => r.kind === "spawner/start");
      expect(starts.at(-1)?.params.targetPosition).toEqual([300, 0, 0]);
    });
  });

  it("warns when the game wouldn't fire at the target", async () => {
    render(<SpawnerPane bridge={bridgeWith({ ...missile, category: "GRENADE" }, true, true)} />);
    expect((await screen.findByTestId("spawner-projectile-out-of-reach")).textContent).toContain("out of lob range");
  });

  it("flags categories the preview still flies straight and refuses a projectile with no speed", async () => {
    const { unmount } = render(
      <SpawnerPane bridge={bridgeWith({ ...laser, category: "ROCKET", pathEmulated: false })} />);
    expect((await screen.findByTestId("spawner-projectile-curved")).textContent).toContain("ROCKET");
    unmount();
    render(<SpawnerPane bridge={bridgeWith({ ...laser, flyable: false, speed: 0 }, true)} />);
    const box = (await screen.findByLabelText("Fly like the projectile")) as HTMLInputElement;
    expect(box.disabled).toBe(true);
    expect(screen.getByLabelText("Max lifetime").matches(":disabled")).toBe(false);
  });
});
