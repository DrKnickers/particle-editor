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
import { afterEach, describe, it, expect, vi } from "vitest";
import { act, fireEvent, render, waitFor } from "@testing-library/react";
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

// ─── Locked focus channel (read-only mirror) ─────────────────────────────────
//
// When the focus channel has lockedTo set, the renderer must:
//   - mark the focus <g> with data-readonly="true"
//   - dash the stroke and hollow the markers (visual signal)
//   - suppress key drag and canvas marquee (gesture-inert)
//   - still allow backdrop clicks through to onCanvasClick (clear-selection UX)

const LOCK_RED_CHANNEL: ChannelDef = {
  id: "red", label: "Red", color: "#FF0000", defaultOn: true, trackName: "red",
};
const LOCK_GREEN_CHANNEL: ChannelDef = {
  id: "green", label: "Green", color: "#00FF00", defaultOn: true, trackName: "green",
};

function makeLockedTrack(name: "red" | "green", lockedTo: "red" | null): TrackDto {
  return {
    name,
    keys: [
      { time: 0, value: 0 },
      { time: 50, value: 0.5 },
      { time: 100, value: 1 },
    ],
    interpolation: "linear",
    lockedTo,
  };
}

/** Render the multi-channel CurveEditor with red + green tracks, green
 *  focused and optionally locked. Returns container + svg + backdrop. */
function renderLockFixture(
  greenLockedTo: "red" | null,
  extras: Partial<React.ComponentProps<typeof CurveEditor>> = {},
) {
  const result = render(
    <CurveEditor
      tracks={[makeLockedTrack("red", null), makeLockedTrack("green", greenLockedTo)]}
      channels={[LOCK_RED_CHANNEL, LOCK_GREEN_CHANNEL]}
      visibleChannels={{ red: true, green: true }}
      focusChannel="green"
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
  if (backdrop) {
    backdrop.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);
  }
  return { ...result, svg, backdrop };
}

