// CRV gutter-marquee: the axis-label wrapper routes a primary pointerdown
// that lands OUTSIDE the plot SVG (i.e. in a label gutter) to
// onGutterPointerDown, so the curve marquee can start from the margins. A
// pointerdown inside the SVG belongs to the plot's own handlers and must not.

import { describe, it, expect, vi } from "vitest";
import { fireEvent, render } from "@testing-library/react";
import { CanvasWithAxisLabels } from "../CurveEditorPanel";

function renderWrapper(onGutterPointerDown: ReturnType<typeof vi.fn>) {
  return render(
    <CanvasWithAxisLabels yMin={0} yMax={1} onGutterPointerDown={onGutterPointerDown}>
      <svg data-testid="curve-editor-svg">
        <rect data-testid="plot-inside" width={10} height={10} />
      </svg>
    </CanvasWithAxisLabels>,
  );
}

describe("CanvasWithAxisLabels — gutter pointerdown routing (CRV)", () => {
  it("a primary pointerdown outside the plot SVG calls onGutterPointerDown", () => {
    const onGutter = vi.fn();
    const { container } = renderWrapper(onGutter);
    const grid = container.querySelector("[data-testid='curve-canvas-with-axes']")!;
    fireEvent.pointerDown(grid, { button: 0, clientX: 5, clientY: 5 });
    expect(onGutter).toHaveBeenCalledTimes(1);
  });

  it("a pointerdown inside the plot SVG does NOT call onGutterPointerDown", () => {
    const onGutter = vi.fn();
    const { getByTestId } = renderWrapper(onGutter);
    fireEvent.pointerDown(getByTestId("plot-inside"), { button: 0, clientX: 5, clientY: 5 });
    expect(onGutter).not.toHaveBeenCalled();
  });

  it("a non-primary (right) button pointerdown in a gutter does NOT start a marquee", () => {
    const onGutter = vi.fn();
    const { container } = renderWrapper(onGutter);
    const grid = container.querySelector("[data-testid='curve-canvas-with-axes']")!;
    fireEvent.pointerDown(grid, { button: 2, clientX: 5, clientY: 5 });
    expect(onGutter).not.toHaveBeenCalled();
  });
});
