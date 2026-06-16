// Vitest: Toolbar renders the 4 groups and dispatches the right bridge
// calls on click. ThemeToggle has been moved to Edit → Preferences….

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { Toolbar } from "../Toolbar";
import type { Bridge } from "@particle-editor/bridge-schema";
import { __resetRightDockForTests } from "@/lib/right-dock";

//: toolbar buttons mount a Tip (Radix Tooltip.Root), which requires
// the Tooltip.Provider that App.tsx supplies in production — this helper
// stands in for it here (same precedent as EmitterTree.test.tsx).
const renderToolbar = (bridge: Bridge) =>
  render(
    <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
      <Toolbar bridge={bridge} />
    </Tooltip.Provider>,
  );

function makeBridge() {
  const snap = {
    paused: false,
    bloom: false,
    bloomAvailable: true,
    ground: true,
    gridVisible: true,
    heatDebug: false,
    canUndo: true,
    canRedo: false,
  };
  const request = vi.fn().mockImplementation((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") return Promise.resolve(snap);
    return Promise.resolve({});
  });
  const on = vi.fn().mockReturnValue(() => {});
  return { request, on } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  localStorage.removeItem("alo:right-dock");
  localStorage.removeItem("alo:spawner-visible");
  // The Zustand store's `dock` state is module-level and survives
  // across tests; clearing localStorage isn't enough on its own.
  __resetRightDockForTests();
});

