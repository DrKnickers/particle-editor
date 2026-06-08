// Vitest unit test for the RescaleEmitterDialog (Screen 4 Batch B1).
// Verifies that OK fires `engine/action/rescale-emitter` with the
// current spinner values.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { RescaleEmitterDialog } from "../RescaleEmitterDialog";
import { useTreeContextStore } from "@/lib/tree-context";
import type { Bridge } from "@particle-editor/bridge-schema";

function makeStubBridge(): Bridge & { request: ReturnType<typeof vi.fn> } {
  return {
    request: vi.fn().mockResolvedValue({}),
    on: vi.fn().mockReturnValue(() => {}),
  } as unknown as Bridge & { request: ReturnType<typeof vi.fn> };
}

beforeEach(() => {
  useTreeContextStore.getState().close();
});

describe("RescaleEmitterDialog", () => {
  it("clicking OK fires engine/action/rescale-emitter with default 100/100", () => {
    const bridge = makeStubBridge();
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
