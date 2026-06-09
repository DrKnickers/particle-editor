// Vitest tests for CurveEditor (Phase 3 Screen 6 Batch A foundation +
// Screen 5 / Screen 6 Batch B-α interaction).
//
// Covered:
//   - Renders a <polyline> + one <circle> per key for an N-key linear track.
//   - Empty-key track suppresses the polyline.
//   - Grid + axes render the expected ≥22 lines.
//   - Clicking a key fires onKeyClick; selected-key styling applies
//     (fill + radius).
//   - Ctrl/Cmd+click toggles selection without losing the prior one
//     (verified by passing in a multi-selection set).
//   - Smooth interpolation renders a <path> with cubic-Bezier (C)
//     commands; step interpolation renders the staircase polyline.

import type { InterpolationType, TrackDto } from "@particle-editor/bridge-schema";
import type * as React from "react";
import { createRef } from "react";
import { describe, it, expect, vi } from "vitest";
import { act, fireEvent, render } from "@testing-library/react";
import { CurveEditor, type ChannelDef, type CurveMarqueeHandle } from "../CurveEditor";

function fixtureTrack(
  keyCount: number,
  interpolation: InterpolationType = "linear",
): TrackDto {
  const keys = Array.from({ length: keyCount }, (_, i) => ({
    time: (i / Math.max(1, keyCount - 1)) * 100,
    value: i % 2 === 0 ? 0.1 : 0.9,
  }));
  return { name: "red", keys, interpolation, lockedTo: null };
}

