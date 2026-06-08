// Vitest unit tests for the Spinner primitive.
// Exercises: blur-clamping, scroll-wheel increment, scientific notation parse.

import { describe, it, expect, vi } from "vitest";
import { render, screen, fireEvent } from "@testing-library/react";
import userEvent from "@testing-library/user-event";
import { Spinner } from "../Spinner";

describe("Spinner", () => {
  it("commit-on-blur clamps value to max", async () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} max={10} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    // Focus, type a value above max, blur to commit.
    await userEvent.click(input);
    await userEvent.clear(input);
    await userEvent.type(input, "999");
    fireEvent.blur(input);
    // onChange should be called with the clamped value.
    expect(onChange).toHaveBeenCalledWith(10);
  });

  it("scroll-wheel up increments by step", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={1} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    // Wheel deltaY < 0 = scroll up = increment.
    fireEvent.wheel(input, { deltaY: -100 });
    expect(onChange).toHaveBeenCalledWith(6);
  });

  it("scientific notation '2.5e3' parses to 2500 on blur", async () => {
    const onChange = vi.fn();
    render(
      <Spinner value={0} onChange={onChange} decimals={0} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    await userEvent.click(input);
    await userEvent.clear(input);
    await userEvent.type(input, "2.5e3");
    fireEvent.blur(input);
    expect(onChange).toHaveBeenCalledWith(2500);
  });

  // F7: wheel steps a flat 0.1 on decimal fields (regardless of `step`).
  it("scroll-wheel steps 0.1 on a decimal field", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={0.1} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    fireEvent.wheel(input, { deltaY: -100 });
    expect(onChange).toHaveBeenCalledWith(5.1);
  });

  // F7: Shift coarsens the wheel step by ×10 (0.1 → 1 on a decimal field).
  it("scroll-wheel with Shift steps ×10", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={0.1} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    fireEvent.wheel(input, { deltaY: -100, shiftKey: true });
    expect(onChange).toHaveBeenCalledWith(6);
  });

  // F6: dragging the text INPUT must NOT scrub the value (it selects text).
  it("dragging the text input does not change the value", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={1} aria-label="test-spinner" />
    );
    const input = screen.getByRole("textbox");
    fireEvent.mouseDown(input, { clientY: 100, button: 0 });
    fireEvent.mouseMove(document, { clientY: 60 });
    fireEvent.mouseUp(document);
    expect(onChange).not.toHaveBeenCalled();
  });

  // F6: dragging the ARROW COLUMN vertically scrubs the value.
  it("dragging the arrow column scrubs the value", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={1} aria-label="test-spinner" />
    );
    const column = screen.getByLabelText("Increment").parentElement as HTMLElement;
    // Drag up 20px (dy = +20) at step 1 → 5 + 20 = 25.
    fireEvent.mouseDown(column, { clientY: 100, button: 0 });
    fireEvent.mouseMove(document, { clientY: 80 });
    fireEvent.mouseUp(document);
    expect(onChange).toHaveBeenLastCalledWith(25);
  });

  // F6: a plain click on an arrow still steps by ±step (no drag).
  it("clicking the increment arrow steps by step", () => {
    const onChange = vi.fn();
    render(
      <Spinner value={5} onChange={onChange} step={1} aria-label="test-spinner" />
    );
    const incr = screen.getByLabelText("Increment");
    fireEvent.mouseDown(incr, { clientY: 100, button: 0 });
    fireEvent.mouseUp(incr);
    fireEvent.click(incr);
    expect(onChange).toHaveBeenLastCalledWith(6);
  });

  // an-audit-finding: the wheel honors the field's actual step magnitude, not a flat
  // 0.1/1. Legacy wheel stepped by the spinner's Increment (Spinner.cpp:107).
  it("scroll-wheel steps by the field's step magnitude", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={5} aria-label="s" />);
    fireEvent.wheel(screen.getByRole("textbox"), { deltaY: -100 });
    expect(onChange).toHaveBeenCalledWith(10);
  });

  // an-audit-finding: wheel Ctrl = fine (×0.1) on a decimal field (Spinner.cpp:109);
  // ignored on whole-number fields so it never produces fractions.
  it("scroll-wheel with Ctrl steps fine on a decimal field", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={0.1} aria-label="s" />);
    fireEvent.wheel(screen.getByRole("textbox"), { deltaY: -100, ctrlKey: true });
    expect(onChange).toHaveBeenCalledWith(5.01);
  });
  it("scroll-wheel with Ctrl stays whole on an integer field", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={1} aria-label="s" />);
    fireEvent.wheel(screen.getByRole("textbox"), { deltaY: -100, ctrlKey: true });
    expect(onChange).toHaveBeenCalledWith(6);
  });

  // an-audit-finding: drag Shift = coarse (×10) and Ctrl = fine, matching the wheel and
  // keyboard arrows (the old drag had these inverted).
  it("drag with Shift scrubs coarse (×10)", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={1} aria-label="s" />);
    const column = screen.getByLabelText("Increment").parentElement as HTMLElement;
    fireEvent.mouseDown(column, { clientY: 100, button: 0 });
    fireEvent.mouseMove(document, { clientY: 80, shiftKey: true }); // dy=20 → ×10
    fireEvent.mouseUp(document);
    expect(onChange).toHaveBeenLastCalledWith(205);
  });
  it("drag with Ctrl scrubs fine on a decimal field", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={0.1} aria-label="s" />);
    const column = screen.getByLabelText("Increment").parentElement as HTMLElement;
    fireEvent.mouseDown(column, { clientY: 100, button: 0 });
    fireEvent.mouseMove(document, { clientY: 80, ctrlKey: true }); // dy=20 → ×0.01
    fireEvent.mouseUp(document);
    expect(onChange).toHaveBeenLastCalledWith(5.2);
  });

  // an-audit-finding: holding an arrow button auto-repeats the step (legacy
  // hold-to-repeat, Spinner.cpp:438-455).
  it("holding the increment arrow auto-repeats", () => {
    vi.useFakeTimers();
    try {
      const onChange = vi.fn();
      render(<Spinner value={5} onChange={onChange} step={1} aria-label="s" />);
      const incr = screen.getByLabelText("Increment");
      fireEvent.mouseDown(incr, { clientY: 100, button: 0 });
      vi.advanceTimersByTime(350 + 50 * 3); // past initial delay + 3 repeats
      fireEvent.mouseUp(incr);
      expect(onChange.mock.calls.length).toBeGreaterThanOrEqual(3);
      expect(onChange).toHaveBeenLastCalledWith(8); // 5 → 6, 7, 8
    } finally {
      vi.useRealTimers();
    }
  });
});
