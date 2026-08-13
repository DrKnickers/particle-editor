// Vitest unit test for the Rescale System dialog.
// Verifies that clicking OK fires the `engine/action/rescale-system`
// bridge call with the current spinner values.

import { describe, it, expect } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { RescaleDialog } from "../RescaleDialog";
import { makeBridgeStub } from "./bridge-stub";

describe("RescaleDialog", () => {
  it("clicking OK fires engine/action/rescale-system with current spinner values", () => {
    const bridge = makeBridgeStub();
    render(
      <RescaleDialog bridge={bridge} open onOpenChange={() => {}} />
    );
    // Defaults: durationScale=100, sizeScale=100. We click OK directly
    // (no spinner edits) so the call payload reflects the initial state.
    const okBtn = screen.getByRole("button", { name: "OK" });
    fireEvent.click(okBtn);
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "engine/action/rescale-system",
      params: {
        durationScalePercent: 100,
        sizeScalePercent: 100,
      },
    });
  });
});
