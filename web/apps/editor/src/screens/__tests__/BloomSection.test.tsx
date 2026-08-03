// Vitest unit tests for BloomSection (the bloom controls folded into the
// Lighting pane, session 11; formerly BloomPanel).
// Verifies: Enable checkbox + 3 Spinners (Strength / Cutoff / Size) render;
// changing Strength dispatches engine/set/bloom-strength with the new value.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { BloomSection } from "../BloomSection";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeStubBridge(): Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> } {
  const snapshot = {
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
