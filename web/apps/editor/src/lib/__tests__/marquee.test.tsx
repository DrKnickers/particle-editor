import { type ReactElement } from "react";
import { render, screen, fireEvent, waitFor } from "@testing-library/react";
import * as Tooltip from "@radix-ui/react-tooltip";
import { describe, it, expect, beforeEach } from "vitest";
import {
  rectFromPoints,
  rectsIntersect,
  emittersInMarquee,
  mergeMarqueeSelection,
  type Rect,
} from "../marquee";
import { EmitterTree } from "../../screens/EmitterTree";
import { useEmitterSelectionStore } from "../emitter-selection";
import { makeStubBridge, marqueeFlatRootsTree, stubRect } from "../../screens/__tests__/emitter-tree-fixtures";

const renderWithTooltips = (ui: ReactElement) =>
  render(
    <Tooltip.Provider delayDuration={0} skipDelayDuration={0}>
      {ui}
    </Tooltip.Provider>,
  );

beforeEach(() => {
  useEmitterSelectionStore.getState().clear();
});

describe("marquee geometry", () => {
  it("rectFromPoints normalises any corner order", () => {
    expect(rectFromPoints(10, 20, 4, 6)).toEqual({ left: 4, top: 6, right: 10, bottom: 20 });
    expect(rectFromPoints(4, 6, 10, 20)).toEqual({ left: 4, top: 6, right: 10, bottom: 20 });
  });

  it("rectsIntersect detects overlap and gaps (edge-inclusive)", () => {
    const a: Rect = { left: 0, top: 0, right: 10, bottom: 10 };
    expect(rectsIntersect(a, { left: 5, top: 5, right: 15, bottom: 15 })).toBe(true); // overlap
    expect(rectsIntersect(a, { left: 10, top: 10, right: 20, bottom: 20 })).toBe(true); // touching edge
    expect(rectsIntersect(a, { left: 11, top: 0, right: 20, bottom: 10 })).toBe(false); // gap on x
    expect(rectsIntersect(a, { left: 0, top: 11, right: 10, bottom: 20 })).toBe(false); // gap on y
  });

  it("emittersInMarquee returns intersecting ids in row order", () => {
    // Three stacked rows 0..30, 30..60, 60..90 (x 0..100).
    const rows = [
      { id: 0, rect: { left: 0, top: 0, right: 100, bottom: 30 } },
      { id: 1, rect: { left: 0, top: 30, right: 100, bottom: 60 } },
      { id: 2, rect: { left: 0, top: 60, right: 100, bottom: 90 } },
    ];
    // A marquee covering rows 1 and 2 (top 40 → 80).
    expect(emittersInMarquee(rows, { left: 5, top: 40, right: 50, bottom: 80 })).toEqual([1, 2]);
    // A marquee in empty x-space hits nothing.
    expect(emittersInMarquee(rows, { left: 200, top: 0, right: 300, bottom: 90 })).toEqual([]);
  });

  it("mergeMarqueeSelection (non-additive) = swept, primary = last swept", () => {
    expect(mergeMarqueeSelection([], [1, 2])).toEqual({ ids: [1, 2], primary: 2 });
  });

  it("mergeMarqueeSelection (additive) unions base then swept, no dupes", () => {
    expect(mergeMarqueeSelection([5], [1, 5, 2])).toEqual({ ids: [5, 1, 2], primary: 2 });
  });

  it("mergeMarqueeSelection with nothing swept keeps base, primary = last base", () => {
    expect(mergeMarqueeSelection([5, 7], [])).toEqual({ ids: [5, 7], primary: 7 });
    expect(mergeMarqueeSelection([], [])).toEqual({ ids: [], primary: null });
  });

  it("marquee snapshots row rects once for many pointer moves", async () => {
    const bridge = makeStubBridge(marqueeFlatRootsTree());
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    const scroll = document.querySelector(".emitter-tree-scroll") as HTMLElement;
    const smoke = screen.getByText("Smoke").closest("[data-emitter-id]") as HTMLElement;
    const sparks = screen.getByText("Sparks").closest("[data-emitter-id]") as HTMLElement;
    const flash = screen.getByText("Flash").closest("[data-emitter-id]") as HTMLElement;
    const scrollRect = stubRect(scroll, { left: 0, top: 0, right: 200, bottom: 120 });
    const rowRects = [
      stubRect(smoke, { left: 0, top: 0, right: 180, bottom: 24 }),
      stubRect(sparks, { left: 0, top: 24, right: 180, bottom: 48 }),
      stubRect(flash, { left: 0, top: 48, right: 180, bottom: 72 }),
    ];

    fireEvent.pointerDown(scroll, { button: 0, clientX: 190, clientY: 4 });
    for (let i = 0; i < 12; i += 1) {
      fireEvent.pointerMove(document, { button: 0, clientX: 4, clientY: 60 + i });
    }

    expect(screen.getByTestId("emitter-row:0")).toHaveAttribute("data-selected", "true");
    expect(screen.getByTestId("emitter-row:1")).toHaveAttribute("data-selected", "true");
    expect(screen.getByTestId("emitter-row:2")).toHaveAttribute("data-selected", "true");
    expect(screen.getByTestId("emitter-tree")).toHaveAttribute("data-selected-count", "3");
    expect(rowRects.reduce((sum, spy) => sum + spy.mock.calls.length, 0)).toBeLessThanOrEqual(3);
    expect(scrollRect.mock.calls.length).toBeGreaterThan(0);

    fireEvent.pointerUp(document, { button: 0, clientX: 4, clientY: 72 });
  });

  it("additive marquee unions swept rows with the prior component selection", async () => {
    const bridge = makeStubBridge(marqueeFlatRootsTree());
    renderWithTooltips(<EmitterTree bridge={bridge} />);
    await waitFor(() => expect(screen.getByText("Smoke")).toBeInTheDocument());

    // Flash is the pre-existing base selection. The Ctrl-drag sweeps Smoke
    // only, so the component contract is the union { Flash, Smoke }.
    fireEvent.click(screen.getByText("Flash"));

    const scroll = document.querySelector(".emitter-tree-scroll") as HTMLElement;
    const smoke = screen.getByText("Smoke").closest("[data-emitter-id]") as HTMLElement;
    const sparks = screen.getByText("Sparks").closest("[data-emitter-id]") as HTMLElement;
    const flash = screen.getByText("Flash").closest("[data-emitter-id]") as HTMLElement;
    stubRect(scroll, { left: 0, top: 0, right: 200, bottom: 120 });
    stubRect(smoke, { left: 0, top: 0, right: 180, bottom: 24 });
    stubRect(sparks, { left: 0, top: 24, right: 180, bottom: 48 });
    stubRect(flash, { left: 0, top: 48, right: 180, bottom: 72 });

    fireEvent.pointerDown(scroll, { button: 0, ctrlKey: true, clientX: 190, clientY: 4 });
    fireEvent.pointerMove(document, { button: 0, ctrlKey: true, clientX: 4, clientY: 23 });

    expect(screen.getByTestId("emitter-row:0")).toHaveAttribute("data-selected", "true");
    expect(screen.getByTestId("emitter-row:1")).toHaveAttribute("data-selected", "false");
    expect(screen.getByTestId("emitter-row:2")).toHaveAttribute("data-selected", "true");
    expect(screen.getByTestId("emitter-tree")).toHaveAttribute("data-selected-count", "2");

    fireEvent.pointerUp(document, { button: 0, ctrlKey: true, clientX: 4, clientY: 23 });
  });
});