describe("locked focus channel (read-only mirror)", () => {
  it("dashed + hollow + data-readonly when locked", () => {
    const { container } = renderLockFixture("red");
    const focusG = container.querySelector(
      '[data-channel-id="green"][data-focus="true"]',
    ) as Element;
    expect(focusG).not.toBeNull();
    expect(focusG.getAttribute("data-readonly")).toBe("true");

    const polyline = focusG.querySelector(
      '[data-testid="curve-polyline"]',
    ) as SVGPolylineElement;
    expect(polyline).not.toBeNull();
    expect(polyline.getAttribute("stroke-dasharray")).toBe("7 5");

    const markers = focusG.querySelectorAll(".curve-key-marker");
    expect(markers.length).toBeGreaterThan(0);
    for (const m of markers) {
      expect(m.getAttribute("fill")).toBe("none");
      expect(m.getAttribute("stroke")).toBe(LOCK_GREEN_CHANNEL.color);
    }
  });

  it("smooth interpolation on a locked focus also carries stroke-dasharray", () => {
    // Re-render the locked fixture but with smooth interpolation so the
    // renderer takes the <path data-testid="curve-path"> branch instead
    // of the linear <polyline data-testid="curve-polyline"> branch.
    const smoothRed: TrackDto = { ...makeLockedTrack("red", null), interpolation: "smooth" };
    const smoothGreen: TrackDto = { ...makeLockedTrack("green", "red"), interpolation: "smooth" };
    const { container } = render(
      <CurveEditor
        tracks={[smoothRed, smoothGreen]}
        channels={[LOCK_RED_CHANNEL, LOCK_GREEN_CHANNEL]}
        visibleChannels={{ red: true, green: true }}
        focusChannel="green"
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
      />,
    );
    const focusG = container.querySelector(
      '[data-channel-id="green"][data-focus="true"]',
    ) as Element;
    expect(focusG).not.toBeNull();
    const path = focusG.querySelector('[data-testid="curve-path"]') as SVGPathElement | null;
    expect(path).not.toBeNull();
    expect(path!.getAttribute("stroke-dasharray")).toBe("7 5");
  });

  it("solid + filled when unlocked", () => {
    const { container } = renderLockFixture(null);
    const focusG = container.querySelector(
      '[data-channel-id="green"][data-focus="true"]',
    ) as Element;
    expect(focusG).not.toBeNull();
    expect(focusG.getAttribute("data-readonly")).toBe("false");

    const polyline = focusG.querySelector('[data-testid="curve-polyline"]') as SVGPolylineElement;
    expect(polyline).not.toBeNull();
    expect(polyline.hasAttribute("stroke-dasharray")).toBe(false);

    const markers = focusG.querySelectorAll(".curve-key-marker");
    expect(markers.length).toBeGreaterThan(0);
    for (const m of markers) {
      expect(m.getAttribute("fill")).toBe(LOCK_GREEN_CHANNEL.color);
    }
  });

  it("no drag on a locked focus key", () => {
    const onKeyDragStart = vi.fn();
    const onKeyDragEnd = vi.fn();
    const { container, svg } = renderLockFixture("red", {
      onKeyDragStart,
      onKeyDragEnd,
    });
    const hitPads = container.querySelectorAll('[data-testid="curve-key"][data-channel-id="green"]');
    expect(hitPads.length).toBeGreaterThan(0);
    const pad = container.querySelector('[data-testid="curve-key"][data-key-time="50"]') as SVGCircleElement;
    expect(pad).not.toBeNull();
    // Pointer-down on the middle key hit-pad.
    fireEvent.pointerDown(pad, { pointerId: 50, button: 0, clientX: 300, clientY: 150 });
    // Pointer-move past slop on the SVG.
    fireEvent.pointerMove(svg, { pointerId: 50, clientX: 200, clientY: 100 });
    // Pointer-up.
    fireEvent.pointerUp(svg, { pointerId: 50, clientX: 200, clientY: 100 });
    expect(onKeyDragStart).not.toHaveBeenCalled();
    expect(onKeyDragEnd).not.toHaveBeenCalled();
  });

  it("no marquee on a locked focus canvas, click still clears", () => {
    const onCanvasMarqueeSelect = vi.fn();
    const onCanvasClick = vi.fn();
    const { container, svg, backdrop } = renderLockFixture("red", {
      onCanvasMarqueeSelect,
      onCanvasClick,
    });
    // Pointer-down on backdrop then move past slop.
    fireEvent.pointerDown(backdrop, { pointerId: 51, button: 0, clientX: 100, clientY: 50 });
    fireEvent.pointerMove(svg, { pointerId: 51, clientX: 400, clientY: 250 });
    // No marquee rect should have mounted.
    expect(container.querySelector('[data-testid="curve-marquee"]')).toBeNull();
    fireEvent.pointerUp(svg, { pointerId: 51, clientX: 400, clientY: 250 });
    expect(onCanvasMarqueeSelect).not.toHaveBeenCalled();
    // Plain click on the backdrop should still fire onCanvasClick.
    fireEvent.click(backdrop);
    expect(onCanvasClick).toHaveBeenCalledTimes(1);
  });

  it("insertMode + locked focus: pointer-down does NOT call onCanvasAdd", () => {
    // Fix 1: the focusReadOnly guard is hoisted before the insertMode branch,
    // so insert is suppressed on a locked canvas even when insertMode=true.
    const onCanvasAdd = vi.fn();
    const { backdrop } = renderLockFixture("red", {
      insertMode: true,
      onCanvasAdd,
    });
    fireEvent.pointerDown(backdrop, { pointerId: 60, button: 0, clientX: 180, clientY: 90 });
    expect(onCanvasAdd).not.toHaveBeenCalled();
  });

  it("right-click and click on a locked hit pad do NOT invoke onKeyContextMenu / onKeyClick", () => {
    // Fixes 2 + 3: context-menu and click handlers bail immediately when
    // focusReadOnly, so the callbacks are never reached.
    const onKeyContextMenu = vi.fn();
    const onKeyClick = vi.fn();
    const { container } = renderLockFixture("red", {
      onKeyContextMenu,
      onKeyClick,
    });
    const pad = container.querySelector(
      '[data-testid="curve-key"][data-channel-id="green"][data-key-time="50"]',
    ) as SVGCircleElement;
    expect(pad).not.toBeNull();
    fireEvent.contextMenu(pad);
    expect(onKeyContextMenu).not.toHaveBeenCalled();
    fireEvent.click(pad);
    expect(onKeyClick).not.toHaveBeenCalled();
  });
});

