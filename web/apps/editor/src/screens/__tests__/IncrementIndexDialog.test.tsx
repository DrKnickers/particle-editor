// Vitest unit test for the IncrementIndexDialog (Screen 4 Batch B1).
// Verifies that the modal renders a Spinner and OK fires
// `emitters/duplicate-with-index-increment`.

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { IncrementIndexDialog } from "../IncrementIndexDialog";
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

describe("IncrementIndexDialog", () => {
  it("renders the spinner and OK fires emitters/duplicate-with-index-increment with the default delta", () => {
    const bridge = makeStubBridge();
    useTreeContextStore.getState().openDialog("increment", 3);
    render(<IncrementIndexDialog bridge={bridge} />);

    // The Spinner primitive renders a numeric input with the
    // aria-label we passed in.
    expect(screen.getByLabelText("Increment by")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "emitters/duplicate-with-index-increment",
      params: { id: 3, delta: 1 },
    });
  });
});
