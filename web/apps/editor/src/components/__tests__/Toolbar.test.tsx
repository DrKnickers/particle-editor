// Vitest: Toolbar renders the 4 groups and dispatches the right bridge
// calls on click. ThemeToggle has been moved to Edit → Preferences….

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { Toolbar } from "../Toolbar";
import type { Bridge } from "@particle-editor/bridge-schema";
import { __resetRightDockForTests } from "@/lib/right-dock";

function makeBridge() {
  const snap = {
    paused: false,
    bloom: false,
    bloomAvailable: true,
    ground: true,
    heatDebug: false,
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
    render(<Toolbar bridge={b} />);
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

  it("Pause button dispatches engine/set/paused with paused=true", async () => {
    const b = makeBridge();
    render(<Toolbar bridge={b} />);
    await waitFor(() => expect(screen.getByRole("button", { name: "Pause" })).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Pause" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({ kind: "engine/set/paused", params: { paused: true } });
    });
  });

  it("Step 10 dispatches engine/action/step-frames { frames: 10 }", async () => {
    const b = makeBridge();
    render(<Toolbar bridge={b} />);
    await waitFor(() => expect(screen.getByRole("button", { name: "Step 10" })).toBeInTheDocument());
    fireEvent.click(screen.getByRole("button", { name: "Step 10" }));
    await waitFor(() => {
      expect(b.request).toHaveBeenCalledWith({ kind: "engine/action/step-frames", params: { frames: 10 } });
    });
  });

  it("Spawner toggle updates aria-pressed and persists to localStorage", async () => {
    const b = makeBridge();
    render(<Toolbar bridge={b} />);
    const btn = await screen.findByRole("button", { name: "Toggle Spawner panel" });
    expect(btn).toHaveAttribute("aria-pressed", "true"); // default dock = spawner
    fireEvent.click(btn);
    expect(btn).toHaveAttribute("aria-pressed", "false");
    expect(localStorage.getItem("alo:right-dock")).toBe("none");
  });

  it("Lighting toggle opens the lighting dock and is exclusive with the Spawner", async () => {
    const b = makeBridge();
    render(<Toolbar bridge={b} />);
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
    render(<Toolbar bridge={b} />);
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
    render(<Toolbar bridge={b} />);
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
    render(<Toolbar bridge={b} />);
    const btn = await screen.findByRole("button", {
      name: "Toggle Spawner panel",
    });
    expect(btn).not.toHaveTextContent("Spawner");
    expect(btn.querySelector("svg")).toBeTruthy();
  });

  it("clicking a viewport toggle dispatches engine/set/* with the inverted value", async () => {
    const b = makeBridge();
    render(<Toolbar bridge={b} />);
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
});