// ─── Curve morph animation tests ─────────────────────────────────────────────
//
// These tests stub window.matchMedia so morphs RUN (jsdom lacks it by
// default, which is the mechanism that keeps the other ~720 tests in
// snap mode).

// Helper: build a TrackDto key point.
function k(time: number, value: number): { time: number; value: number } {
  return { time, value };
}

// Helper: build a full TrackDto.
// Name must be a TrackName literal; "red" and "green" are valid in the schema.
function trk(
  name: "red" | "green",
  keys: Array<{ time: number; value: number }>,
  interpolation: InterpolationType = "linear",
  lockedTo: "red" | "green" | null = null,
): TrackDto {
  return { name, keys, interpolation, lockedTo };
}

// A canonical 3-key set shared across tests.
const KEYS3 = [k(0, 0), k(50, 0.5), k(100, 1)];

// Channel definitions for morph tests.
// Re-use the lock fixtures' pattern: `trackName` must be a valid TrackName
// literal. "red" and "green" are valid TrackName values in the schema.
const MORPH_RED_CHANNEL = {
  id: "red", label: "Red", color: "#FF0000", defaultOn: true, trackName: "red",
} satisfies ChannelDef;
const MORPH_GREEN_CHANNEL = {
  id: "green", label: "Green", color: "#00FF00", defaultOn: true, trackName: "green",
} satisfies ChannelDef;

/** Render the multi-channel CurveEditor with the given tracks, focusing
 *  the given channel. Width/height are pinned to 600×300. */
function mcCurve(
  tracks: TrackDto[],
  focusId: string,
): React.ReactElement {
  const channelDefs = tracks.map((t) =>
    t.name === "green" ? MORPH_GREEN_CHANNEL : MORPH_RED_CHANNEL,
  );
  const visibleChannels = Object.fromEntries(tracks.map((t) => [t.name, true]));
  return (
    <CurveEditor
      tracks={tracks}
      channels={channelDefs}
      visibleChannels={visibleChannels}
      focusChannel={focusId}
      valueRange={{ min: 0, max: 1 }}
      width={600}
      height={300}
    />
  );
}

// matchMedia stub — morphs RUN (reduce=false).
function stubMatchMediaMotionOn(): () => void {
  const realMM = (window as Window & typeof globalThis).matchMedia;
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: (q: string) => ({
      matches: false, // prefers-reduced-motion: reduce → false → motion OK
      media: q,
      onchange: null,
      addEventListener() {},
      removeEventListener() {},
      addListener() {},
      removeListener() {},
      dispatchEvent() { return false; },
    }),
  });
  return () => {
    Object.defineProperty(window, "matchMedia", { configurable: true, value: realMM });
  };
}

// matchMedia stub — reduced-motion ON.
function stubMatchMediaMotionOff(): () => void {
  const realMM = (window as Window & typeof globalThis).matchMedia;
  Object.defineProperty(window, "matchMedia", {
    configurable: true,
    value: (q: string) => ({
      matches: q.includes("reduce"), // prefers-reduced-motion: reduce → true → NO motion
      media: q,
      onchange: null,
      addEventListener() {},
      removeEventListener() {},
      addListener() {},
      removeListener() {},
      dispatchEvent() { return false; },
    }),
  });
  return () => {
    Object.defineProperty(window, "matchMedia", { configurable: true, value: realMM });
  };
}

/** Variant of mcCurve that lets the caller override the channel colour.
 *  Used for the var(...) colour branch test. */
