// Vitest: GroundDropdown renders the toolbar button with a swatch
// preview and opens a popover containing the picker body when clicked.
//
// The actual slot-click dispatch is tested via GroundTexturePanelBody's
// own coverage (GroundTexturePanel.test.tsx); this spec just verifies
// the trigger + popover wiring. Mirrors BackgroundDropdown.test.tsx.

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import { GroundDropdown } from "../GroundDropdown";
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

describe("GroundDropdown", () => {
  it("renders the toolbar trigger button", async () => {
    const b = makeBridge();
    render(<GroundDropdown bridge={b} />);
    expect(await screen.findByRole("button", { name: "Ground" })).toBeInTheDocument();
  });

  it("clicking the trigger opens the popover with slot buttons", async () => {
    const b = makeBridge();
    render(<GroundDropdown bridge={b} />);
    const trigger = await screen.findByRole("button", { name: "Ground" });
    fireEvent.click(trigger);
    await waitFor(() => {
      expect(screen.getByRole("button", { name: "Solid colour" })).toBeInTheDocument();
    });
    expect(screen.getByRole("button", { name: "Dirt" })).toBeInTheDocument();
  });
});
