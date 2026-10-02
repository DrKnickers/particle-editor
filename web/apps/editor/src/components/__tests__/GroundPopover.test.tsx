// Vitest: GroundPopover renders the toolbar button with a swatch
// preview and opens a popover containing the picker body when clicked.
//
// The actual slot-click dispatch is tested via GroundPopoverBody's
// own coverage (GroundPopoverBody.test.tsx); this spec just verifies
// the trigger + popover wiring. Mirrors BackgroundPopover.test.tsx.

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { GroundPopover } from "../GroundPopover";
import { makeBridgeStub } from "@/test/bridge-stub";

function makeBridge() {
  return makeBridgeStub({
    responses: {
      "engine/state/snapshot": {
        paused: false,
        ground: true,
        groundTexture: 0,
        groundSolidColor: 0x00888888,
        groundSlotCustomPaths: ["", "", "", "", "", "", "", ""],
      },
    },
    fallback: { ok: true },
  });
}

describe("GroundPopover", () => {
  it("renders the toolbar trigger button", async () => {
    const b = makeBridge();
    render(<GroundPopover bridge={b} />);
    expect(await screen.findByRole("button", { name: "Ground" })).toBeInTheDocument();
  });

  it("clicking the trigger opens the popover with slot buttons", async () => {
    const b = makeBridge();
    render(<GroundPopover bridge={b} />);
    const trigger = await screen.findByRole("button", { name: "Ground" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: "Solid colour" })).toBeInTheDocument();
    });
    expect(screen.getByRole("button", { name: "Dirt" })).toBeInTheDocument();
  });
});