function mcCurveWithColor(
  tracks: TrackDto[],
  focusId: string,
  color: string,
): React.ReactElement {
  const channelDefs: ChannelDef[] = tracks.map((t) => ({
    id: t.name,
    label: t.name,
    color,
    defaultOn: true,
    trackName: t.name as ChannelDef["trackName"],
  }));
  const visibleChannels = Object.fromEntries(tracks.map((t) => [t.name, true]));
  return (
    <CurveEditor
      tracks={tracks}
      channels={channelDefs}
      visibleChannels={visibleChannels}
      focusChannel={focusId}
      valueRange={{ min: 0, max: 1 }}
      width={600}
      height={300}
    />
  );
}

describe("curve morph (structural changes)", () => {
  let restoreMatchMedia: (() => void) | null = null;

  afterEach(() => {
    restoreMatchMedia?.();
    restoreMatchMedia = null;
  });

  it("mounts a morph overlay on a structural change, hides the static curve, then settles", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));

    const t1 = [trk("red", [k(0, 0), k(50, 0.9), k(100, 1)], "linear")];
    rerender(mcCurve(t1, "red"));

    // Overlay should mount.
    const overlay = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // Static focus layer should be hidden while morphing.
    const staticLayer = container.querySelector('[data-channel-id="red"][data-focus="true"]')!;
    expect((staticLayer as SVGGElement).style.visibility).toBe("hidden");

    // After the morph completes, overlay unmounts and static layer re-appears.
    await waitFor(() => {
      expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    }, { timeout: 2000 });

    expect((staticLayer as SVGGElement).style.visibility).not.toBe("hidden");
    // Suppress unused-variable warning — overlay was captured to verify the
    // same node is used throughout.
    void overlay;
  });

  it("interp change morphs; locked follower morphs with stroke-dasharray '7 5' on its overlay polyline", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", KEYS3, "linear"), trk("green", KEYS3, "linear", "red")];
    const { rerender, container } = render(mcCurve(t0, "green"));

    const t1 = [trk("red", KEYS3, "smooth"), trk("green", KEYS3, "smooth", "red")];
    rerender(mcCurve(t1, "green"));

    // The green channel is the focused+locked follower — its overlay polyline
    // should carry the READONLY_DASH ("7 5").
    const line = await waitFor(() => {
      const el = container.querySelector(
        '[data-testid="curve-morph-overlay"][data-channel-id="green"] polyline',
      );
      expect(el).not.toBeNull();
      return el!;
    });

    expect(line.getAttribute("stroke-dasharray")).toBe("7 5");
  });

  it("no matchMedia (jsdom default) => no overlay ever mounts", async () => {
    // Run WITHOUT any stub — jsdom has no matchMedia, so the gate blocks morphs.
    // Ensure matchMedia is genuinely absent for this test.
    const savedMM = (window as unknown as Record<string, unknown>).matchMedia;
    delete (window as unknown as Record<string, unknown>).matchMedia;
    try {
      const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
      const { rerender, container } = render(mcCurve(t0, "red"));
      rerender(mcCurve([trk("red", [k(0, 0), k(50, 1), k(100, 1)], "linear")], "red"));
      // Give any rAF-backed morph time to manifest (it shouldn't).
      await new Promise<void>((r) => setTimeout(r, 50));
      expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    } finally {
      (window as unknown as Record<string, unknown>).matchMedia = savedMM;
    }
  });

  it("reduced-motion => no overlay", async () => {
    restoreMatchMedia = stubMatchMediaMotionOff(); // prefers-reduced-motion: reduce = true

    const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));
    rerender(mcCurve([trk("red", [k(0, 0), k(50, 1), k(100, 1)], "linear")], "red"));
    // Give rAF time to fire if the gate erroneously passes.
    await new Promise<void>((r) => setTimeout(r, 50));
    expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
  });

  it("interruption: a second structural change mid-morph retargets without unmounting the overlay", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));

    // First structural change — starts a morph.
    rerender(mcCurve([trk("red", [k(0, 0), k(40, 0.8), k(100, 1)], "linear")], "red"));

    // Wait for the overlay to appear.
    const overlay = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // About 30ms into the morph, issue a second structural change.
    await new Promise<void>((r) => setTimeout(r, 30));
    rerender(mcCurve([trk("red", [k(0, 0), k(60, 0.3), k(100, 1)], "linear")], "red"));

    // The overlay element should be the SAME node (not unmounted/remounted).
    await new Promise<void>((r) => setTimeout(r, 10));
    const overlayAfterRetarget = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]');
    expect(overlayAfterRetarget).toBe(overlay);

    // Eventually the morph settles.
    await waitFor(() => {
      expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    }, { timeout: 2000 });
  });

  it("sustained interruption-folding: repeated retargets past 430 ms never trigger the stale fallback", async () => {
    // Regression test for the stale-fallback bug: before the fix, the fallback
    // was set ONCE at ensureLoop() call time (at the first retarget). A stream
    // of retargets extending the total duration past MORPH_MS+250 (~430ms)
    // would hit that deadline and snap everything to done early.
    //
    // After the fix, each retarget resets the fallback to MORPH_MS+250 from
    // NOW, so the overlay survives for the full sequence.
    //
    // Approach: retarget 4 times at ~80ms intervals (t≈0, 80, 160, 240, 320).
    // Assert the overlay still exists at ~500ms (well past the old 430ms
    // stale deadline, but only ~180ms after the last retarget). Then wait for
    // natural completion.
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));

    // Retarget 1 — starts the morph.
    rerender(mcCurve([trk("red", [k(0, 0), k(30, 0.7), k(100, 1)], "linear")], "red"));
    await waitFor(() => {
      expect(container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]')).not.toBeNull();
    });

    // Retarget 2 at ~80 ms.
    await new Promise<void>((r) => setTimeout(r, 80));
    rerender(mcCurve([trk("red", [k(0, 0), k(45, 0.4), k(100, 1)], "linear")], "red"));

    // Retarget 3 at ~160 ms.
    await new Promise<void>((r) => setTimeout(r, 80));
    rerender(mcCurve([trk("red", [k(0, 0), k(55, 0.6), k(100, 1)], "linear")], "red"));

    // Retarget 4 at ~240 ms.
    await new Promise<void>((r) => setTimeout(r, 80));
    rerender(mcCurve([trk("red", [k(0, 0), k(65, 0.2), k(100, 1)], "linear")], "red"));

    // Retarget 5 at ~320 ms.
    await new Promise<void>((r) => setTimeout(r, 80));
    rerender(mcCurve([trk("red", [k(0, 0), k(70, 0.9), k(100, 1)], "linear")], "red"));

    // At ~400 ms (>430 ms from the first start but only ~80 ms after last
    // retarget), wait another 100 ms (total ~500 ms from first retarget).
    // The stale-fallback bug would have snapped done at ~430 ms; the fixed
    // version must NOT have snapped.
    await new Promise<void>((r) => setTimeout(r, 100));
    expect(
      container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]'),
      "overlay must still exist at ~500 ms (fallback must NOT have fired at ~430 ms)",
    ).not.toBeNull();

    // Allow the morph to settle naturally (MORPH_MS=180 from the last
    // retarget at ~320 ms → done by ~500 ms + rAF latency). Give extra
    // headroom for CI.
    await waitFor(() => {
      expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    }, { timeout: 2000 });
  });

  it("focus-channel markers: matched keys glide, added key pops in, removed key ghosts out", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", [k(0, 0), k(50, 0.5), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));

    // delete the 50-key, add a 75-key in one change (paste-like)
    rerender(mcCurve([trk("red", [k(0, 0), k(75, 0.9), k(100, 1)], "linear")], "red"));

    const overlay = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // mid-morph: overlay carries marker circles — 2 moved (0, 100) + 1 in (75) + 1 ghost (50) = 4
    await waitFor(() => {
      expect(overlay.querySelectorAll("circle").length).toBe(4);
    });

    // Structural diversity check: the 4 circles must not all share the same cx.
    // Move markers sit at px 0 and 600 (keys at time 0 and 100 are stable).
    // The "in" marker sits at px 450 (time 75) and the "out" at px 300 (time 50).
    // A regression that creates 4 circles with wrong choreography (e.g. all at
    // the same position) will fail here even if the count is still 4.
    const circles4 = Array.from(overlay.querySelectorAll("circle"));
    const cxValues = new Set(circles4.map((c) => c.getAttribute("cx")));
    expect(cxValues.size, "all 4 marker circles must have distinct cx values").toBe(4);

    await waitFor(() => {
      expect(container.querySelector('[data-testid="curve-morph-overlay"]')).toBeNull();
    }, { timeout: 2000 });
  });

  it("non-focus channels morph their line but render no overlay markers", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    const t0 = [trk("red", KEYS3, "linear"), trk("green", KEYS3, "linear")];
    const { rerender, container } = render(mcCurve(t0, "red")); // focus red; green is background

    rerender(mcCurve([trk("red", KEYS3, "linear"),
                      trk("green", [k(0, 0), k(40, 1), k(100, 0.5)], "linear")], "red"));

    const overlay = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="green"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // Give at least one rAF tick so drawJob runs.
    await new Promise<void>((r) => setTimeout(r, 30));

    expect(overlay.querySelectorAll("circle").length).toBe(0);
  });

  it("var(...) channel colour: overlay polyline stroke is the var string; focus fill references gradient with var(...) stops", async () => {
    // The focus overlay fill must use a self-contained linearGradient whose
    // stop-color attributes carry the channel colour verbatim — even when the
    // colour is a CSS variable token (e.g. var(--x-axis)) that is not parseable
    // as hex. The gradient id is morph-fill-<channelId>. The fill path's fill
    // attribute is url(#morph-fill-<channelId>).
    restoreMatchMedia = stubMatchMediaMotionOn();

    const varColor = "var(--x-axis)";
    const t0 = [trk("red", [k(0, 0), k(100, 1)], "linear")];
    const { rerender, container } = render(mcCurveWithColor(t0, "red", varColor));

    rerender(mcCurveWithColor([trk("red", [k(0, 0), k(50, 0.9), k(100, 1)], "linear")], "red", varColor));

    // Wait for the overlay to mount and for drawJob to create the imperative children.
    const overlayG = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // Give at least one rAF tick so drawJob creates the imperative children.
    await new Promise<void>((r) => setTimeout(r, 30));

    const polyline = overlayG.querySelector("polyline");
    expect(polyline, "overlay polyline must exist after first rAF tick").not.toBeNull();
    // The var(...) colour must be forwarded verbatim as the stroke — no NaN corruption.
    expect(polyline!.getAttribute("stroke")).toBe(varColor);

    // Focus fill: path fill must reference the gradient, not a flat colour.
    const fillPath = overlayG.querySelector("path");
    expect(fillPath, "overlay fill path must exist for focus channel").not.toBeNull();
    expect(fillPath!.getAttribute("fill")).toBe("url(#morph-fill-red)");
    // No flat fill-opacity attribute (gradient handles opacity).
    expect(fillPath!.getAttribute("fill-opacity")).toBeNull();

    // A <linearGradient> with the correct id and two stops must exist.
    const grad = overlayG.querySelector("linearGradient#morph-fill-red");
    expect(grad, "linearGradient#morph-fill-red must exist in overlay").not.toBeNull();
    const stops = grad!.querySelectorAll("stop");
    expect(stops).toHaveLength(2);
    // Both stops must carry the channel colour verbatim.
    expect(stops[0]!.getAttribute("stop-color")).toBe(varColor);
    expect(stops[1]!.getAttribute("stop-color")).toBe(varColor);
    // Stops must have the matching opacities.
    expect(stops[0]!.getAttribute("stop-opacity")).toBe("0.25");
    expect(stops[1]!.getAttribute("stop-opacity")).toBe("0");
  });

  it("non-focus channel morph overlay has no fill path", async () => {
    // The static layer draws no fill under non-focus (background) channels.
    // The overlay must match: only a polyline, no <path> fill element.
    restoreMatchMedia = stubMatchMediaMotionOn();

    // red is focus; green is the background channel that will morph.
    const t0 = [trk("red", KEYS3, "linear"), trk("green", KEYS3, "linear")];
    const { rerender, container } = render(mcCurve(t0, "red"));

    rerender(mcCurve([trk("red", KEYS3, "linear"),
                      trk("green", [k(0, 0), k(40, 1), k(100, 0.5)], "linear")], "red"));

    const overlay = await waitFor(() => {
      const el = container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="green"]');
      expect(el).not.toBeNull();
      return el!;
    });

    // Give at least one rAF tick so drawJob runs.
    await new Promise<void>((r) => setTimeout(r, 30));

    // Non-focus: no fill <path> should exist.
    expect(overlay.querySelector("path"), "non-focus overlay must not have a fill path").toBeNull();
    // But the line polyline must be there.
    expect(overlay.querySelector("polyline"), "non-focus overlay must have a polyline").not.toBeNull();
  });

  it("a drag-committed move does not re-morph the dragged channel, but its locked follower morphs", async () => {
    restoreMatchMedia = stubMatchMediaMotionOn();

    // red is focus; green is locked-to-red — both have identical keys.
    const t0 = [
      trk("red",   [k(0, 0), k(50, 0.5), k(100, 1)], "linear"),
      trk("green", [k(0, 0), k(50, 0.5), k(100, 1)], "linear", "red"),
    ];
    const onKeyDragEnd = vi.fn();

    const { rerender, container } = render(
      <CurveEditor
        tracks={t0}
        channels={[MORPH_RED_CHANNEL, MORPH_GREEN_CHANNEL]}
        visibleChannels={{ red: true, green: true }}
        focusChannel="red"
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        onKeyDragEnd={onKeyDragEnd}
      />,
    );

    // Stub getBoundingClientRect so eventToViewBox gets a valid scale.
    const svg = container.querySelector('[data-testid="curve-editor-svg"]') as SVGSVGElement;
    svg.getBoundingClientRect = () =>
      ({ left: 0, top: 0, right: 600, bottom: 300, width: 600, height: 300, x: 0, y: 0, toJSON: () => "" } as DOMRect);

    // Find the hit-pad for the t=50 key on the red (focus) channel.
    // data-key-time="50" on the focus channel's <circle>.
    const pad = container.querySelector(
      '[data-testid="curve-key"][data-key-time="50"][data-channel-id="red"]',
    )!;
    expect(pad).not.toBeNull();

    // Simulate a drag: down at key position, move past slop, up.
    // t=50 → x=300; v=0.5 → y=150.
    fireEvent.pointerDown(pad, { button: 0, pointerId: 99, clientX: 300, clientY: 150 });
    fireEvent.pointerMove(svg, { pointerId: 99, clientX: 340, clientY: 120 });
    fireEvent.pointerUp(svg,  { pointerId: 99, clientX: 340, clientY: 120 });

    // onKeyDragEnd must have fired with (oldTime=50, newTime, newValue).
    expect(onKeyDragEnd).toHaveBeenCalledTimes(1);
    const [, committedTime, committedValue] = onKeyDragEnd.mock.calls[0] as [number, number, number];

    // Build t1: red's 50-key is now at the committed position; green mirrors it.
    // Both tracks get the moved key; the other keys stay in place.
    const t1 = [
      trk("red",   [k(0, 0), k(committedTime, committedValue), k(100, 1)], "linear"),
      trk("green", [k(0, 0), k(committedTime, committedValue), k(100, 1)], "linear", "red"),
    ];

    rerender(
      <CurveEditor
        tracks={t1}
        channels={[MORPH_RED_CHANNEL, MORPH_GREEN_CHANNEL]}
        visibleChannels={{ red: true, green: true }}
        focusChannel="red"
        valueRange={{ min: 0, max: 1 }}
        width={600}
        height={300}
        onKeyDragEnd={onKeyDragEnd}
      />,
    );

    // The locked follower (green) MUST morph — its change wasn't the dragged channel.
    await waitFor(() => {
      expect(
        container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="green"]'),
      ).not.toBeNull();
    });

    // The dragged channel (red) must NOT morph — suppression swallows it.
    expect(
      container.querySelector('[data-testid="curve-morph-overlay"][data-channel-id="red"]'),
    ).toBeNull();
  });
});