describe("CurveEditor", () => {
  it("renders a <polyline> and N <circle> elements for an N-key track", () => {
    const { container } = render(
      <CurveEditor track={fixtureTrack(5)} valueRange={{ min: 0, max: 1 }} />,
    );
    const polyline = container.querySelector("polyline");
    expect(polyline).not.toBeNull();
    // polyline points string should have 5 comma-separated pairs.
    const pts = polyline!.getAttribute("points") ?? "";
    expect(pts.trim().split(/\s+/)).toHaveLength(5);

    const circles = container.querySelectorAll("circle");
    expect(circles).toHaveLength(5);
  });

  it("suppresses the <polyline> when the track has fewer than 2 keys", () => {
    const { container } = render(
      <CurveEditor track={fixtureTrack(1)} valueRange={{ min: 0, max: 1 }} />,
    );
    expect(container.querySelector("polyline")).toBeNull();
    // A single-key track still shows its one circle.
    expect(container.querySelectorAll("circle")).toHaveLength(1);
  });

  it("renders the grid (≥10 vertical + ≥10 horizontal lines) + axes", () => {
    const { container } = render(
      <CurveEditor track={fixtureTrack(3)} valueRange={{ min: 0, max: 1 }} />,
    );
    // Grid + axes use <line> elements. 11 vertical + 11 horizontal +
    // 2 axes = 24.
    const lines = container.querySelectorAll("line");
    expect(lines.length).toBeGreaterThanOrEqual(22);
  });

  // ─── Screen 5 / Screen 6 Batch B-α ────────────────────────────────

  it("clicking a key fires onKeyClick + the selected key renders with accent fill + r=5", () => {
    const track = fixtureTrack(3); // times 0, 50, 100
    const onKeyClick = vi.fn();
    const { container, rerender } = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        onKeyClick={onKeyClick}
      />,
    );
    const circles = container.querySelectorAll("[data-testid='curve-key']");
    expect(circles).toHaveLength(3);
    // Click the middle key (time=50).
    fireEvent.click(circles[1]!);
    expect(onKeyClick).toHaveBeenCalledTimes(1);
    expect(onKeyClick.mock.calls[0]![0]).toBe(50);

    // Re-render with the click's time in the selected set; the matching
    // circle enlarges (r 4→6) and is tagged data-selected. Its fill stays
    // the key's OWN colour (interior grey) — selection styling is the
    // saturate()+shadow CSS on [data-selected="true"], not a blue fill.
    rerender(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        selectedKeyTimes={new Set([50])}
        onKeyClick={onKeyClick}
      />,
    );
    const middle = container.querySelectorAll("[data-testid='curve-key']")[1]!;
    expect(middle.getAttribute("data-selected")).toBe("true");
    expect(middle.getAttribute("r")).toBe("6");
    expect(middle.getAttribute("fill")).toBe("#e5e5e5");
    // Sanity: the unselected siblings stay at r=4.
    const first = container.querySelectorAll("[data-testid='curve-key']")[0]!;
    expect(first.getAttribute("r")).toBe("4");
    expect(first.getAttribute("data-selected")).toBe("false");
  });

  it("Ctrl+click on a second key adds it to the selection (multi-selection styling)", () => {
    const track = fixtureTrack(3);
    const onKeyClick = vi.fn();
    // Start with the first key selected (time=0). Render then simulate
    // the second key being clicked with the ctrlKey modifier.
    const { container, rerender } = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        selectedKeyTimes={new Set([0])}
        onKeyClick={onKeyClick}
      />,
    );
    const circles = container.querySelectorAll("[data-testid='curve-key']");
    fireEvent.click(circles[1]!, { ctrlKey: true });
    expect(onKeyClick).toHaveBeenCalledTimes(1);
    // The handler received the click with ctrlKey set on the event.
    const evt = onKeyClick.mock.calls[0]![1] as { ctrlKey: boolean };
    expect(evt.ctrlKey).toBe(true);

    // Now render with both keys in the selection set and assert both
    // circles paint as selected. (The parent owns the actual toggle
    // logic; CurveEditor is presentational from the selection-set's
    // point of view.)
    rerender(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        selectedKeyTimes={new Set([0, 50])}
        onKeyClick={onKeyClick}
      />,
    );
    const post = container.querySelectorAll("[data-testid='curve-key']");
    expect(post[0]!.getAttribute("data-selected")).toBe("true");
    expect(post[1]!.getAttribute("data-selected")).toBe("true");
    expect(post[2]!.getAttribute("data-selected")).toBe("false");
  });

  // ─── Screen 6 Batch B-β ────────────────────────────────────────────

  it("pointer-down + move + up on an interior key fires onKeyDragEnd with the new (time, value)", () => {
    const track = fixtureTrack(3);            // times 0, 50, 100; values 0.1, 0.9, 0.1
    const onDragEnd = vi.fn();
    const onKeyClick = vi.fn();
    const { container } = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        onKeyClick={onKeyClick}
        onKeyDragEnd={onDragEnd}
      />,
    );
    const svg = container.querySelector(
      "[data-testid='curve-editor-svg']",
    ) as SVGSVGElement;
    // jsdom returns 0×0 for getBoundingClientRect; stub it so the
    // viewBox <-> client-coord mapping has a valid scale.
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    const circles = container.querySelectorAll("[data-testid='curve-key']");
    const middle = circles[1]! as SVGCircleElement;
    // Pointer-down on the middle key (time=50, value=0.9).
    fireEvent.pointerDown(middle, { pointerId: 1, button: 0, clientX: 300, clientY: 30 });
    // Pointer-move on the SVG itself (the source's pointer is captured
    // by the SVG-level handler). New target client coords: (270, 60)
    // → x=270 → time=45; y=60 → height-y=240 → value=0.8.
    fireEvent.pointerMove(svg, { pointerId: 1, clientX: 270, clientY: 60 });
    // Pointer-up commits.
    fireEvent.pointerUp(svg, { pointerId: 1, clientX: 270, clientY: 60 });

    expect(onDragEnd).toHaveBeenCalledTimes(1);
    const [oldTime, newTime, newValue] = onDragEnd.mock.calls[0] as [number, number, number];
    expect(oldTime).toBe(50);
    // newTime should be clamped within (0, 100) exclusive; 45 falls in
    // range so it's preserved (or very close — pixel math).
    expect(newTime).toBeGreaterThan(0);
    expect(newTime).toBeLessThan(100);
    expect(newTime).toBeCloseTo(45, 1);
    // newValue should map to 0.8 (within float epsilon).
    expect(newValue).toBeCloseTo(0.8, 2);
    // The plain click handler is NOT invoked when a drag actually
    // moved.
    expect(onKeyClick).not.toHaveBeenCalled();
  });

  it("pointer-down then pointer-up on a key without movement fires onKeyClick (not onKeyDragEnd)", () => {
    const track = fixtureTrack(3);
    const onDragEnd = vi.fn();
    const onKeyClick = vi.fn();
    const { container } = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        onKeyClick={onKeyClick}
        onKeyDragEnd={onDragEnd}
      />,
    );
    const svg = container.querySelector("[data-testid='curve-editor-svg']") as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
    const middle = container.querySelectorAll("[data-testid='curve-key']")[1]!;
    // Pointer down + up at the same coords (no movement past slop).
    fireEvent.pointerDown(middle, { pointerId: 3, button: 0, clientX: 300, clientY: 30 });
    fireEvent.pointerUp(svg, { pointerId: 3, clientX: 300, clientY: 30 });
    expect(onDragEnd).not.toHaveBeenCalled();
    expect(onKeyClick).toHaveBeenCalledTimes(1);
    expect(onKeyClick.mock.calls[0]![0]).toBe(50);
  });

  it("border keys render with the accent stroke ring + darker fill", () => {
    const track = fixtureTrack(3);
    const { container } = render(
      <CurveEditor track={track} valueRange={{ min: 0, max: 1 }} />,
    );
    const circles = container.querySelectorAll("[data-testid='curve-key']");
    // First + last keys are border.
    const first = circles[0]!;
    const last = circles[2]!;
    const middle = circles[1]!;
    expect(first.getAttribute("data-border")).toBe("true");
    expect(last.getAttribute("data-border")).toBe("true");
    expect(middle.getAttribute("data-border")).toBe("false");
    // Stroke + stroke-width attributes confirm the visual.
    expect(first.getAttribute("stroke")).toBe("#0EA5E9");
    expect(first.getAttribute("stroke-width")).toBe("1.5");
    expect(first.getAttribute("fill")).toBe("#94A3B8");
    expect(last.getAttribute("stroke")).toBe("#0EA5E9");
    expect(last.getAttribute("stroke-width")).toBe("1.5");
    // Interior key has no outline stroke (drop-shadow via CSS) + lighter fill.
    expect(middle.getAttribute("stroke")).toBe("none");
    expect(middle.getAttribute("fill")).toBe("#e5e5e5");
  });

  it("pointer-down on empty canvas in Insert mode fires onCanvasAdd with the projected (time, value)", () => {
    const track = fixtureTrack(2);
    const onCanvasAdd = vi.fn();
    const { container } = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        insertMode
        onCanvasAdd={onCanvasAdd}
      />,
    );
    const svg = container.querySelector(
      "[data-testid='curve-editor-svg']",
    ) as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
    const backdrop = container.querySelector(
      "[data-testid='curve-canvas-backdrop']",
    ) as SVGRectElement;
    // Click the canvas at (180, 90) → x=180 → time=30; height-y=210 → value=0.7.
    fireEvent.pointerDown(backdrop, { pointerId: 2, button: 0, clientX: 180, clientY: 90 });
    expect(onCanvasAdd).toHaveBeenCalledTimes(1);
    const [time, value] = onCanvasAdd.mock.calls[0] as [number, number];
    expect(time).toBeCloseTo(30, 1);
    expect(value).toBeCloseTo(0.7, 2);
  });

  // ─── Phase 4.1 Fix dispatch 5: marquee select ─────────────────────

  /** Helper — render a CurveEditor with bounding-rect-stubbed SVG so
   *  the viewBox-coord math has a deterministic scale. Returns the
   *  container + SVG element + backdrop element. */
  function renderForMarquee(
    track: TrackDto,
    extras: Partial<React.ComponentProps<typeof CurveEditor>> = {},
  ) {
    const result = render(
      <CurveEditor
        track={track}
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        {...extras}
      />,
    );
    const svg = result.container.querySelector(
      "[data-testid='curve-editor-svg']",
    ) as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
    const backdrop = result.container.querySelector(
      "[data-testid='curve-canvas-backdrop']",
    ) as SVGRectElement;
    backdrop.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
    return { ...result, svg, backdrop };
  }

  it("Select-mode marquee drag selects every key whose (time, value) falls inside the rect (inclusive)", () => {
    // 5 keys at times 0, 25, 50, 75, 100; alternating values 0.1 / 0.9.
    // viewBox 600×300; with valueRange [0, 1]:
    //   time 0   → x=0    | time 100 → x=600
    //   value 0.1 → y=270 | value 0.9 → y=30
    // Drag a marquee from client (120, 0) → (480, 300) covers x∈[120,480]
    // which corresponds to times 25, 50, 75 (their x = 150, 300, 450)
    // and y∈[0, 300] which covers ALL values. Expected hits: {25, 50, 75}.
    const track = fixtureTrack(5);
    const onMarquee = vi.fn();
    const { svg, backdrop } = renderForMarquee(track, {
      onCanvasMarqueeSelect: onMarquee,
    });

    fireEvent.pointerDown(backdrop, {
      pointerId: 10, button: 0, clientX: 120, clientY: 0, shiftKey: false,
    });
    fireEvent.pointerMove(svg, { pointerId: 10, clientX: 480, clientY: 300 });
    fireEvent.pointerUp(svg, { pointerId: 10, clientX: 480, clientY: 300 });

    expect(onMarquee).toHaveBeenCalledTimes(1);
    const [times, shift] = onMarquee.mock.calls[0] as [number[], boolean];
    expect(shift).toBe(false);
    // Sort because hit-test order matches points array order, but
    // assertion clarity wins over implementation detail.
    expect([...times].sort((a, b) => a - b)).toEqual([25, 50, 75]);
  });

  it("Shift-held marquee passes shift: true to the callback (parent appends)", () => {
    const track = fixtureTrack(5);
    const onMarquee = vi.fn();
    const { svg, backdrop } = renderForMarquee(track, {
      onCanvasMarqueeSelect: onMarquee,
    });

    fireEvent.pointerDown(backdrop, {
      pointerId: 11, button: 0, clientX: 120, clientY: 0, shiftKey: true,
    });
    fireEvent.pointerMove(svg, { pointerId: 11, clientX: 480, clientY: 300 });
    fireEvent.pointerUp(svg, { pointerId: 11, clientX: 480, clientY: 300 });

    expect(onMarquee).toHaveBeenCalledTimes(1);
    const [, shift] = onMarquee.mock.calls[0] as [number[], boolean];
    expect(shift).toBe(true);
  });

  it("Esc during an active marquee cancels — callback not fired, rect removed", () => {
    const track = fixtureTrack(5);
    const onMarquee = vi.fn();
    const onClick = vi.fn();
    const { container, backdrop, svg } = renderForMarquee(track, {
      onCanvasMarqueeSelect: onMarquee,
      onCanvasClick: onClick,
    });

    fireEvent.pointerDown(backdrop, {
      pointerId: 12, button: 0, clientX: 120, clientY: 30, shiftKey: false,
    });
    // Drag past slop so the rect renders.
    fireEvent.pointerMove(svg, { pointerId: 12, clientX: 280, clientY: 200 });
    expect(container.querySelector("[data-testid='curve-marquee']")).not.toBeNull();
    // Esc cancels.
    fireEvent.keyDown(window, { key: "Escape" });
    expect(container.querySelector("[data-testid='curve-marquee']")).toBeNull();
    // A subsequent pointer-up should NOT fire either callback because
    // the marquee state was cleared. The pointer-up here lands without
    // any active marquee, so it's a no-op.
    fireEvent.pointerUp(svg, { pointerId: 12, clientX: 280, clientY: 200 });
    expect(onMarquee).not.toHaveBeenCalled();
    expect(onClick).not.toHaveBeenCalled();
  });

  it("Marquee is suppressed in Insert mode — pointer-down still routes to onCanvasAdd", () => {
    const track = fixtureTrack(3);
    const onMarquee = vi.fn();
    const onAdd = vi.fn();
    const { backdrop } = renderForMarquee(track, {
      insertMode: true,
      onCanvasAdd: onAdd,
      onCanvasMarqueeSelect: onMarquee,
    });
    fireEvent.pointerDown(backdrop, {
      pointerId: 13, button: 0, clientX: 180, clientY: 90,
    });
    // Insert mode: onCanvasAdd fires immediately; no marquee state
    // gets set, so a follow-up move/up is irrelevant.
    expect(onAdd).toHaveBeenCalledTimes(1);
    expect(onMarquee).not.toHaveBeenCalled();
  });

  it("Select-mode pointer-down/up without drag past slop fires onCanvasClick (preserves clear-selection)", () => {
    const track = fixtureTrack(3);
    const onMarquee = vi.fn();
    const onClick = vi.fn();
    const { svg, backdrop } = renderForMarquee(track, {
      onCanvasMarqueeSelect: onMarquee,
      onCanvasClick: onClick,
    });
    fireEvent.pointerDown(backdrop, {
      pointerId: 14, button: 0, clientX: 300, clientY: 150,
    });
    // Tiny micro-move within DRAG_SLOP (1.5 viewBox units) — still a
    // "click", not a "drag".
    fireEvent.pointerMove(svg, { pointerId: 14, clientX: 300.5, clientY: 150.5 });
    fireEvent.pointerUp(svg, { pointerId: 14, clientX: 300.5, clientY: 150.5 });
    expect(onMarquee).not.toHaveBeenCalled();
    expect(onClick).toHaveBeenCalledTimes(1);
  });

  it("smooth interpolation renders a <path> with cubic-Bezier (C) commands; step renders a staircase polyline", () => {
    // Smooth.
    const { container: smoothContainer } = render(
      <CurveEditor
        track={fixtureTrack(3, "smooth")}
        valueRange={{ min: 0, max: 1 }}
      />,
    );
    const path = smoothContainer.querySelector("[data-testid='curve-path']") as SVGPathElement | null;
    expect(path).not.toBeNull();
    const d = path!.getAttribute("d") ?? "";
    // 2 segments → 2 cubic-Bezier C commands.
    const cCount = (d.match(/C /g) ?? []).length;
    expect(cCount).toBe(2);
    // No straight-line linear polyline in the smooth branch.
    expect(
      smoothContainer.querySelector("polyline[data-interpolation='linear']"),
    ).toBeNull();

    // Step. The staircase polyline expands to 1 + 2*(N-1) points
    // (start key + 2 per segment).
    const { container: stepContainer } = render(
      <CurveEditor
        track={fixtureTrack(3, "step")}
        valueRange={{ min: 0, max: 1 }}
      />,
    );
    const stepPoly = stepContainer.querySelector(
      "polyline[data-interpolation='step']",
    ) as SVGPolylineElement | null;
    expect(stepPoly).not.toBeNull();
    const pts = (stepPoly!.getAttribute("points") ?? "").trim().split(/\s+/);
    // For N=3 keys → 5 points in the staircase.
    expect(pts).toHaveLength(5);
    // Sanity: the linear <path> isn't drawn here.
    expect(stepContainer.querySelector("[data-testid='curve-path']")).toBeNull();
  });
});

