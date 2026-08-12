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

  // #614: the scrub must emit through the LATEST onChange, not the one captured
  // at mousedown. A consumer (CurveEditorPanel) recreates its onChange whenever
  // the committed key state changes; a scrub that kept firing the mousedown
  // closure would carry a stale reference (frozen oldTime) and diverge.
  it("a scrub emits through the latest onChange prop, not the one captured at mousedown (#614)", () => {
    const first = vi.fn();
    const second = vi.fn();
    const { rerender } = render(
      <Spinner value={5} onChange={first} step={1} aria-label="test-spinner" />
    );
    const column = screen.getByLabelText("Increment").parentElement as HTMLElement;
    fireEvent.mouseDown(column, { clientY: 100, button: 0 });
    // Consumer re-renders with a fresh onChange mid-gesture (same instance).
    rerender(<Spinner value={5} onChange={second} step={1} aria-label="test-spinner" />);
    fireEvent.mouseMove(document, { clientY: 80 }); // dy=+20 at step 1 → 25
    fireEvent.mouseUp(document);
    expect(second).toHaveBeenLastCalledWith(25); // latest handler
    expect(first).not.toHaveBeenCalled();        // NOT the mousedown closure
  });

  // The arrow column is inset + clipped so its hover/active background
  // can't paint over the input's rounded border (the "outline looks
  // broken on press" bug). Guards the containment classes from being
  // silently reverted.
  it("arrow column is clipped + rounded so its background stays inside the box outline", () => {
    render(<Spinner value={5} onChange={vi.fn()} step={1} aria-label="test-spinner" />);
    const column = screen.getByLabelText("Increment").parentElement as HTMLElement;
    expect(column.className).toContain("overflow-hidden");
    expect(column.className).toContain("rounded-r-[3px]");
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

  // The wheel honors the field's actual step magnitude, not a flat
  // 0.1/1. Legacy wheel stepped by the spinner's Increment (Spinner.cpp:107).
  it("scroll-wheel steps by the field's step magnitude", () => {
    const onChange = vi.fn();
    render(<Spinner value={5} onChange={onChange} step={5} aria-label="s" />);
    fireEvent.wheel(screen.getByRole("textbox"), { deltaY: -100 });
    expect(onChange).toHaveBeenCalledWith(10);
  });

  // Wheel Ctrl = fine (×0.1) on a decimal field (Spinner.cpp:109);
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

  // Drag Shift = coarse (×10) and Ctrl = fine, matching the wheel and
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

  // Reference-object "slides to origin on panel close" bug: the shared Spinner
  // froze its displayed text whenever the input was focused, even if the user
  // hadn't typed. So an external `value` change that arrived while focused (a
  // gizmo drag moving the reference object) updated the prop but not the visible
  // text, and the blur fired on closing the panel committed the STALE text —
  // clobbering the live value back to its pre-focus number. The field must
  // instead (a) track external changes while focused-but-untouched and (b) only
  // commit on blur when the user actually edited.
  it("focused-but-untouched field tracks an external value change (not stale)", () => {
    const onChange = vi.fn();
    const { rerender } = render(
      <Spinner value={0} onChange={onChange} decimals={1} aria-label="s" />
    );
    const input = screen.getByRole("textbox") as HTMLInputElement;
    fireEvent.focus(input); // user clicks in but types nothing
    rerender(<Spinner value={-130} onChange={onChange} decimals={1} aria-label="s" />); // gizmo moves it
    expect(input.value).toBe("-130.0"); // tracks, not stale "0.0"
  });

  it("blur with no keystrokes does not commit (no clobber of an external change)", () => {
    const onChange = vi.fn();
    const { rerender } = render(
      <Spinner value={0} onChange={onChange} decimals={1} aria-label="s" />
    );
    const input = screen.getByRole("textbox") as HTMLInputElement;
    fireEvent.focus(input);
    rerender(<Spinner value={-130} onChange={onChange} decimals={1} aria-label="s" />);
    onChange.mockClear();
    fireEvent.blur(input); // panel/popover closes -> blur, but user never typed
    expect(onChange).not.toHaveBeenCalled();
  });

  // Optimistic-echo gap: when the field emits a value, a bridge-backed caller's
  // `value` prop lags before it echoes back. The field must HOLD the optimistic
  // text instead of flashing back to the stale prop for a frame (the
  // reference-object spinner flicker), then reconcile to the echo — or to a
  // genuinely different external change.
  it("holds the optimistic value across the async echo gap, then reconciles", () => {
    const onChange = vi.fn();
    const { rerender } = render(
      <Spinner value={5} onChange={onChange} step={1} decimals={0} aria-label="s" />
    );
    const input = screen.getByRole("textbox") as HTMLInputElement;
    const incr = screen.getByLabelText("Increment");
    // arrow-click → emits 6, but the controlled prop hasn't echoed yet (still 5)
    fireEvent.mouseDown(incr, { clientY: 100, button: 0 });
    fireEvent.mouseUp(incr);
    fireEvent.click(incr);
    expect(onChange).toHaveBeenLastCalledWith(6);
    rerender(<Spinner value={5} onChange={onChange} step={1} decimals={0} aria-label="s" />);
    expect(input.value).toBe("6"); // optimistic held, NOT flashed back to "5"
    rerender(<Spinner value={6} onChange={onChange} step={1} decimals={0} aria-label="s" />);
    expect(input.value).toBe("6"); // echo arrived
    rerender(<Spinner value={-130} onChange={onChange} step={1} decimals={0} aria-label="s" />);
    expect(input.value).toBe("-130"); // a real external change reconciles
  });

  // The optimistic guard must reconcile on echo even when the user TYPED before
  // stepping (so the focused+edited skip would otherwise run first): the guard
  // must not outlive the echo and later swallow an external change back to the
  // pre-step baseline.
  it("optimistic guard does not get stuck when the user typed before stepping", () => {
    const onChange = vi.fn();
    const { rerender } = render(
      <Spinner value={5} onChange={onChange} step={1} decimals={0} aria-label="s" />
    );
    const input = screen.getByRole("textbox") as HTMLInputElement;
    fireEvent.focus(input);
    fireEvent.change(input, { target: { value: "5" } }); // a keystroke → edited=true
    fireEvent.keyDown(input, { key: "ArrowUp" }); // steps to 6; pendingBase armed at 5
    expect(onChange).toHaveBeenLastCalledWith(6);
    rerender(<Spinner value={6} onChange={onChange} step={1} decimals={0} aria-label="s" />); // echo
    fireEvent.blur(input); // edited cleared; "6" === 6 so no commit
    rerender(<Spinner value={5} onChange={onChange} step={1} decimals={0} aria-label="s" />); // external change back to baseline
    expect(input.value).toBe("5"); // reconciles, not stuck at "6"
  });

  // Holding an arrow button auto-repeats the step (legacy
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
