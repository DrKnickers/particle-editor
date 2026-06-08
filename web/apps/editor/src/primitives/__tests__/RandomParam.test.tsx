// Vitest unit tests for the RandomParam primitive.
// Exercises: mode switch renders correct spinners, max-spinner onChange,
// Normal mode shows µ/σ labels.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import { RandomParam } from "../RandomParam";
import type { RandomParamValue } from "../RandomParam";
import { useState } from "react";

// Wrapper that holds state so onChange works.
function Harness({ initial }: { initial: RandomParamValue }) {
  const [val, setVal] = useState<RandomParamValue>(initial);
  return <RandomParam value={val} onChange={setVal} step={1} decimals={0} />;
}

describe("RandomParam", () => {
  it("switching mode Constant → UniformRange renders 2 spinners", () => {
    // Radix Select's open/close interaction is not reliable in jsdom due to
    // pointer-event limitations. We test the rendered output by re-rendering
    // with the new mode (simulating what onChange + setState produces). The
    // Playwright spec covers the full end-to-end mode-switch click flow.
    const { rerender } = render(
      <RandomParam
        value={{ mode: "Constant", value: 0 }}
        onChange={() => {}}
        step={1}
        decimals={0}
      />
    );
    // Initially 1 spinner.
    expect(screen.getAllByRole("textbox")).toHaveLength(1);

    // Re-render with UniformRange (simulating the onChange → setState cycle).
    rerender(
      <RandomParam
        value={{ mode: "UniformRange", min: 0, max: 1 }}
        onChange={() => {}}
        step={1}
        decimals={0}
      />
    );
    // After mode change: 2 spinners (min + max).
    expect(screen.getAllByRole("textbox")).toHaveLength(2);
  });

  it("modifying the max spinner fires onChange with the new max", () => {
    const onChange = vi.fn();
    render(
      <RandomParam
        value={{ mode: "UniformRange", min: 1, max: 5 }}
        onChange={onChange}
        step={1}
        decimals={0}
      />
    );
    // The two spinners: [0]=min, [1]=max.
    const inputs = screen.getAllByRole("textbox");
    const maxInput = inputs[1];
    // Focus → change text → blur → commit.
    fireEvent.focus(maxInput);
    fireEvent.change(maxInput, { target: { value: "10" } });
    fireEvent.blur(maxInput);
    expect(onChange).toHaveBeenCalledWith(
      expect.objectContaining({ mode: "UniformRange", max: 10 })
    );
  });

  it("Normal mode shows µ and σ labels", async () => {
    render(<Harness initial={{ mode: "Normal", mean: 1, sigma: 0.5 }} />);
    // The µ and σ labels should be visible in the DOM.
    expect(screen.getByText("µ")).toBeInTheDocument();
    expect(screen.getByText("σ")).toBeInTheDocument();
    // And 2 spinners (mean + sigma).
    expect(screen.getAllByRole("textbox")).toHaveLength(2);
  });
});
