// Vitest unit tests for the GroundTexturePanel:
//   1. Bundled slot click → engine/set/ground-texture.
//   2. Empty Custom slot → file/open with filter:"ground" (the host
//      pops the DDS/TGA picker, not the .alo one).
//   3. On a resolved path, the chain dispatches set-ground-slot-custom-path
//      then set-ground-texture in order.
//   4. Height spinner → engine/set/ground-z.
//   5. Solid-colour tile → selects slot 4; native colour input →
//      engine/set/ground-solid-color.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { GroundTexturePanel } from "../GroundTexturePanel";
import { hexToColorref } from "@/lib/colorref";
import type { Bridge } from "@particle-editor/bridge-schema";

type RequestFn = (req: { kind: string; params?: Record<string, unknown> }) => Promise<unknown>;

function makeStubBridge(
  opts: { fileOpen?: { ok: true; path: string } | { ok: false; error: string } } = {},
): Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> } {
  const snapshot = {
    ground: true,
    groundZ: 0,
    groundTexture: 0,
    groundSolidColor: 0x00888888,
    // All 8 slots empty so the custom slots (5..7) render in their
    // Browse... empty state.
    groundSlotCustomPaths: ["", "", "", "", "", "", "", ""],
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
    if (req.kind === "file/open") {
      return Promise.resolve(opts.fileOpen ?? { ok: false, error: "browser-mode" });
    }
    return Promise.resolve({});
  });
  return {
    request,
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

describe("GroundTexturePanel", () => {
  it("clicking a bundled slot dispatches engine/set/ground-texture with the correct slot", () => {
    const bridge = makeStubBridge();
    render(<GroundTexturePanel bridge={bridge} onClose={() => {}} />);
    // Click the Grass tile (slot 1).
    const grass = screen.getByRole("button", { name: "Grass" });
    fireEvent.click(grass);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setSlot = calls.find((c) => c.kind === "engine/set/ground-texture");
    expect(setSlot).toBeDefined();
    expect(setSlot.params.slot).toBe(1);
  });

  it("clicking an empty Custom slot dispatches file/open with filter:\"ground\"", async () => {
    const bridge = makeStubBridge({ fileOpen: { ok: false, error: "browser-mode" } });
    render(<GroundTexturePanel bridge={bridge} onClose={() => {}} />);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ }));
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
      const open = calls.find((c) => c.kind === "file/open");
      expect(open).toBeDefined();
      expect(open.params).toEqual({ filter: "ground" });
    });
  });

  it("on a resolved path the chain dispatches set-ground-slot-custom-path then set-ground-texture", async () => {
    const bridge = makeStubBridge({ fileOpen: { ok: true, path: "C:/textures/dirt.dds" } });
    render(<GroundTexturePanel bridge={bridge} onClose={() => {}} />);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ })).toBeInTheDocument();
    });
    fireEvent.click(screen.getByRole("button", { name: /Custom slot 1 \(empty\)/ }));
    await waitFor(() => {
      const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0].kind);
      expect(calls).toContain("engine/set/ground-slot-custom-path");
      // The bundled-slot test's set-ground-texture dispatch (slot 0
      // on snapshot mount? no — snapshot only) shouldn't pollute, but
      // we explicitly look for the post-pick activation dispatch.
      const customPathIdx = calls.indexOf("engine/set/ground-slot-custom-path");
      const lastTextureIdx = calls.lastIndexOf("engine/set/ground-texture");
      expect(lastTextureIdx).toBeGreaterThan(customPathIdx);
    });
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setPath = calls.find((c) => c.kind === "engine/set/ground-slot-custom-path");
    expect(setPath.params).toEqual({ slot: 5, path: "C:/textures/dirt.dds" });
    const activateCalls = calls.filter((c) => c.kind === "engine/set/ground-texture");
    const lastActivate = activateCalls[activateCalls.length - 1];
    expect(lastActivate.params).toEqual({ slot: 5 });
  });

  it("changing the Height spinner dispatches engine/set/ground-z", () => {
    const bridge = makeStubBridge();
    render(<GroundTexturePanel bridge={bridge} onClose={() => {}} />);
    // Spinner commits on blur, not keystroke.
    const height = screen.getByRole("textbox", { name: "Ground height" });
    fireEvent.change(height, { target: { value: "5" } });
    fireEvent.blur(height);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setZ = calls.find((c) => c.kind === "engine/set/ground-z");
    expect(setZ).toBeDefined();
    expect(setZ.params.z).toBe(5);
  });

  it("clicking the Solid colour tile selects slot 4; the native colour input dispatches engine/set/ground-solid-color", () => {
    const bridge = makeStubBridge();
    const { container } = render(<GroundTexturePanel bridge={bridge} onClose={() => {}} />);

    // The prominent wide tile both selects the solid-colour slot (4) and
    // (in the host) pops the OS picker via the hidden native input.
    fireEvent.click(screen.getByRole("button", { name: "Solid colour" }));
    let calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setSlot = calls.find((c) => c.kind === "engine/set/ground-texture");
    expect(setSlot?.params.slot).toBe(4);

    // The native <input type="color"> drives the colour change.
    const colorInput = container.querySelector('input[type="color"]') as HTMLInputElement;
    expect(colorInput).toBeTruthy();
    fireEvent.change(colorInput, { target: { value: "#ff0000" } });
    calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setColor = calls.find((c) => c.kind === "engine/set/ground-solid-color");
    expect(setColor).toBeDefined();
    expect(setColor.params.rgb).toBe(hexToColorref("#ff0000"));
  });
});