// ─── CRV: gutter-initiated marquee on the multi-channel editor ───────
//
// The panel renders the MULTI-CHANNEL editor (focusChannel set). Its
// marquee already tracks everywhere via pointer capture; these tests
// cover the NEW imperative `startMarquee` entry point that lets a
// marquee begin from the axis-label gutters, anchored at the plot edge.
// jsdom rejects the live measurement, so width/height fall back to the
// 600×300 props (deterministic).
const GUTTER_CHANNELS: ChannelDef[] = [
  { id: "red", label: "Red", color: "red", defaultOn: true, trackName: "red" },
];

function gutterTrack(): TrackDto {
  // 5 keys at times 0/25/50/75/100, all value 0.5 → y=150 on 600×300.
  return {
    name: "red",
    keys: [0, 25, 50, 75, 100].map((time) => ({ time, value: 0.5 })),
    interpolation: "linear",
    lockedTo: null,
  };
}

function renderGutterMarquee(
  onMarquee: ReturnType<typeof vi.fn>,
  extra: Partial<React.ComponentProps<typeof CurveEditor>> = {},
) {
  const ref = createRef<CurveMarqueeHandle>();
  const result = render(
    <CurveEditor
      marqueeRef={ref}
      tracks={[gutterTrack()]}
      channels={GUTTER_CHANNELS}
      visibleChannels={{ red: true }}
      focusChannel="red"
      valueRange={{ min: 0, max: 1 }}
      width={600}
      height={300}
      onCanvasMarqueeSelect={onMarquee}
      {...extra}
    />,
  );
  const svg = result.container.querySelector(
    "[data-testid='curve-editor-svg']",
  ) as SVGSVGElement;
  svg.getBoundingClientRect = () =>
    ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
  return { ...result, svg, ref };
}