describe("Toolbar — Particle Editor 2026 layout", () => {
  it("renders the file/playback/spawner groups", async () => {
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() => expect(screen.getByRole("button", { name: "Pause" })).toBeInTheDocument());
    expect(screen.getByRole("button", { name: "New" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Open" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Save" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Save As" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Step" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Step 10" })).toBeInTheDocument();
    expect(screen.getByRole("button", { name: "Toggle Spawner panel" })).toBeInTheDocument();
    // ThemeToggle removed from toolbar — theme is in Edit → Preferences…
    expect(screen.queryByRole("button", { name: /light theme/i })).not.toBeInTheDocument();
    expect(screen.queryByRole("button", { name: /dark theme/i })).not.toBeInTheDocument();
  });

  it("renders Undo/Redo buttons reflecting canUndo/canRedo", async () => {
    // snapshot: canUndo=true, canRedo=false
    const b = makeBridge();
    renderToolbar(b);
    const undo = await screen.findByRole("button", { name: "Undo" });
    const redo = screen.getByRole("button", { name: "Redo" });
    await waitFor(() => expect(undo).not.toBeDisabled());
    expect(redo).toBeDisabled();
  });

  it("Undo button dispatches undo/perform { direction: 'undo' }", async () => {
    const b = makeBridge();
    renderToolbar(b);
    const undo = await screen.findByRole("button", { name: "Undo" });
    await waitFor(() => expect(undo).not.toBeDisabled());
    fireEvent.click(undo);
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "undo" } });
    });
  });

  it("Redo is disabled when canRedo is false (no dispatch on click)", async () => {
    const b = makeBridge();
    renderToolbar(b);
    const redo = await screen.findByRole("button", { name: "Redo" });
    expect(redo).toBeDisabled();
    fireEvent.click(redo);
    expect(b.request).not.toHaveBeenCalledWith({ kind: "undo/perform", params: { direction: "redo" } });
  });

  it("Pause button dispatches engine/set/paused with paused=true", async () => {
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() => expect(screen.getByRole("button", { name: "Pause" })).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Pause" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/paused", params: { paused: true } });
    });
  });

  it("Step 10 dispatches engine/action/step-frames { frames: 10 }", async () => {
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() => expect(screen.getByRole("button", { name: "Step 10" })).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Step 10" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/step-frames", params: { frames: 10 } });
    });
  });

  it("Spawner toggle updates aria-pressed and persists to localStorage", async () => {
    const b = makeBridge();
    renderToolbar(b);
    const btn = await screen.findByRole("button", { name: "Toggle Spawner panel" });
    expect(btn).toHaveAttribute("aria-pressed", "true"); // default dock = spawner
    fireEvent.click(btn);
    expect(btn).toHaveAttribute("aria-pressed", "false");
    expect(localStorage.getItem("alo:right-dock")).toBe("none");
  });

  it("Lighting toggle opens the lighting dock and is exclusive with the Spawner", async () => {
    const b = makeBridge();
    renderToolbar(b);
    const spawnerBtn = await screen.findByRole("button", { name: "Toggle Spawner panel" });
    const lightingBtn = screen.getByRole("button", { name: "Toggle Lighting panel" });
    // Default dock = spawner → Spawner pressed, Lighting not.
    expect(spawnerBtn).toHaveAttribute("aria-pressed", "true");
    expect(lightingBtn).toHaveAttribute("aria-pressed", "false");
    // Opening Lighting swaps the shared slot: Lighting pressed, Spawner not.
    fireEvent.click(lightingBtn);
    expect(lightingBtn).toHaveAttribute("aria-pressed", "true");
    expect(spawnerBtn).toHaveAttribute("aria-pressed", "false");
    expect(localStorage.getItem("alo:right-dock")).toBe("lighting");
    // Clicking Lighting again closes the column entirely.
    fireEvent.click(lightingBtn);
    expect(lightingBtn).toHaveAttribute("aria-pressed", "false");
    expect(localStorage.getItem("alo:right-dock")).toBe("none");
  });

  // ── Viewport engine toggles (moved here from the deleted ViewportPill) ──

  it("renders the three viewport toggles with their aria-labels", async () => {
    const b = makeBridge();
    renderToolbar(b);
    expect(
      await screen.findByRole("button", { name: "Show ground" }),
    ).toBeInTheDocument();
    expect(
      screen.getByRole("button", { name: "Toggle bloom" }),
    ).toBeInTheDocument();
    expect(
      screen.getByRole("button", {
        name: "Leave particles after instance death",
      }),
    ).toBeInTheDocument();
  });

  it("reflects the engine snapshot on the viewport toggles via aria-pressed", async () => {
    // makeBridge snapshot: ground=true, bloom=false, leaveParticles absent
    // (so it falls back to the default `true`).
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() =>
      expect(screen.getByRole("button", { name: "Show ground" })).toHaveAttribute(
        "aria-pressed",
        "true",
      ),
    );
    expect(
      screen.getByRole("button", { name: "Toggle bloom" }),
    ).toHaveAttribute("aria-pressed", "false");
    expect(
      screen.getByRole("button", {
        name: "Leave particles after instance death",
      }),
    ).toHaveAttribute("aria-pressed", "true");
  });

  it("renders the Spawner toggle as an icon, not a text label", async () => {
    const b = makeBridge();
    renderToolbar(b);
    const btn = await screen.findByRole("button", {
      name: "Toggle Spawner panel",
    });
    expect(btn).not.toHaveTextContent("Spawner");
    expect(btn.querySelector("svg")).toBeTruthy();
  });

  it("clicking a viewport toggle dispatches engine/set/* with the inverted value", async () => {
    const b = makeBridge();
    renderToolbar(b);
    // Wait for the snapshot so ground=true is reflected before clicking.
    await waitFor(() =>
      expect(screen.getByRole("button", { name: "Show ground" })).toHaveAttribute(
        "aria-pressed",
        "true",
      ),
    );
    fireEvent.click(screen.getByRole("button", { name: "Show ground" }));
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/ground",
      params: { enabled: false },
    });
    fireEvent.click(screen.getByRole("button", { name: "Toggle bloom" }));
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/bloom",
      params: { enabled: true },
    });
    fireEvent.click(
      screen.getByRole("button", {
        name: "Leave particles after instance death",
      }),
    );
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/leave-particles",
      params: { enabled: false },
    });
  });

  it("renders a 'Show grid' viewport toggle", async () => {
    const b = makeBridge();
    renderToolbar(b);
    expect(
      await screen.findByRole("button", { name: "Show grid" }),
    ).toBeInTheDocument();
  });

  it("reflects gridVisible on the 'Show grid' toggle via aria-pressed", async () => {
    // makeBridge snapshot: gridVisible=true
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() =>
      expect(screen.getByRole("button", { name: "Show grid" })).toHaveAttribute(
        "aria-pressed",
        "true",
      ),
    );
  });

  it("'Show grid' dispatches engine/set/grid-visible with the inverted value (visible key, not enabled)", async () => {
    const b = makeBridge();
    renderToolbar(b);
    await waitFor(() =>
      expect(screen.getByRole("button", { name: "Show grid" })).toHaveAttribute(
        "aria-pressed",
        "true",
      ),
    );
    fireEvent.click(screen.getByRole("button", { name: "Show grid" }));
    expect(b.request).toHaveBeenCalledWith({
      kind: "engine/set/grid-visible",
      params: { visible: false },
    });
  });
});
