import { render } from "@testing-library/react";
import { describe, it, expect } from "vitest";
import { RecordCursor } from "../RecordCursor";

describe("RecordCursor", () => {
  it("renders nothing until visible", () => {
    const { container, rerender } = render(<RecordCursor x={100} y={50} visible={false} pressed={false} />);
    expect(container.querySelector('[data-testid="record-cursor"]')).toBeNull();
    rerender(<RecordCursor x={100} y={50} visible={true} pressed={false} />);
    expect(container.querySelector('[data-testid="record-cursor"]')).not.toBeNull();
  });

  it("positions in CSS px (device px / devicePixelRatio)", () => {
    const prev = window.devicePixelRatio;
    Object.defineProperty(window, "devicePixelRatio", { value: 2, configurable: true });
    const { container } = render(<RecordCursor x={200} y={100} visible={true} pressed={false} />);
    const el = container.querySelector('[data-testid="record-cursor"]') as HTMLElement;
    expect(el.style.left).toBe("100px"); // 200 / 2
    expect(el.style.top).toBe("50px"); // 100 / 2
    Object.defineProperty(window, "devicePixelRatio", { value: prev, configurable: true });
  });

  it("marks pressed state", () => {
    const { container } = render(<RecordCursor x={0} y={0} visible={true} pressed={true} />);
    const el = container.querySelector('[data-testid="record-cursor"]') as HTMLElement;
    const sprite = container.querySelector('[data-testid="record-cursor-sprite"]') as SVGElement;
    expect(el.getAttribute("data-pressed")).toBe("true");
    expect(container.querySelector("span")).toBeNull();
    expect(sprite.style.transform).toBe("scale(0.82)");
  });
});
