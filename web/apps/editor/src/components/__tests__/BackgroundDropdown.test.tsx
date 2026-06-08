// Vitest: BackgroundDropdown renders the toolbar button with a swatch
// preview and opens a popover containing the picker body when clicked.
//
// The actual slot-click dispatch is tested via BackgroundPickerBody's
// own coverage (formerly BackgroundPicker.test.tsx); this spec just
// verifies the trigger + popover wiring.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { BackgroundDropdown } from "../BackgroundDropdown";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeBridge() {
  const snap = {
    paused: false,
    skydomeSlot: 0,
    background: 0x00ff0000, // COLORREF for blue (0x00BBGGRR)
    skydomeCustomPaths: ["", "", ""],
  };
  const request = vi.fn().mockImplementation((req: { kind: string }) => {
    if (req.kind === "engine/state/snapshot") return Promise.resolve(snap);
    return Promise.resolve({ ok: true });
  });
  const on = vi.fn().mockReturnValue(() => {});
  return { request, on } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

describe("BackgroundDropdown", () => {
  it("renders the toolbar trigger button", async () => {
    const b = makeBridge();
    render(<BackgroundDropdown bridge={b} />);
    expect(await screen.findByRole("button", { name: "Background" })).toBeInTheDocument();
  });

  it("clicking the trigger opens the popover with slot buttons", async () => {
    const b = makeBridge();
    render(<BackgroundDropdown bridge={b} />);
    const trigger = await screen.findByRole("button", { name: "Background" });
    fireEvent.click(trigger);
    // Wait for popover to mount and slot buttons to render
    await waitFor(() => {
      expect(screen.getByRole("button", { name: "Solid colour" })).toBeInTheDocument();
    });
    // At least one bundled slot should be present
    expect(screen.getByRole("button", { name: "Storm" })).toBeInTheDocument();
  });
});