describe("CurveEditor — gutter-initiated marquee (CRV multi-channel)", () => {
  it("startMarquee from a left-gutter origin sweeps and selects the covered keys", () => {
    const onMarquee = vi.fn();
    const { svg, ref } = renderGutterMarquee(onMarquee);
    // Begin in the left Y-gutter (clientX negative) at the bottom edge.
    act(() => ref.current!.startMarquee(-50, 300, false, 20));
    // Sweep up and right, past slop, covering times 0..75 (x ≤ 480; x=600 out).
    fireEvent.pointerMove(svg, { pointerId: 20, clientX: 480, clientY: 0 });
    fireEvent.pointerUp(svg, { pointerId: 20, clientX: 480, clientY: 0 });
    expect(onMarquee).toHaveBeenCalledTimes(1);
    expect([...onMarquee.mock.calls[0]![0]].sort((a, b) => a - b)).toEqual([0, 25, 50, 75]);
    expect(onMarquee.mock.calls[0]![1]).toBe(false);
  });

  it("begins a gutter-origin marquee AT the press point, not snapped to the plot edge", () => {
    const onMarquee = vi.fn();
    const { container, svg, ref } = renderGutterMarquee(onMarquee);
    // Press in the left Y-gutter (clientX -50 → viewBox x -50 on the 600px stub).
    act(() => ref.current!.startMarquee(-50, 150, false, 21));
    // Move past slop so the marquee rectangle renders.
    fireEvent.pointerMove(svg, { pointerId: 21, clientX: 300, clientY: 150 });
    const rect = container.querySelector("[data-testid='curve-marquee']");
    expect(rect).not.toBeNull();
    // The rectangle starts at the raw gutter x (-50) — it does NOT snap to 0.
    // The SVG's overflow="visible" renders that into the margin.
    expect(rect!.getAttribute("x")).toBe("-50");
  });

  it("a trailing click after a gutter marquee does NOT clear the selection (suppresses onCanvasClick)", () => {
    const onMarquee = vi.fn();
    const onCanvasClick = vi.fn();
    const { svg, ref } = renderGutterMarquee(onMarquee, { onCanvasClick });
    act(() => ref.current!.startMarquee(-50, 300, false, 22));
    fireEvent.pointerMove(svg, { pointerId: 22, clientX: 480, clientY: 0 });
    fireEvent.pointerUp(svg, { pointerId: 22, clientX: 480, clientY: 0 });
    expect(onMarquee).toHaveBeenCalledTimes(1);
    // A real browser fires a synthetic click on the captured SVG right after a
    // drag. Because the gutter marquee captures the SVG (not the backdrop),
    // that click lands on the SVG element — whose onClick must honour the
    // marquee's click-suppression flag, or it clears the just-made selection.
    fireEvent.click(svg);
    expect(onCanvasClick).not.toHaveBeenCalled();
  });
});
