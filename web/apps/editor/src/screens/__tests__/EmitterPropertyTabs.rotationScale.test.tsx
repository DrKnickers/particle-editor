import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";

import { FieldSpinner } from "../EmitterPropertyTabs";

// PRM-4 / PRM-5: the legacy panel displays rotation average as
// `stored * 360` (integer degrees, -180..180) and commits `typed / 360`,
// and rotation variance as `stored * 100` (integer 0..100) committing
// `typed / 100` (Emitter.cpp:498-499, 828-829). The host serialises the
// raw stored ratio, so the display transform must live in the new UI.
// `displayScale` is the general form of that transform.
describe("FieldSpinner displayScale (rotation average / variance)", () => {
  it("displays value * scale (average: 0.25 -> 90°)", () => {
    const onCommit = vi.fn();
    render(
      <FieldSpinner
        label="Rotation average"
        value={0.25}
        displayScale={360}
        min={-180}
        max={180}
        step={1}
        decimals={0}
        unit="°"
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Rotation average") as HTMLInputElement;
    expect(input.value).toBe("90");
  });

  it("commits displayed / scale (average: type 90 -> 0.25)", () => {
    const onCommit = vi.fn();
    render(
      <FieldSpinner
        label="Rotation average"
        value={0}
        displayScale={360}
        min={-180}
        max={180}
        step={1}
        decimals={0}
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Rotation average") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "90" } });
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledWith(0.25);
  });

  it("variance scale 100: 0.5 -> 50, commit 30 -> 0.3", () => {
    const onCommit = vi.fn();
    const { rerender } = render(
      <FieldSpinner
        label="Rotation variance"
        value={0.5}
        displayScale={100}
        min={0}
        max={100}
        step={1}
        decimals={0}
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Rotation variance") as HTMLInputElement;
    expect(input.value).toBe("50");
    fireEvent.change(input, { target: { value: "30" } });
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledWith(0.3);
    rerender(
      <FieldSpinner
        label="Rotation variance"
        value={1}
        displayScale={100}
        min={0}
        max={100}
        step={1}
        decimals={0}
        onCommit={onCommit}
      />,
    );
    expect((screen.getByLabelText("Rotation variance") as HTMLInputElement).value).toBe("100");
  });

  it("average clamps display to -180..180 (typing 200 clamps to 180 -> 0.5)", () => {
    const onCommit = vi.fn();
    render(
      <FieldSpinner
        label="Rotation average"
        value={0}
        displayScale={360}
        min={-180}
        max={180}
        step={1}
        decimals={0}
        onCommit={onCommit}
      />,
    );
    const input = screen.getByLabelText("Rotation average") as HTMLInputElement;
    fireEvent.change(input, { target: { value: "200" } });
    fireEvent.blur(input);
    expect(onCommit).toHaveBeenCalledWith(0.5);
  });
});
