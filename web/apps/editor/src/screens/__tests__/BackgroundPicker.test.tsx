// Vitest unit tests for BackgroundPicker — specifically the
// custom-slot chain (file/open → set-skydome-custom-path →
// set-skydome-slot). The bundled-slot path is covered indirectly by
// the Playwright suite (background-picker.spec.ts). Here we focus on:
//
//   1. The file/open call carries `filter: "skydome"` so the native
//      host pops the DDS/TGA picker, not the .alo one.
//   2. On a resolved path, the two follow-up dispatches fire in order.
//   3. On a cancelled / failed pick, the chain aborts cleanly with no
//      follow-ups (the slot stays empty).

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { BackgroundPicker } from "../BackgroundPicker";
import type { Bridge } from "@particle-editor/bridge-schema";

type RequestFn = (req: { kind: string; params?: Record<string, unknown> }) => Promise<unknown>;

function makeStubBridge(opts: {
  fileOpen: { ok: true; path: string } | { ok: false; error: string };
}): Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> } {
  const snapshot = {
    ground: false,
    groundZ: 0,
    groundTexture: 0,
    groundSolidColor: 0x00888888,
    groundSlotCustomPaths: ["", "", "", "", "", "", "", ""],
    skydomeSlot: 0,
    // Slot 9 is empty so clicking it routes through the picker chain.
    skydomeCustomPaths: ["", "", ""],
    background: 0,
    lights: {
      sun:   { diffuse: [1, 1, 1, 1], specular: [1, 1, 1, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
      fill1: { diffuse: [0, 0, 0, 1], specular: [0, 0, 0, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
      fill2: { diffuse: [0, 0, 0, 1], specular: [0, 0, 0, 1], position: [0, 0, 1, 0], direction: [0, 0, 0, 0] },
    },
    ambient: [0, 0, 0, 1],
    shadow:  [0, 0, 0, 1],
    bloom: false,
    bloomAvailable: true,
    bloomStrength: 1,
    bloomCutoff: 0.5,
    bloomSize: 8,
    heatDebug: false,
    paused: false,
    camera: { position: [0, 0, 0], target: [0, 0, 0], up: [0, 0, 1] },
    wind: [0, 0, 0],
    gravity: [0, 0, 0],
  };
  const request: RequestFn = vi.fn().mockImplementation((req) => {
    if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
    if (req.kind === "file/open") return Promise.resolve(opts.fileOpen);
    return Promise.resolve({});
  });
  return {
    request,
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

describe("BackgroundPicker — custom-slot file picker chain", () => {
  it("clicking an empty Custom slot dispatches file/open with filter:\"skydome\"", async () => {
    const bridge = makeStubBridge({ fileOpen: { ok: false, error: "browser-mode" } });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);
    // Wait for the snapshot to land so the Custom slot tiles render
    // with their empty-state Browse... label.
    await waitFor(() => {
      expect(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ }));
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
      const open = calls.find((c) => c.kind === "file/open");
      expect(open).toBeDefined();
      expect(open.params).toEqual({ filter: "skydome" });
    });
  });

  it("on a resolved path the chain dispatches set-skydome-custom-path then set-skydome-slot", async () => {
    const bridge = makeStubBridge({ fileOpen: { ok: true, path: "C:/textures/sky.dds" } });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ }));
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0].kind);
      // Snapshot lands first, then file/open, then the two engine setters.
      expect(calls).toContain("engine/set/skydome-custom-path");
      expect(calls).toContain("engine/set/skydome-slot");
      const customPathIdx = calls.indexOf("engine/set/skydome-custom-path");
      const slotIdx = calls.indexOf("engine/set/skydome-slot");
      expect(customPathIdx).toBeLessThan(slotIdx);
    });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setPath = calls.find((c) => c.kind === "engine/set/skydome-custom-path");
    expect(setPath.params).toEqual({ slot: 9, path: "C:/textures/sky.dds" });
    const setSlot = calls.find((c) => c.kind === "engine/set/skydome-slot");
    expect(setSlot.params).toEqual({ slot: 9 });
  });

  it("on a cancelled pick the follow-up chain does not fire", async () => {
    const bridge = makeStubBridge({ fileOpen: { ok: false, error: "user-cancelled" } });
    render(<BackgroundPicker bridge={bridge} onClose={() => {}} />);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ }));
    // Give the async chain a tick to settle before asserting absence.
    await new Promise((r) => setTimeout(r, 20));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0].kind);
    expect(calls).toContain("file/open");
    expect(calls).not.toContain("engine/set/skydome-custom-path");
    expect(calls).not.toContain("engine/set/skydome-slot");
  });
});
