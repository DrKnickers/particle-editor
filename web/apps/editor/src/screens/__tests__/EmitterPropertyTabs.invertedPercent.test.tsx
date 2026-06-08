import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";

// Import FieldSpinner — it's not exported today; we'll export it as
// part of this task. If the import fails, the test fails red as
// expected for a TDD step.
import { FieldSpinner } from "../EmitterPropertyTabs";

describe("FieldSpinner displayInvertedPercent", () => {
  it("displays 100 - value*100 rounded to integer", () => {
    const onCommit = vi.fn();
    render(
      <FieldSpinner
        label="Minimum lifetime"
        value={0.25}
        displayInvertedPercent
        unit="%"
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Minimum lifetime") as HTMLInputElement;
    expect(input.value).toBe("75");
  });

  it("commits (100 - displayed) / 100 on change", () => {
    const onCommit = vi.fn();
    render(
      <FieldSpinner
        label="Minimum scale"
        value={0.5}
        displayInvertedPercent
        unit="%"
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Minimum scale") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "30" } });
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledWith(0.7);
  });

  it("round-trips boundary values 0 / 100", () => {
    const onCommit = vi.fn();
    const { rerender } = render(
      <FieldSpinner
        label="Field"
        value={0}
        displayInvertedPercent
        unit="%"
        onCommit={onCommit}
      />,
    );
    expect((screen.getByLabelText("Field") as HTMLInputElement).value).toBe("100");
    rerender(
      <FieldSpinner
        label="Field"
        value={1}
        displayInvertedPercent
        unit="%"
        onCommit={onCommit}
      />,
    );
    expect((screen.getByLabelText("Field") as HTMLInputElement).value).toBe("0");
  });
});
