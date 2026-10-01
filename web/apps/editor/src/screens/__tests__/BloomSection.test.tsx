// Vitest unit tests for BloomSection (the bloom controls folded into the
// Lighting pane, session 11; formerly BloomPanel).
// Verifies: Enable checkbox + 3 Spinners (Strength / Cutoff / Size) render;
// changing Strength dispatches engine/set/bloom-strength with the new value.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, act } from "@testing-library/react";
import { BloomSection } from "../BloomSection";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeSnapshot() {
  return {
    ground: false,
    groundZ: 0,
    groundTexture: 0,
    groundSolidColor: 0,
    groundSlotCustomPaths: [],
    skydomeSlot: 0,
    skydomeCustomPaths: ["", "", ""],
    background: 0,
    lights: {
      sun: { diffuse: [1, 1, 1, 1], specular: [1, 1, 1, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
      fill1: { diffuse: [0, 0, 0, 1], specular: [0, 0, 0, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
      fill2: { diffuse: [0, 0, 0, 1], specular: [0, 0, 0, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
    },
    ambient: [0, 0, 0, 1],
    shadow: [0, 0, 0, 1],
    bloom: true,
    bloomAvailable: true,
    bloomStrength: 1.5,
    bloomCutoff: 0.5,
    bloomSize: 8,
    heatDebug: false,
    paused: false,
    camera: { position: [0, 0, 0], target: [0, 0, 0], up: [0, 0, 1] },
    wind: [0, 0, 0],
    gravity: [0, 0, 0],
  };
}

function makeStubBridge(): Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> } {
  const snapshot = makeSnapshot();
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      if (req.kind === "engine/query/bloom-available") return Promise.resolve(true);
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

describe("BloomSection", () => {
  it("renders the Enable checkbox + 3 Spinners (Strength / Cutoff / Size)", () => {
    const bridge = makeStubBridge();
    render(<BloomSection bridge={bridge} defaultOpen />);
    expect(screen.getByLabelText("Enable bloom")).toBeInTheDocument();
    expect(screen.getByLabelText("Bloom strength")).toBeInTheDocument();
    expect(screen.getByLabelText("Bloom cutoff")).toBeInTheDocument();
    expect(screen.getByLabelText("Bloom size")).toBeInTheDocument();
  });

  it("a late engine/state/snapshot does not overwrite a newer engine/state/changed", async () => {
    const bridge = makeStubBridge();
    const base = makeSnapshot();
    let resolveSnapshot: (s: unknown) => void = () => {};
    const captured: { onChanged?: (e: { payload: unknown }) => void } = {};
    bridge.request.mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") {
        return new Promise((resolve) => { resolveSnapshot = resolve; });
      }
      if (req.kind === "engine/query/bloom-available") return Promise.resolve(true);
      return Promise.resolve({});
    });
    bridge.on.mockImplementation((kind: string, h: (e: { payload: unknown }) => void) => {
      if (kind === "engine/state/changed") captured.onChanged = h;
      return () => {};
    });
    render(<BloomSection bridge={bridge} defaultOpen />);

    // The newer broadcast (strength 3) lands first, then the stale mount
    // snapshot (strength 1.5) resolves — the panel must keep the broadcast.
    act(() => captured.onChanged?.({ payload: { ...base, bloomStrength: 3 } }));
    await act(async () => {
      resolveSnapshot({ ...base, bloomStrength: 1.5 });
      await Promise.resolve();
    });
    expect(Number((screen.getByLabelText("Bloom strength") as HTMLInputElement).value)).toBe(3);
  });

  it("changing Strength dispatches engine/set/bloom-strength", () => {
    const bridge = makeStubBridge();
    render(<BloomSection bridge={bridge} defaultOpen />);
    const strength = screen.getByLabelText("Bloom strength") as HTMLInputElement;
    fireEvent.change(strength, { target: { value: "2.5" } });
    fireEvent.blur(strength);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setStrength = calls.find((c) => c.kind === "engine/set/bloom-strength");
    expect(setStrength).toBeDefined();
    expect(setStrength.params.v).toBe(2.5);
  });
});
