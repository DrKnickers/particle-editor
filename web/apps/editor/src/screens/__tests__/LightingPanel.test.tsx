// Vitest unit tests for the LightingPanel.
// Verifies: Sun section renders 3 Spinners + 2 ColorButtons; changing
// Sun intensity dispatches engine/set/light with which: "sun"; the
// Mirror Sun button dispatches engine/set/light for both fills.

import { describe, it, expect, vi } from "vitest";
import { render as rtlRender, screen, fireEvent } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { LightingPanel } from "../LightingPanel";
import type { Bridge } from "@particle-editor/bridge-schema";

//: the Mirror Sun button mounts a Tip (Radix Tooltip.Root) while
// disabled, which requires the app-level Tooltip.Provider — wrapper stands
// in for it (precedent: renderToolbar in Toolbar.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

function makeStubBridge(): Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> } {
  // Bridge stub: snapshot returns a minimal EngineStateDto so the
  // panel's seed branch runs without throwing; everything else
  // resolves with `{}` for fire-and-forget setters.
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
      sun: { diffuse: [0.7, 0.7, 0.75, 1], specular: [0.75, 0.75, 0.8, 1], position: [1, 0, 0.7, 0], direction: [0, 0, 0, 0] },
      fill1: { diffuse: [0.24, 0.31, 0.62, 1], specular: [0, 0, 0, 1], position: [-0.5, 0.85, -0.17, 0], direction: [0, 0, 0, 0] },
      fill2: { diffuse: [0.24, 0.31, 0.62, 1], specular: [0, 0, 0, 1], position: [-0.85, -0.5, -0.17, 0], direction: [0, 0, 0, 0] },
    },
    ambient: [0.16, 0.16, 0.2, 1],
    shadow: [0.4, 0.4, 0.43, 1],
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
  return {
    request: vi.fn().mockImplementation((req: { kind: string }) => {
      if (req.kind === "engine/state/snapshot") return Promise.resolve(snapshot);
      return Promise.resolve({});
    }),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn>; on: ReturnType<typeof vi.fn> };
}

describe("LightingPanel", () => {
  it("Sun section renders 3 Spinners + 2 ColorButtons", () => {
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    // Spinners are <input type="text"> with aria-label.
    expect(screen.getByLabelText("Sun intensity")).toBeInTheDocument();
    expect(screen.getByLabelText("Sun azimuth")).toBeInTheDocument();
    expect(screen.getByLabelText("Sun altitude")).toBeInTheDocument();
    // ColorButtons are <button> with aria-label.
    expect(screen.getByRole("button", { name: "Sun diffuse colour" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Sun specular colour" })).toBeInTheDocument();
  });

  it("changing Sun intensity dispatches engine/set/light with which: 'sun'", () => {
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    const intensity = screen.getByLabelText("Sun intensity") as HTMLInputElement;
    // The Spinner commits on blur. Enter sends Enter→blur internally.
    fireEvent.change(intensity, { target: { value: "1.25" } });
    fireEvent.blur(intensity);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const setLight = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "sun");
    expect(setLight).toBeDefined();
  });

  it("Mirror Sun button dispatches engine/set/light for both fills (with Force Align disabled)", () => {
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    // Group D: Mirror Sun is disabled while Force Align is on
    // (matches legacy — Mirror Sun is undefined while the fill angles
    // are pinned to sun.az + offset). Toggle Force Align off first.
    const forceAlign = screen.getByLabelText("Force Align Fill Lights") as HTMLInputElement;
    expect(forceAlign.checked).toBe(true); // default ON per legacy
    fireEvent.click(forceAlign);
    expect(forceAlign.checked).toBe(false);
    const btn = screen.getByRole("button", { name: "Mirror Sun" });
    fireEvent.click(btn);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const fill1Call = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "fill1");
    const fill2Call = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "fill2");
    expect(fill1Call).toBeDefined();
    expect(fill2Call).toBeDefined();
  });

  it("Reset pushes ambient + shadow with alpha 0 (matches the as-loaded engine state)", () => {
    // Regression guard for the load≠Reset brightness bug. The engine folds
    // ambient into the SPH lighting as `ambient.xyz * ambient.w`
    // (src/SphericalHarmonics.cpp:76), and the load path (host restore /
    // legacy ColorToVec4) pushes ambient with w=0. Reset must push w=0 too,
    // or it lights an ambient floor the as-loaded scene leaves dark and the
    // whole render brightens. Pin the exact bit: color[3] === 0.
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    fireEvent.click(screen.getByRole("button", { name: "Reset" }));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const ambient = calls.find((c) => c.kind === "engine/set/ambient");
    const shadow = calls.find((c) => c.kind === "engine/set/shadow");
    expect(ambient).toBeDefined();
    expect(shadow).toBeDefined();
    expect(ambient.params.color[3]).toBe(0);
    expect(shadow.params.color[3]).toBe(0);
  });

  it("Reset keeps sun diffuse/specular at alpha 1 (no regression to the per-light round-trip)", () => {
    // The ambient/shadow w=0 above is deliberate, but the lights must stay
    // w=1 (buildLightDto folds intensity into rgb, alpha=1) — mirroring
    // legacy MakeLight. Guard the asymmetry so a future "consistency" edit
    // can't zero the light alpha too.
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    fireEvent.click(screen.getByRole("button", { name: "Reset" }));
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    const sun = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "sun");
    expect(sun).toBeDefined();
    expect(sun.params.diffuse[3]).toBe(1);
    expect(sun.params.specular[3]).toBe(1);
  });

  it("Force Align ON cascades sun.az to fill1/fill2 azimuth ()", () => {
    const bridge = makeStubBridge();
    render(<LightingPanel bridge={bridge} onClose={() => {}} />);
    // Default state: Force Align is ON. Changing sun.az should
    // dispatch fill1 + fill2 with the offset values.
    const sunAz = screen.getByLabelText("Sun azimuth") as HTMLInputElement;
    fireEvent.change(sunAz, { target: { value: "30" } });
    fireEvent.blur(sunAz);
    const calls = (bridge.request as ReturnType<typeof vi.fn>).mock.calls.map((c) => c[0]);
    // The fill az values come through the LightDto's direction vector
    // (not as raw az/alt) — so we verify a fill light call fired, not
    // the exact azimuth. The az/alt → direction conversion is covered
    // by the existing buildLightDto unit tests.
    const fill1Call = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "fill1");
    const fill2Call = calls.find((c) => c.kind === "engine/set/light" && c.params.which === "fill2");
    expect(fill1Call).toBeDefined();
    expect(fill2Call).toBeDefined();
  });
});
