import { describe, it, expect } from "vitest";
import { render, screen } from "@testing-library/react";
import { useStackReorder } from "../use-stack-reorder";

// Minimal consumer of the hook: a flip-keyed <ul> + the portaled chip. Drives the
// --record `pose` path. jsdom has no layout (getBoundingClientRect → 0), but the pose
// effect still resolves a gap + chip from the row count and renders the chip node —
// which is exactly the deterministic "drag visual without events" the clip stills need.
function Harness({ pose }: { pose: { from: number; gap: number } | null }) {
  const order = ["alpha", "bravo", "charlie"];
  const api = useStackReorder({
    order,
    labelFor: (p) => p,
    onReorder: () => {},
    getScrollContainer: () => null,
    pose,
  });
  return (
    <>
      <ul ref={api.listRef}>
        {order.map((k) => (
          <li key={k} data-flip-key={k}>
            {k}
          </li>
        ))}
      </ul>
      {api.chipNode}
    </>
  );
}

describe("useStackReorder pose", () => {
  it("renders no drag chip without a pose", () => {
    render(<Harness pose={null} />);
    expect(screen.queryByTestId("stack-drag-chip")).toBeNull();
  });

  it("renders a frozen lifted chip for the posed row", () => {
    render(<Harness pose={{ from: 0, gap: 2 }} />);
    const chip = screen.getByTestId("stack-drag-chip");
    expect(chip).toBeTruthy();
    expect(chip.textContent).toContain("alpha"); // labelFor(order[0])
  });

  it("ignores an out-of-range posed row", () => {
    render(<Harness pose={{ from: 9, gap: 0 }} />);
    expect(screen.queryByTestId("stack-drag-chip")).toBeNull();
  });
});
