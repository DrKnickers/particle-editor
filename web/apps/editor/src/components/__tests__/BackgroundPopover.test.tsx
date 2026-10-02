// Vitest: BackgroundPopover renders the toolbar button with a swatch
// preview and opens a popover containing the picker body when clicked.
//
// The actual slot-click dispatch is tested via BackgroundPopoverBody's
// own coverage (BackgroundPopoverBody.test.tsx); this spec just
// verifies the trigger + popover wiring.

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { BackgroundPopover } from "../BackgroundPopover";
import { makeBridgeStub } from "@/test/bridge-stub";

function makeBridge() {
  return makeBridgeStub({
    responses: {
      "engine/state/snapshot": {
        paused: false,
        skydomeSlot: 0,
        background: 0x00ff0000, // COLORREF for blue (0x00BBGGRR)
        skydomeCustomPaths: ["", "", ""],
      },
    },
    fallback: { ok: true },
  });
}

describe("BackgroundPopover", () => {
  it("renders the toolbar trigger button", async () => {
    const b = makeBridge();
    render(<BackgroundPopover bridge={b} />);
    expect(await screen.findByRole("button", { name: "Background" })).toBeInTheDocument();
  });

  it("clicking the trigger opens the popover with the picker body", async () => {
    const b = makeBridge();
    render(<BackgroundPopover bridge={b} />);
    const trigger = await screen.findByRole("button", { name: "Background" });
    fireEvent.click(trigger);
    // Wait for the popover to mount and the picker body to render.
    await waitFor(() => {
      expect(screen.getByRole("button", { name: "Solid colour" })).toBeInTheDocument();
    });
    // the game-dome section's primary selector is present.
    expect(screen.getByRole("combobox", { name: "Primary dome" })).toBeInTheDocument();
  });
});
