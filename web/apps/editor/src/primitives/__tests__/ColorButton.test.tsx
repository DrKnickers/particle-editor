// Vitest unit tests for the ColorButton primitive.
// Exercises: popover opens, basic color fires onChange, "Add to custom" fills a slot,
// live preview (slider/number/hex fire onChange as you go), cancel/revert
// (Cancel button + Escape restore the open-time color; OK keeps), and the UX extras
// (before/after swatch, editable R/G/B inputs, Enter-in-hex commits + closes).

import { describe, it, expect, vi, beforeEach } from "vitest";
import { render as rtlRender, screen, fireEvent, waitFor } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import * as Tooltip from "@radix-ui/react-tooltip";
import type { ReactElement, ReactNode } from "react";
import { ColorButton } from "../ColorButton";
import { usePaletteStore } from "../palette-store";

//: the swatch grids mount Tips (Radix Tooltip.Root), which require
// the app-level Tooltip.Provider — wrapper stands in for it (precedent:
// renderToolbar in Toolbar.test.tsx).
const TipProvider = ({ children }: { children: ReactNode }) => (
  <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>{children}</Tooltip.Provider>
);
const render = (ui: ReactElement) => rtlRender(ui, { wrapper: TipProvider });

const RED = { r: 255, g: 0, b: 0 };

// Reset the palette store between tests.
beforeEach(() => {
  usePaletteStore.setState({
    slots: Array(16).fill(null) as null[],
  });
});

// Open the popover and wait for its content to mount.
async function openPicker(label = "color-btn") {
  await userEvent.click(screen.getByRole("button", { name: new RegExp(label, "i") }));
  await waitFor(() => screen.getByText("Basic colors"));
}

describe("ColorButton", () => {
  it("popover opens on button click", async () => {
    render(<ColorButton value={RED} onChange={() => {}} aria-label="color-btn" />);
    await openPicker();
    expect(screen.getByText("Basic colors")).toBeInTheDocument();
  });

  it("clicking a basic color fires onChange with that RGB", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    // The first basic color is { r:255, g:128, b:128 } — aria-label contains "#FF8080".
    fireEvent.click(screen.getByRole("button", { name: /Basic color #FF8080/i }));
    expect(onChange).toHaveBeenCalledWith({ r: 255, g: 128, b: 128 });
  });

  it("'Add to custom' places the picker color into the first empty custom slot", async () => {
    render(<ColorButton value={{ r: 0, g: 200, b: 100 }} onChange={() => {}} aria-label="color-btn" />);
    await openPicker();
    fireEvent.click(screen.getByRole("button", { name: /Add to custom colors/i }));
    expect(usePaletteStore.getState().slots[0]).toEqual({ r: 0, g: 200, b: 100 });
  });

  // ---: live preview ---------------------------------------------------

  it("dragging an RGB slider fires onChange live, not just on release ()", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    expect(onChange).toHaveBeenLastCalledWith({ r: 255, g: 0, b: 255 });
  });

  it("typing in an R/G/B number input fires onChange live and clamps ()", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("spinbutton", { name: /^G value$/i }), {
      target: { value: "300" },
    });
    expect(onChange).toHaveBeenLastCalledWith({ r: 255, g: 255, b: 0 }); // clamped to 255
  });

  it("typing a valid hex fires onChange live; an invalid hex does not ()", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    const hex = screen.getByRole("textbox", { name: /Hex color input/i });
    fireEvent.change(hex, { target: { value: "00FF00" } });
    expect(onChange).toHaveBeenLastCalledWith({ r: 0, g: 255, b: 0 });
    onChange.mockClear();
    fireEvent.change(hex, { target: { value: "ZZ" } });
    expect(onChange).not.toHaveBeenCalled();
  });

  // ---: cancel / revert ------------------------------------------------

  it("Cancel reverts the engine to the color the picker opened with ()", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    expect(onChange).toHaveBeenLastCalledWith({ r: 255, g: 0, b: 255 });
    fireEvent.click(screen.getByRole("button", { name: /^Cancel$/i }));
    expect(onChange).toHaveBeenLastCalledWith(RED);
  });

  it("Escape reverts to the open-time color ()", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    fireEvent.keyDown(document, { key: "Escape", code: "Escape" });
    expect(onChange).toHaveBeenLastCalledWith(RED);
  });

  it("OK keeps the edited color (no revert)", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    fireEvent.click(screen.getByRole("button", { name: /^OK$/i }));
    expect(onChange).toHaveBeenLastCalledWith({ r: 255, g: 0, b: 255 });
  });

  // --- UX extras -------------------------------------------------------------

  it("the before/after swatch shows the original color while editing", async () => {
    render(<ColorButton value={RED} onChange={() => {}} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    expect(screen.getByTestId("color-original")).toHaveStyle({
      backgroundColor: "rgb(255, 0, 0)",
    });
  });

  it("Enter in the hex field commits and closes the popover", async () => {
    const onChange = vi.fn();
    render(<ColorButton value={RED} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    const hex = screen.getByRole("textbox", { name: /Hex color input/i });
    fireEvent.change(hex, { target: { value: "00FF00" } });
    fireEvent.keyDown(hex, { key: "Enter" });
    expect(onChange).toHaveBeenCalledWith({ r: 0, g: 255, b: 0 });
    await waitFor(() => expect(screen.queryByText("Basic colors")).not.toBeInTheDocument());
  });

  // --- lifecycle -------------------------------------------------------------

  it("reopening after an external value change re-snapshots the fresh original", async () => {
    const onChange = vi.fn();
    const { rerender } = render(
      <ColorButton value={RED} onChange={onChange} aria-label="color-btn" />
    );
    // Value changes externally before the user opens the picker.
    rerender(<ColorButton value={{ r: 0, g: 255, b: 0 }} onChange={onChange} aria-label="color-btn" />);
    await openPicker();
    fireEvent.change(screen.getByRole("slider", { name: /^B channel$/i }), {
      target: { value: "255" },
    });
    fireEvent.click(screen.getByRole("button", { name: /^Cancel$/i }));
    // Reverts to the NEW value (green), not the stale mount value (red).
    expect(onChange).toHaveBeenLastCalledWith({ r: 0, g: 255, b: 0 });
  });
});
