// Covers readout visibility and measured placement; native gizmo rendering is outside this suite.
import { describe, it, expect, vi } from "vitest";
import { render, screen } from "@testing-library/react";
import { makeBridgeStub, type BridgeStub } from "@/test/bridge-stub";
import { ManipulatorReadout } from "../ManipulatorReadout";

const READOUT_WIDTH_PX = 120;
const READOUT_HEIGHT_PX = 24;

function fakeBridge(): { bridge: BridgeStub; emit: BridgeStub["emit"] } {
  const bridge: BridgeStub = makeBridgeStub({
    fallback: { referenceObjectName: "Cantina", referenceObjectLocked: false },
  });
  return { bridge, emit: bridge.emit };
}
function overlay() {
  const el = document.createElement("div");
  el.getBoundingClientRect = () =>
    ({ width: 800, height: 600, x: 0, y: 0, top: 0, left: 0, right: 800, bottom: 600, toJSON() {} }) as DOMRect;
  return { current: el } as React.RefObject<HTMLElement>;
}

describe("ManipulatorReadout", () => {
  it("clamps the first drag update using the mounted pill's dimensions", (): void => {
    vi.spyOn(HTMLElement.prototype, "offsetWidth", "get").mockReturnValue(READOUT_WIDTH_PX);
    vi.spyOn(HTMLElement.prototype, "offsetHeight", "get").mockReturnValue(READOUT_HEIGHT_PX);
    try {
      const { bridge, emit }: ReturnType<typeof fakeBridge> = fakeBridge();
      render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
      emit("engine/manipulator/drag", {
        active: true,
        kind: "translate",
        nx: 1,
        ny: 1,
        visible: true,
        labels: ["X"],
        values: [1],
        decimals: 1,
      });
      expect(screen.getByText(/X 1\.0\s+units\b/)).toHaveStyle({ transform: "translate(676px, 528px)" });
    } finally {
      vi.restoreAllMocks();
    }
  });

  it("renders a translate value and hides on active:false", () => {
    const { bridge, emit } = fakeBridge();
    render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
    emit("engine/manipulator/drag", {
      active: true,
      kind: "translate",
      nx: 0.5,
      ny: 0.5,
      visible: true,
      labels: ["X"],
      values: [42.5],
      decimals: 1,
    });
    // translate is a distance -> trailing "units" (anchored so a
    // value/unit run-together would fail), and never the rotate-only ° suffix.
    expect(screen.getByText(/X 42\.5\s+units\b/)).toBeInTheDocument();
    expect(screen.queryByText(/°/)).not.toBeInTheDocument(); // the ° suffix is rotate-only
    emit("engine/manipulator/drag", { active: false });
    expect(screen.queryByText(/X 42\.5/)).not.toBeInTheDocument();
  });

  it("renders plane (two values) and rotate (degrees), with U+2212 for negatives", () => {
    const { bridge, emit } = fakeBridge();
    render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
    emit("engine/manipulator/drag", {
      active: true,
      kind: "plane",
      nx: 0.5,
      ny: 0.5,
      visible: true,
      labels: ["X", "Y"],
      values: [5, -3.2],
      decimals: 1,
    });
    expect(screen.getByText(/X 5\.0\s+Y −3\.2\s+units\b/)).toBeInTheDocument(); // plane is distance
    emit("engine/manipulator/drag", {
      active: true,
      kind: "rotate",
      nx: 0.5,
      ny: 0.5,
      visible: true,
      labels: ["Z"],
      values: [45],
      decimals: 0,
    });
    expect(screen.getByText(/Z 45°/)).toBeInTheDocument();
    expect(screen.queryByText(/\bunits\b/)).not.toBeInTheDocument(); // rotate uses ° not units
  });

  it("hides when visible:false (behind camera)", () => {
    const { bridge, emit } = fakeBridge();
    render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
    emit("engine/manipulator/drag", {
      active: true,
      kind: "translate",
      nx: 0.5,
      ny: 0.5,
      visible: false,
      labels: ["X"],
      values: [1],
      decimals: 1,
    });
    expect(screen.queryByText(/X/)).not.toBeInTheDocument();
  });

  it("hides when the reference object is cleared mid-drag (name → '')", () => {
    const { bridge, emit } = fakeBridge();
    render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
    emit("engine/manipulator/drag", {
      active: true,
      kind: "translate",
      nx: 0.5,
      ny: 0.5,
      visible: true,
      labels: ["X"],
      values: [9],
      decimals: 1,
    });
    expect(screen.getByText(/X 9\.0/)).toBeInTheDocument();
    emit("engine/state/changed", { referenceObjectName: "", referenceObjectLocked: false });
    expect(screen.queryByText(/X 9\.0/)).not.toBeInTheDocument();
  });

  it("hides when the reference object is locked mid-drag", () => {
    const { bridge, emit } = fakeBridge();
    render(<ManipulatorReadout bridge={bridge} overlayRef={overlay()} />);
    emit("engine/manipulator/drag", {
      active: true,
      kind: "translate",
      nx: 0.5,
      ny: 0.5,
      visible: true,
      labels: ["X"],
      values: [9],
      decimals: 1,
    });
    expect(screen.getByText(/X 9\.0/)).toBeInTheDocument();
    emit("engine/state/changed", { referenceObjectName: "Cantina", referenceObjectLocked: true });
    expect(screen.queryByText(/X 9\.0/)).not.toBeInTheDocument();
  });
});
