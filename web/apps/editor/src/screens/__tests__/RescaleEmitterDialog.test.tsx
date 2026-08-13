// Vitest unit test for the RescaleEmitterDialog.
// Verifies that OK fires `engine/action/rescale-emitter` with the
// current spinner values.

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { RescaleEmitterDialog } from "../RescaleEmitterDialog";
import { useTreeContextStore } from "@/lib/tree-context";
import { makeBridgeStub } from "./bridge-stub";

beforeEach(() => {
  useTreeContextStore.getState().close();
});

describe("RescaleEmitterDialog", () => {
  it("clicking OK fires engine/action/rescale-emitter with default 100/100", () => {
    const bridge = makeBridgeStub();
    useTreeContextStore.getState().openDialog("rescale", 4);
    render(<RescaleEmitterDialog bridge={bridge} />);

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "engine/action/rescale-emitter",
      params: {
        id: 4,
        durationScalePercent: 100,
        sizeScalePercent: 100,
      },
    });
  });
});
