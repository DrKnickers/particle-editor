// Vitest unit test for the IncrementIndexDialog.
// Verifies that the modal renders the delta + repeat spinners and OK fires
// `emitters/duplicate-with-index-increment-many` with both values (#575).

import { describe, it, expect, beforeEach } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { IncrementIndexDialog } from "../IncrementIndexDialog";
import { useTreeContextStore } from "@/lib/tree-context";
import { makeBridgeStub } from "./bridge-stub";

// The Spinner commits on blur/Enter, not on keystroke — set the text then blur.
function setSpinner(label: string, value: string): void {
  const input = screen.getByLabelText(label);
  fireEvent.change(input, { target: { value } });
  fireEvent.blur(input);
}

beforeEach(() => {
  useTreeContextStore.getState().close();
});

describe("IncrementIndexDialog", () => {
  it("renders both spinners and OK fires the batch request with default delta + count", () => {
    const bridge = makeBridgeStub();
    useTreeContextStore.getState().openDialog("increment", 3);
    render(<IncrementIndexDialog bridge={bridge} />);

    // Both Spinners render numeric inputs with the aria-labels we passed in.
    expect(screen.getByLabelText("Increment by")).toBeTruthy();
    expect(screen.getByLabelText("Repeat count")).toBeTruthy();

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: 3, delta: 1, count: 1 },
    });
  });

  it("passes the chosen delta and repeat count", () => {
    const bridge = makeBridgeStub();
    useTreeContextStore.getState().openDialog("increment", 7);
    render(<IncrementIndexDialog bridge={bridge} />);

    setSpinner("Increment by", "2");
    setSpinner("Repeat count", "5");

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: 7, delta: 2, count: 5 },
    });
  });

  it("rounds a fractional repeat count so the sent value matches the displayed integer", () => {
    const bridge = makeBridgeStub();
    useTreeContextStore.getState().openDialog("increment", 1);
    render(<IncrementIndexDialog bridge={bridge} />);

    setSpinner("Repeat count", "2.7"); // Spinner shows "3" (decimals=0)

    fireEvent.click(screen.getByRole("button", { name: "OK" }));
    expect(bridge.request).toHaveBeenCalledWith({
      kind: "emitters/duplicate-with-index-increment-many",
      params: { id: 1, delta: 1, count: 3 },
    });
  });
});
