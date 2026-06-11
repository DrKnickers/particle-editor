// CurveEditor — SVG renderer for a single track.
//
// Phase 3 Screen 6 Batch A: read-only foundation.
// Screen 5 / Screen 6 Batch B-α:
//   - Key selection (click + Ctrl/Cmd+click toggle, click empty SVG to
//     clear). Selection state is OWNED by the parent (TrackEditor or
//     EmitterPropertyPanel) and identified by key TIME — not array
//     index — so a future drag-to-move (Batch B) can re-order keys in
//     the underlying multiset without invalidating the selection.
//   - Smooth (cubic-Bezier) + step (staircase) rendering branches.
//     The control-point formula matches the legacy implementation at
//     [src/UI/CurveEditor.cpp:289-292]:
//       cp1 = (p1.x + (p2.x - p1.x) / 4, p1.y)
//       cp2 = (p1.x + (p2.x - p1.x) * 3 / 4, p2.y)
//     Step expands each segment as [p1, (p2.x, p1.y), p2] so a single
//     <polyline> can render the staircase.
//
// Screen 6 Batch B-β adds:
//   - Drag-to-move via pointer events. Pointer-down on a key starts a
//     drag (local state); pointer-move re-projects the dragged key to
//     the new screen position, clamped to:
//       * border keys (first + last by time order): time fixed; value
//         clamped to [valueRange.min, valueRange.max].
//       * interior keys: time clamped to `(prev.time, next.time)`
//         EXCLUSIVE; value clamped to track range.
//     Pointer-up commits via `onKeyDragEnd` so the parent can fire
//     `emitters/set-track-key`. We rely on `setPointerCapture` to
//     receive pointer-move events even when the cursor leaves the
//     element. jsdom doesn't implement setPointerCapture; we guard
//     each call with a `typeof` check so the Vitest pointer-event
//     specs still exercise the drag math without throwing.
//   - Click-to-add via canvas pointer-down in Insert mode. The parent
//     passes `insertMode` and the canvas inverse-maps the pointer's
//     (x, y) to (time, value) before invoking `onCanvasAdd`.
//   - Border-key visual: first + last keys (by time order) render
//     with a stroke ring (sky-500 accent + 1.5 stroke-width) and a
//     slightly darker un-selected fill so they read as "anchor"
//     points distinct from interior keys. When selected they keep
//     the selected styling (filled accent + r=5) and the ring stroke
//     as a layered cue.
//
// Phase 4.1 Fix dispatch 5 adds:
//   - Marquee (rubber-band) select on empty-canvas pointer-down in
//     Select mode. While dragging, a semi-transparent rectangle with
//     a dashed border tracks the cursor. At pointer-up every key
//     whose projected (x, y) falls inside the rectangle (INCLUSIVE
//     on both edges in viewBox space) is collected and passed to
//     `onCanvasMarqueeSelect`. Shift-held marquee passes
//     `shift: true` so the parent appends rather than replaces.
//     Esc during an active marquee cancels — the rectangle is
//     cleared and the callback is NOT fired. When the gesture never
//     grows past `DRAG_SLOP` between down and up we treat it as a
//     plain click and fire `onCanvasClick` (preserves the existing
//     "click empty area to clear selection" UX from). Insert
//     mode is unchanged: empty-canvas pointer-down still fires
//     `onCanvasAdd` and marquee is suppressed.

import { useEffect, useImperativeHandle, useLayoutEffect, useRef, useState, type PointerEvent as ReactPointerEvent, type Ref } from "react";
import type { InterpolationType, TrackDto, TrackName } from "@particle-editor/bridge-schema";
import { useCurveMorph, type SuppressedMove } from "../lib/use-curve-morph";

/** Channel definition for the multi-channel overlay branch (Task 2.6).
 *  `id` is the UI-facing identifier (e.g. "rotation"); `trackName` is
 *  the wire-level TrackName (e.g. "rotationSpeed") used to look up the
 *  track in the `tracks` array. `color` is a CSS colour string (token
 *  ref like `var(--warning)` or raw hex). */
export type ChannelDef = {
  id: string;
  label: string;
  color: string;
  defaultOn: boolean;
  trackName: TrackName;
};

type Props = {
  /** The track to render in single-track mode. `keys` are expected
   *  sorted ascending by time but the component doesn't re-sort — it
   *  trusts the wire contract. Optional only because the multi-
   *  channel branch (when `tracks` + `channels` are passed) doesn't
   *  need it. */
  track?: TrackDto;
  /** Multi-channel overlay branch (Task 2.6): when `tracks` +
   *  `channels` + `visibleChannels` are all passed, the renderer
   *  ignores `track` / `valueRange` and instead draws one curve per
   *  visible channel in the channel's colour. Multi-channel mode is
   *  view-only — selection / drag / marquee paths are no-ops because
   *  no parent owns selection state across channels. */
  tracks?: TrackDto[] | null;
  channels?: readonly ChannelDef[];
  visibleChannels?: Record<string, boolean>;
  /** Hybrid focus-channel mode (restored edit surface). When set in the
   *  multi-channel branch, the renderer emphasises this channel (thick
   *  stroke + full opacity + key circles + interactive handlers) and
   *  dims the others (thinner stroke + reduced opacity + no markers).
   *  When unset, the multi-channel branch stays view-only and dims
   *  every channel equally (the Task 2.6 behavior). */
  focusChannel?: string;
  /** Y-axis range for the canvas. In single-track mode this is the
   *  one and only Y range. In multi-channel mode this is the
   *  UNIFIED range across visible channels — every channel's curve
   *  projects into the same Y space so when Scale-at-20 is visible
   *  alongside RGB the canvas extends to 0..20 and the RGB curves
   *  squish near the bottom. When omitted in multi-channel mode the
   *  renderer falls back to per-channel ranges (legacy behaviour;
   *  each curve fills the canvas independently). The drag
   *  value-clamp uses the focus CHANNEL's own range regardless,
   *  so engine bounds aren't violated even when the visible canvas
   *  extends past them. Min defaults to 0 and max to 1. */
  valueRange?: { min: number; max: number };
  /** SVG drawable area in viewBox units. Defaults to 600×300; tests
   *  pin these to deterministic numbers when asserting positions. */
  width?: number;
  height?: number;
  /** Time range. Locked to 0..100 to match legacy
   *  `CurveEditor_SetHorzRange(hEditor, 0.0f, 100.0f, true)` at
   *  [src/UI/CurveEditor.cpp]. Exposed as a prop so future panels
   *  (lifetime-curve sub-editors) can override. */
  timeMin?: number;
  timeMax?: number;
  /** Set of key times currently selected. Identified by TIME (not
   *  array index) so selection survives future key-time mutations. */
  selectedKeyTimes?: ReadonlySet<number>;
  /** Click handler for a single key circle. Receives the key's time
   *  + the raw mouse event so the parent can branch on modifier keys
   *  (Ctrl/Cmd toggle, plain click replace). Click fires only when
   *  the pointer-down did NOT begin a drag (i.e. the pointer didn't
   *  move beyond a small threshold between down and up). */
  onKeyClick?: (time: number, event: React.MouseEvent | React.PointerEvent) => void;
  /** Click handler for the empty SVG canvas (anywhere not on a key
   *  circle). Convention: clear the selection in Select mode. */
  onCanvasClick?: (event: React.MouseEvent) => void;
  /** Insert-mode flag from the parent. When true, pointer-down on
   *  empty canvas computes (time, value) from the pointer position
   *  and fires `onCanvasAdd` instead of `onCanvasClick`. */
  insertMode?: boolean;
  /** Insert-mode add handler. Called with the (time, value) computed
   *  from the pointer position. The parent should fire
   *  `emitters/add-track-key` and (typically) auto-select the new
   *  key. */
  onCanvasAdd?: (time: number, value: number) => void;
  /** Right-click on the empty canvas — convention is "drop back to
   *  Select mode" (legacy parity; matches Photoshop/Blender pen-mode
   *  right-click escape). Called on empty backdrop only — right-click
   *  on a key fires onKeyContextMenu instead. The browser context
   *  menu is suppressed (preventDefault) when this is wired. */
  onCanvasContextMenu?: () => void;
  /** Right-click on a key circle. The parent typically opens a small
   *  popup at (clientX, clientY) with per-key actions (Delete, …).
   *  `isBorder` is true for the first/last key in time order; border
   *  keys can't be deleted (the host filters them out), so the
   *  parent should disable destructive entries accordingly. The
   *  browser context menu is suppressed when this is wired. */
  onKeyContextMenu?: (
    time: number,
    isBorder: boolean,
    clientX: number,
    clientY: number,
  ) => void;
  /** Drag-end handler. Fires after pointer-up when a drag actually
   *  produced a position change. The parent fires
   *  `emitters/set-track-key { oldTime: keyTime, newTime, newValue }`.
   *  When the drag ends with no net movement, this is NOT called;
   *  the original click path fires instead. */
  onKeyDragEnd?: (keyTime: number, newTime: number, newValue: number) => void;
  /** Drag-start handler. Fires on pointer-down on a focus-channel
   *  key BEFORE any movement is observed. The parent uses this to
   *  pre-select the key so it paints with the selected ring the
   *  moment the user grabs it (otherwise the visual selection only
   *  appears on pointer-up via `onKeyClick`, which never fires when
   *  the gesture turns out to be a drag rather than a click). */
  onKeyDragStart?: (keyTime: number) => void;
  /** Drag-move handler. Fires on every pointer-move during an active
   *  drag once movement has crossed `DRAG_SLOP` (so it doesn't fire
   *  on jitter-y clicks). The parent uses this to live-update Time /
   *  Value spinners while the user is mid-drag. */
  onKeyDragMove?: (keyTime: number, currentTime: number, currentValue: number) => void;
  /** Drag-cancel handler. Fires when pointer-cancel interrupts an
   *  active drag (browser hand-off, ESC, pointer leaving the
   *  capturing surface). The parent uses this to roll back any
   *  live-drag visualisation. */
  onKeyDragCancel?: () => void;
  /** an-audit-finding group-drag commit: a drag of one key within a multi-selection
   *  shifts the whole selection by (dTime, dValue). */
  onGroupDragEnd?: (dTime: number, dValue: number) => void;
  /** Group-drag live move: fires on every pointer-move past slop during
   *  a multi-selection group drag, carrying the accumulated
   *  (dTime, dValue). The parent uses it to live-update the Time/Value
   *  spinners (which show the selection AVERAGE) — the group analogue of
   *  `onKeyDragMove`. */
  onGroupDragMove?: (dTime: number, dValue: number) => void;
  /** Marquee-select handler (Phase 4.1 Fix dispatch 5). Fires at
   *  pointer-up when a Select-mode rubber-band drag has covered at
   *  least one key. `times` is the set of key TIMES inside the
   *  rectangle (inclusive on both axes in viewBox space). `shift`
   *  reflects whether Shift was held at marquee-start; the parent
   *  should append to the existing selection when true, replace when
   *  false. When the gesture is too short to qualify as a drag the
   *  marquee is treated as a click and `onCanvasClick` fires
   *  instead. */
  onCanvasMarqueeSelect?: (times: number[], shift: boolean) => void;
  /** CRV: ref to drive the marquee imperatively so it can be STARTED from
   *  outside the plot SVG (the axis-label gutters). Only the multi-channel
   *  focus editor implements it; the single-track branch ignores it. */
  marqueeRef?: Ref<CurveMarqueeHandle>;
};

/** Imperative handle exposed via `marqueeRef` for starting a marquee
 *  selection from a client point outside the plot SVG (gutter-initiated).
 *  Coordinates are clamped to the plot; the existing pointer-capture marquee
 *  machinery drives the rest. */
export type CurveMarqueeHandle = {
  startMarquee: (clientX: number, clientY: number, shiftKey: boolean, pointerId: number) => void;
};

const DEFAULT_WIDTH = 600;
const DEFAULT_HEIGHT = 300;
const DEFAULT_TIME_MIN = 0;
const DEFAULT_TIME_MAX = 100;

const UNSELECTED_FILL = "#e5e5e5";
const BORDER_FILL = "#94A3B8";      // slate-400 — slightly darker than unselected
const BORDER_STROKE = "#0EA5E9";    // accent ring on border (anchor) keys
/** Stroke dash pattern for the locked-mirror curve. Visually distinguishes
 *  a read-only focus channel from an editable one. Feel-tunable. */
const READONLY_DASH = "7 5";

/** Below this many pixels (in viewBox units) of pointer movement
 *  between down and up, we treat it as a click — not a drag. Matches
 *  legacy CurveEditor's hit-test slop. */
const DRAG_SLOP = 1.5;

/** Linear-interpolate a value into 0..1 then map into 0..length.
 *  Clamps NaN/Infinity at the bounds to prevent broken SVG output. */
function project(value: number, min: number, max: number, length: number): number {
  if (!Number.isFinite(value) || max <= min) return 0;
  const t = (value - min) / (max - min);
  return Math.max(0, Math.min(1, t)) * length;
}

/** Inverse of project — pixel position back to data coordinate. */
function unproject(px: number, min: number, max: number, length: number): number {
  if (length <= 0) return min;
  const t = px / length;
  return min + Math.max(0, Math.min(1, t)) * (max - min);
}

/** Build the SVG path `d` string for a smooth (cubic-Bezier) curve
 *  through the given points. Mirrors the legacy formula at
 *  [src/UI/CurveEditor.cpp:289-292]: control points sit at 1/4 and
 *  3/4 of the horizontal distance, sharing y with the segment's
 *  start / end key respectively. Returns "" when there are fewer
 *  than 2 points (no segment to render). */
function buildSmoothPath(points: ReadonlyArray<{ x: number; y: number }>): string {
  if (points.length < 2) return "";
  const first = points[0]!;
  let d = `M ${first.x} ${first.y}`;
  for (let i = 1; i < points.length; i++) {
    const p1 = points[i - 1]!;
    const p2 = points[i]!;
    const dx = p2.x - p1.x;
    const cp1x = p1.x + dx / 4;
    const cp1y = p1.y;
    const cp2x = p1.x + (dx * 3) / 4;
    const cp2y = p2.y;
    d += ` C ${cp1x} ${cp1y}, ${cp2x} ${cp2y}, ${p2.x} ${p2.y}`;
  }
  return d;
}

/** Build the staircase polyline points for step interpolation. For
 *  each (p1, p2) pair, emits the horizontal leg at p1.y then the
 *  vertical jump to p2.y. Per legacy [src/UI/CurveEditor.cpp:300-318]
 *  the horizontal leg uses the "line pen" and the vertical leg the
 *  "step pen" — visual differentiation is deferred to Batch B; the
 *  shape is identical either way. */
function buildStepPolyline(points: ReadonlyArray<{ x: number; y: number }>): string {
  if (points.length === 0) return "";
  const parts: string[] = [`${points[0]!.x},${points[0]!.y}`];
  for (let i = 1; i < points.length; i++) {
    const p1 = points[i - 1]!;
    const p2 = points[i]!;
    // Horizontal leg, then the key itself.
    parts.push(`${p2.x},${p1.y}`);
    parts.push(`${p2.x},${p2.y}`);
  }
  return parts.join(" ");
}

/** Build the closed area-under-curve path for the gradient fill that
 *  emanates downward from the focus curve to the canvas floor. The
 *  path traces the curve from left to right (matching whichever
 *  interpolation mode is in use), drops straight down to `height`
 *  at the rightmost point, walks left along the floor, and closes.
 *  Caller fills it with a linearGradient that fades from the channel
 *  colour (top, along the curve) to transparent (bottom, at the
 *  floor). The curve stroke is rendered AFTER this path so the line
 *  itself draws crisply on top of the fill. */
function buildFillPath(
  points: ReadonlyArray<{ x: number; y: number }>,
  interp: InterpolationType,
  height: number,
): string {
  if (points.length < 2) return "";
  const first = points[0]!;
  const last = points[points.length - 1]!;
  let d: string;
  if (interp === "smooth") {
    d = buildSmoothPath(points);
  } else if (interp === "step") {
    // Replay the step staircase manually as path commands; using
    // `buildStepPolyline` would give space-separated points suited
    // to <polyline>, not <path>'s `M/L/Z` grammar.
    d = `M ${first.x} ${first.y}`;
    for (let i = 1; i < points.length; i++) {
      const p1 = points[i - 1]!;
      const p2 = points[i]!;
      d += ` L ${p2.x} ${p1.y} L ${p2.x} ${p2.y}`;
    }
  } else {
    // Linear: straight segments between consecutive points.
    d = `M ${first.x} ${first.y}`;
    for (let i = 1; i < points.length; i++) {
      const p = points[i]!;
      d += ` L ${p.x} ${p.y}`;
    }
  }
  // Drop to the floor on the right, walk back left along the floor,
  // close the path back to the starting point.
  d += ` L ${last.x} ${height} L ${first.x} ${height} Z`;
  return d;
}

/** Map a DOM event's (clientX, clientY) to viewBox-space (x, y) using
 *  the SVG element's getBoundingClientRect. Returns (NaN, NaN) when
 *  the bounds aren't measurable (e.g. unmounted element). */
function eventToViewBox(
  svg: SVGSVGElement,
  clientX: number,
  clientY: number,
  width: number,
  height: number,
): { x: number; y: number } {
  const rect = svg.getBoundingClientRect();
  if (rect.width <= 0 || rect.height <= 0) return { x: NaN, y: NaN };
  const x = ((clientX - rect.left) / rect.width) * width;
  const y = ((clientY - rect.top) / rect.height) * height;
  return { x, y };
}

/** Per-channel y-axis range used by the multi-channel overlay branch.
 *  Colour channels lock to [0, 1]; Scale/Index auto-range from 0 up to
 *  the highest key value, floored at a max of 1; RotationSpeed auto-ranges
 *  symmetrically around 0 (expands to the lowest and highest keys, no caps).
 *  Each channel's curve is normalised to fill the panel height — comparing
 *  absolute values across channels isn't the goal of the overlay. */
function valueRangeForTrack(track: TrackDto): { min: number; max: number } {
  switch (track.name) {
    case "red":
    case "green":
    case "blue":
    case "alpha":
      return { min: 0, max: 1 };
    case "scale": {
      // Lower 0, upper auto-grows to highest key, floor at 1. Kept
      // in sync with the CurveEditorPanel copy.
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "index": {
      // Same shape as scale. Kept in sync with the CurveEditorPanel copy.
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "rotationSpeed": {
      // Default 0..1; expands in BOTH directions to include the
      // highest and lowest keys — no caps. Kept in sync with the
      // CurveEditorPanel copy.
      let min = 0;
      let max = 1;
      for (const k of track.keys) {
        if (k.value < min) min = k.value;
        if (k.value > max) max = k.value;
      }
      return { min, max };
    }
  }
}

export function CurveEditor({
  track,
  valueRange,
  tracks,
  channels,
  visibleChannels,
  focusChannel,
  marqueeRef,
  width = DEFAULT_WIDTH,
  height = DEFAULT_HEIGHT,
  timeMin = DEFAULT_TIME_MIN,
  timeMax = DEFAULT_TIME_MAX,
  selectedKeyTimes,
  onKeyClick,
  onCanvasClick,
  insertMode,
  onCanvasAdd,
  onCanvasContextMenu,
  onKeyContextMenu,
  onKeyDragEnd,
  onKeyDragStart,
  onKeyDragMove,
  onKeyDragCancel,
  onGroupDragEnd,
  onGroupDragMove,
  onCanvasMarqueeSelect,
}: Props) {
  // Multi-channel overlay branch. Triggered when the caller provides
  // `tracks` + `channels` + `visibleChannels`. When `focusChannel` is
  // also set, the focus channel renders emphasised + interactive (key
  // circles, drag, marquee, insert, context menu) while the other
  // visible channels render dimmed as background context. When
  // `focusChannel` is unset the branch stays view-only (Task 2.6).
  if (tracks !== undefined && channels !== undefined && visibleChannels !== undefined) {
    return (
      <MultiChannelCurves
        tracks={tracks}
        channels={channels}
        visibleChannels={visibleChannels}
        focusChannel={focusChannel}
        displayRange={valueRange}
        width={width}
        height={height}
        timeMin={timeMin}
        timeMax={timeMax}
        selectedKeyTimes={selectedKeyTimes}
        onKeyClick={onKeyClick}
        onCanvasClick={onCanvasClick}
        insertMode={insertMode}
        onCanvasAdd={onCanvasAdd}
        onCanvasContextMenu={onCanvasContextMenu}
        onKeyContextMenu={onKeyContextMenu}
        onKeyDragEnd={onKeyDragEnd}
        onKeyDragStart={onKeyDragStart}
        onKeyDragMove={onKeyDragMove}
        onKeyDragCancel={onKeyDragCancel}
        onGroupDragEnd={onGroupDragEnd}
        onGroupDragMove={onGroupDragMove}
        onCanvasMarqueeSelect={onCanvasMarqueeSelect}
        marqueeRef={marqueeRef}
      />
    );
  }

  // Single-track legacy branch (unchanged). Requires `track` +
  // `valueRange`; the type system can't enforce the exclusive-or
  // shape via TypeScript discriminated unions without rewriting all
  // callers, so we guard with a runtime null-render instead.
  if (track === undefined || valueRange === undefined) {
    return null;
  }
  const { min: vMin, max: vMax } = valueRange;

  // Pre-compute the projected positions once so the curve and the
  // circles share the same coordinate calculation.
  const points = track.keys.map((k) => ({
    x: project(k.time, timeMin, timeMax, width),
    // Y inversion: SVG origin is top-left. Subtract from height so
    // larger values render higher on the canvas.
    y: height - project(k.value, vMin, vMax, height),
    time: k.time,
    value: k.value,
  }));

  // Border keys (first + last by time order). With keys-ascending-by-
  // time as the wire contract these are simply indices 0 and N-1.
  const borderTimes = new Set<number>();
  if (track.keys.length > 0) {
    borderTimes.add(track.keys[0]!.time);
    borderTimes.add(track.keys[track.keys.length - 1]!.time);
  }

  // Drag state. Held in refs (not useState) so pointer-move handlers
  // can read the latest values without triggering re-renders on every
  // pixel of movement. We DO call setState on the live drag
  // (currentTime/currentValue) so the dragged circle re-renders at the
  // new position — kept in `dragLive` so the read in onPointerMove is
  // ref-backed but the render path is state-backed.
  const dragRef = useRef<{
    keyTime: number;
    startTime: number;
    startValue: number;
    startClientX: number;
    startClientY: number;
    currentTime: number;
    currentValue: number;
    moved: boolean;
    pointerId: number;
    target: Element | null;
  } | null>(null);

  // Force re-render trigger for in-flight drag. dragRef carries the
  // live drag state across pointer-move events without React state's
  // batching delays; we bump a tiny counter to flush a re-render so
  // the dragged circle's cx/cy reflects the latest cursor position.
  const [, setDragTick] = useState(0);

  // Phase 4.1 Fix dispatch 5 — marquee state. Held as React state
  // (not a ref) because the rectangle rendering already needs a
  // re-render on every pointer-move; useState gives us both the
  // value-tracking and the render side-effect in one. Coordinates
  // are stored in VIEWBOX SPACE — same coordinate system as the
  // projected key points, so the hit test at pointer-up is a direct
  // numeric compare with no extra projection step. `clientStartX/Y`
  // is the raw pointer-down client position; we use it to compute
  // movement-past-slop without re-deriving from the viewBox values.
  type MarqueeState = {
    startX: number;       // viewBox space anchor
    startY: number;
    currX: number;        // viewBox space current cursor
    currY: number;
    clientStartX: number; // raw client for slop test
    clientStartY: number;
    shift: boolean;       // shift held at marquee-start
    pointerId: number;
    target: Element | null;
    movedPastSlop: boolean;
  };
  const [marquee, setMarquee] = useState<MarqueeState | null>(null);
  // After a Select-mode pointer-up handled the click-vs-drag decision,
  // the browser still fires a synthetic `click` event on the captured
  // backdrop. We set this ref on every consume from the pointer-up
  // path; the backdrop's onClick reads + clears it before deciding
  // whether to forward to onCanvasClick. Without this, plain clicks
  // would double-fire (once from marquee, once from backdrop click).
  const marqueeConsumedClickRef = useRef(false);

  // Grid: 10 evenly-spaced cells on each axis → 11 lines including
  // the bordering ones. The outer axes (left + bottom) are stroked
  // darker for readability.
  const gridCells = 10;
  const verticalLines: number[] = [];
  for (let i = 0; i <= gridCells; i++) {
    verticalLines.push((i / gridCells) * width);
  }
  const horizontalLines: number[] = [];
  for (let i = 0; i <= gridCells; i++) {
    horizontalLines.push((i / gridCells) * height);
  }

  const interp = track.interpolation;

  /** Begin a drag on a key. Captures the pointer so move/up events
   *  return to the source element even when the cursor leaves it.
   *  jsdom doesn't implement setPointerCapture; we guard with a
   *  `typeof` check so the Vitest pointer specs still exercise the
   *  drag math without throwing. */
  const startDrag = (
    event: ReactPointerEvent<SVGCircleElement>,
    keyTime: number,
    keyValue: number,
  ) => {
    if (event.button !== 0) return;
    // Ctrl/Cmd modifier: defer to the click handler (toggle selection).
    // Pointer-up will fire onKeyClick because moved=false.
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) {
      return;
    }
    dragRef.current = {
      keyTime,
      startTime: keyTime,
      startValue: keyValue,
      startClientX: event.clientX,
      startClientY: event.clientY,
      currentTime: keyTime,
      currentValue: keyValue,
      moved: false,
      pointerId: event.pointerId,
      target: event.currentTarget,
    };
    const t = event.currentTarget;
    // setPointerCapture is undefined in jsdom — guard.
    if (typeof t.setPointerCapture === "function") {
      try {
        t.setPointerCapture(event.pointerId);
      } catch {
        // setPointerCapture can throw in some browsers when the
        // pointer is no longer active; swallow — the drag still
        // works via the document-level fallback.
      }
    }
  };

  /** Pointer-move during an active drag OR marquee. Key drag takes
   *  priority — `dragRef.current !== null` is checked first. Marquee
   *  and key-drag are mutually exclusive in practice because marquee
   *  only begins on the empty backdrop and key-drag only begins on a
   *  circle; the priority is defensive. */
  const onPointerMove = (event: ReactPointerEvent<SVGSVGElement>) => {
    // ── Marquee branch ────────────────────────────────────────────
    if (dragRef.current === null && marquee !== null
        && event.pointerId === marquee.pointerId) {
      const svg = event.currentTarget;
      const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const dx = event.clientX - marquee.clientStartX;
      const dy = event.clientY - marquee.clientStartY;
      const movedNow = Math.abs(dx) > DRAG_SLOP || Math.abs(dy) > DRAG_SLOP;
      setMarquee({
        ...marquee,
        currX: x,
        currY: y,
        movedPastSlop: marquee.movedPastSlop || movedNow,
      });
      return;
    }
    // ── Key-drag branch (original behaviour) ──────────────────────
    const drag = dragRef.current;
    if (drag === null) return;
    if (event.pointerId !== drag.pointerId) return;
    const svg = event.currentTarget;
    const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    let nextTime = unproject(x, timeMin, timeMax, width);
    // Y inversion: pixel y=0 is the top (high value).
    let nextValue = unproject(height - y, vMin, vMax, height);
    // Bounds. Border keys: time fixed to start. Interior keys: time
    // clamped to (prev.time, next.time) exclusive.
    const isBorder = borderTimes.has(drag.startTime);
    if (isBorder) {
      nextTime = drag.startTime;
    } else {
      // Find neighbours in the original keys array (sorted by time).
      const idx = track.keys.findIndex((k) => k.time === drag.startTime);
      if (idx > 0 && idx < track.keys.length - 1) {
        const prevT = track.keys[idx - 1]!.time;
        const nextT = track.keys[idx + 1]!.time;
        // Exclusive bound — pull in by a tiny epsilon so equality with
        // a neighbour is impossible (which would corrupt the multiset).
        const eps = 1e-4;
        nextTime = Math.max(prevT + eps, Math.min(nextT - eps, nextTime));
      } else {
        // Defensive: if findIndex failed (shouldn't happen on an
        // active drag), fall back to the start time.
        nextTime = drag.startTime;
      }
    }
    // Value clamp to track range.
    nextValue = Math.max(vMin, Math.min(vMax, nextValue));
    drag.currentTime = nextTime;
    drag.currentValue = nextValue;
    // Track whether the pointer actually moved past the slop
    // threshold; pointer-up uses this to decide click-vs-drag.
    const dx = event.clientX - drag.startClientX;
    const dy = event.clientY - drag.startClientY;
    if (Math.abs(dx) > DRAG_SLOP || Math.abs(dy) > DRAG_SLOP) {
      drag.moved = true;
    }
    setDragTick((n) => n + 1);
  };

  /** Pointer-up — commit drag, commit marquee, or treat as click. */
  const onPointerUp = (event: ReactPointerEvent<SVGSVGElement>) => {
    // ── Marquee branch ────────────────────────────────────────────
    if (dragRef.current === null && marquee !== null
        && event.pointerId === marquee.pointerId) {
      const { startX, startY, currX, currY, shift, movedPastSlop, target } = marquee;
      // Release pointer capture if held.
      if (target !== null) {
        const el = target as Element & { releasePointerCapture?: (id: number) => void };
        if (typeof el.releasePointerCapture === "function") {
          try { el.releasePointerCapture(event.pointerId); } catch { /* swallow */ }
        }
      }
      setMarquee(null);
      // The synthetic `click` event will fire on the backdrop right
      // after this pointer-up. Set the suppress flag so the backdrop
      // onClick doesn't re-invoke onCanvasClick — we own the click-
      // vs-drag decision here.
      marqueeConsumedClickRef.current = true;
      if (!movedPastSlop) {
        // Treat as a plain canvas click — preserve the "click empty
        // area clears selection" UX (existing behaviour). The
        // backdrop's onClick handler would normally fire this; we do
        // it explicitly here because pointer-down on the backdrop
        // started a marquee and the click event won't reliably reach
        // the backdrop onClick after a captured pointer-up.
        onCanvasClick?.(event as unknown as React.MouseEvent);
        return;
      }
      // Compute inclusive rectangle bounds in viewBox space.
      const xMin = Math.min(startX, currX);
      const xMax = Math.max(startX, currX);
      const yMin = Math.min(startY, currY);
      const yMax = Math.max(startY, currY);
      // Hit test: a key is selected when its projected (x, y)
      // satisfies xMin ≤ x ≤ xMax AND yMin ≤ y ≤ yMax (INCLUSIVE on
      // both ends — a key exactly on the edge is selected). The
      // points list was already projected at render time; reuse those
      // numbers instead of re-projecting.
      const hits: number[] = [];
      for (const p of points) {
        if (p.x >= xMin && p.x <= xMax && p.y >= yMin && p.y <= yMax) {
          hits.push(p.time);
        }
      }
      onCanvasMarqueeSelect?.(hits, shift);
      return;
    }
    // ── Key-drag branch (original behaviour) ──────────────────────
    const drag = dragRef.current;
    if (drag === null) return;
    if (event.pointerId !== drag.pointerId) return;
    const { keyTime, currentTime, currentValue, moved, target } = drag;
    dragRef.current = null;
    // Release pointer capture if held.
    if (target !== null) {
      const el = target as Element & { releasePointerCapture?: (id: number) => void };
      if (typeof el.releasePointerCapture === "function") {
        try {
          el.releasePointerCapture(event.pointerId);
        } catch {
          /* see startDrag — swallow */
        }
      }
    }
    setDragTick((n) => n + 1);
    if (moved && onKeyDragEnd) {
      onKeyDragEnd(keyTime, currentTime, currentValue);
    } else if (!moved && onKeyClick) {
      // No movement — treat as a plain click on the key. We
      // synthesise the React event object enough for the modifier
      // checks the parent does (ctrlKey/metaKey).
      onKeyClick(keyTime, event);
    }
  };

  /** Pointer cancel — drop the drag OR marquee without committing. */
  const onPointerCancel = (event: ReactPointerEvent<SVGSVGElement>) => {
    if (marquee !== null && event.pointerId === marquee.pointerId) {
      setMarquee(null);
      return;
    }
    const drag = dragRef.current;
    if (drag === null) return;
    if (event.pointerId !== drag.pointerId) return;
    dragRef.current = null;
    setDragTick((n) => n + 1);
  };

  // Esc-cancel for marquee. Window-level keydown listener is mounted
  // ONLY while a marquee is active so we don't leak handlers when
  // idle. The marquee state clears without firing the callback —
  // selection is left untouched.
  useEffect(() => {
    if (marquee === null) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        setMarquee(null);
      }
    };
    window.addEventListener("keydown", handler);
    return () => { window.removeEventListener("keydown", handler); };
  }, [marquee]);

  /** Pointer-down on the canvas backdrop (empty area). In Insert
   *  mode, computes (time, value) and fires `onCanvasAdd`. In
   *  Select mode (), starts a marquee — pointer-move grows the
   *  rectangle; pointer-up commits the selection or, if no drag past
   *  slop, fires the click path (clears selection). */
  const onCanvasPointerDown = (event: ReactPointerEvent<SVGRectElement>) => {
    if (event.button !== 0) return;
    const svg = event.currentTarget.ownerSVGElement;
    if (svg === null) return;
    if (insertMode) {
      if (!onCanvasAdd) return;
      const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const time = unproject(x, timeMin, timeMax, width);
      const value = unproject(height - y, vMin, vMax, height);
      event.stopPropagation();
      onCanvasAdd(time, value);
      return;
    }
    // ── Select mode — start a marquee. Even when no marquee callback
    // is wired we still capture the pointer so a pointer-up with no
    // movement past slop can fire onCanvasClick at the right moment
    // (rather than letting the backdrop's onClick race with the
    // captured pointer-up).
    const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    const t = event.currentTarget;
    if (typeof t.setPointerCapture === "function") {
      try { t.setPointerCapture(event.pointerId); } catch { /* swallow */ }
    }
    event.stopPropagation();
    setMarquee({
      startX: x,
      startY: y,
      currX: x,
      currY: y,
      clientStartX: event.clientX,
      clientStartY: event.clientY,
      shift: event.shiftKey,
      pointerId: event.pointerId,
      target: t,
      movedPastSlop: false,
    });
  };

  // Build the render-time points list, overriding the dragged key's
  // position so the circle + curve track the cursor mid-drag. The
  // dragged point's `time` stays at its original value for the
  // selection match; only the display position shifts.
  const drag = dragRef.current;
  const renderPoints = points.map((p) => {
    if (drag !== null && p.time === drag.keyTime) {
      const dx = project(drag.currentTime, timeMin, timeMax, width);
      const dy = height - project(drag.currentValue, vMin, vMax, height);
      return { ...p, x: dx, y: dy };
    }
    return p;
  });

  return (
    <svg
      data-testid="curve-editor-svg"
      data-track={track.name}
      data-key-count={track.keys.length}
      data-interpolation={interp}
      data-insert-mode={insertMode ? "true" : "false"}
      data-dragging={drag !== null ? "true" : "false"}
      role="img"
      aria-label={`${track.name} curve, ${track.keys.length} keys`}
      viewBox={`0 0 ${width} ${height}`}
      preserveAspectRatio="none"
      className="h-full w-full select-none"
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerCancel}
      onClick={(e) => {
        // Click handler fires on the SVG itself — child clicks
        // (circles) bubble up too. We only want to treat THIS event
        // as a canvas click when the actual target is the SVG (i.e.
        // not a key circle). Child click handlers stopPropagation so
        // they never reach here.
        if (e.target !== e.currentTarget) return;
        // (Group D follow-up): in Insert mode, pointer-down on
        // the backdrop fires onCanvasAdd. If the bridge response
        // renders a new key circle before pointer-up, the synthetic
        // click event's target becomes the SVG (LCA of backdrop-down
        // and circle-up), and without this guard onCanvasClick would
        // fire → clear the selection we just established. Mirrors
        // the same guard on the backdrop's onClick below.
        if (insertMode) return;
        onCanvasClick?.(e);
      }}
    >
      {/* Background rect so empty-area clicks are reliably caught by
          the SVG's own onClick (target === SVG). Some browsers route
          clicks through the topmost SVG child even when there's no
          shape under the cursor; an explicit transparent backdrop
          fixes that consistently. */}
      <rect
        data-testid="curve-canvas-backdrop"
        x={0}
        y={0}
        width={width}
        height={height}
        fill="transparent"
        onPointerDown={onCanvasPointerDown}
        onContextMenu={(e) => {
          // (Group D follow-up): right-click on empty canvas
          // drops the parent back to Select mode. preventDefault to
          // suppress the browser's native context menu, since we
          // own the gesture.
          if (onCanvasContextMenu) {
            e.preventDefault();
            e.stopPropagation();
            onCanvasContextMenu();
          }
        }}
        onClick={(e) => {
          e.stopPropagation();
          // In Insert mode the pointer-down branch already fired
          // onCanvasAdd — don't double-fire onCanvasClick here.
          if (insertMode) return;
          // After a Select-mode pointer-up the marquee branch already
          // decided whether to fire onCanvasClick. Skip the
          // synthetic-click double-fire by consuming the ref flag.
          if (marqueeConsumedClickRef.current) {
            marqueeConsumedClickRef.current = false;
            return;
          }
          onCanvasClick?.(e);
        }}
        style={{ cursor: insertMode ? "crosshair" : undefined }}
      />

      {/* Grid */}
      <g data-testid="curve-grid" stroke="var(--curve-grid)" strokeWidth={1} pointerEvents="none">
        {verticalLines.map((x, i) => (
          <line key={`v${i}`} x1={x} y1={0} x2={x} y2={height} />
        ))}
        {horizontalLines.map((y, i) => (
          <line key={`h${i}`} x1={0} y1={y} x2={width} y2={y} />
        ))}
      </g>

      {/* Outer axes (left + bottom darker for orientation) */}
      <g data-testid="curve-axes" stroke="var(--curve-axis)" strokeWidth={1.5} pointerEvents="none">
        <line x1={0} y1={0} x2={0} y2={height} />
        <line x1={0} y1={height} x2={width} y2={height} />
      </g>

      {/* The curve. Hidden when there are <2 keys (a single point
          doesn't form a segment). Smooth → cubic-Bezier <path>;
          step → staircase <polyline>; linear → straight-line
          <polyline>. Track-specific colouring (red curve in red,
          etc.) is a future polish pass. */}
      {renderPoints.length >= 2 && interp === "smooth" && (
        <path
          data-testid="curve-path"
          fill="none"
          stroke="#e5e5e5"
          strokeWidth={1.5}
          d={buildSmoothPath(renderPoints)}
          pointerEvents="none"
        />
      )}
      {renderPoints.length >= 2 && interp === "step" && (
        <polyline
          data-testid="curve-polyline"
          data-interpolation="step"
          fill="none"
          stroke="#e5e5e5"
          strokeWidth={1.5}
          points={buildStepPolyline(renderPoints)}
          pointerEvents="none"
        />
      )}
      {renderPoints.length >= 2 && interp === "linear" && (
        <polyline
          data-testid="curve-polyline"
          data-interpolation="linear"
          fill="none"
          stroke="#e5e5e5"
          strokeWidth={1.5}
          points={renderPoints.map((p) => `${p.x},${p.y}`).join(" ")}
          pointerEvents="none"
        />
      )}

      {/* Per-key circles. r=4 (unselected) / r=5 (selected) — the
          enlarged hit target also makes the selected key
          visually pop. Border keys (first + last by time) carry an
          accent stroke ring + darker un-selected fill so they read
          as "anchor" points distinct from interior keys. Pointer
          handlers route through onPointerDown (drag) + the SVG-
          level onPointerMove/Up so the drag math works even when
          the cursor leaves the circle. */}
      {renderPoints.map((p, i) => {
        const selected = selectedKeyTimes?.has(p.time) ?? false;
        const isBorder = borderTimes.has(p.time);
        // Selected keys keep their OWN colour (interior grey / border
        // slate). The selection treatment — a saturated, larger marker
        // with a stronger shadow — lives in the
        // `.curve-key-marker[data-selected="true"]` CSS rule + the
        // enlarged `r` below, replacing the old flat-blue fill override.
        const fill = isBorder ? BORDER_FILL : UNSELECTED_FILL;
        const stroke = isBorder ? BORDER_STROKE : "none";
        const strokeWidth = isBorder ? 1.5 : 0;
        return (
          <circle
            key={i}
            className="curve-key-marker"
            data-testid="curve-key"
            data-key-time={p.time}
            data-selected={selected ? "true" : "false"}
            data-border={isBorder ? "true" : "false"}
            cx={p.x}
            cy={p.y}
            r={selected ? 6 : 4}
            fill={fill}
            stroke={stroke}
            strokeWidth={strokeWidth}
            style={{ cursor: onKeyClick ? "pointer" : undefined }}
            onPointerDown={(e) => startDrag(e, p.time, p.value)}
            onContextMenu={(e) => {
              if (!onKeyContextMenu) return;
              e.preventDefault();
              e.stopPropagation();
              onKeyContextMenu(p.time, isBorder, e.clientX, e.clientY);
            }}
            onClick={(e) => {
              // The pointer-up handler on the SVG handles drag-end +
              // click-vs-drag detection. The plain click handler is
              // still wired so that environments without pointer
              // events (older test harnesses firing fireEvent.click
              // directly) keep working. To avoid double-firing in
              // pointer-event environments we only invoke the click
              // path here when no drag was just active.
              e.stopPropagation();
              if (dragRef.current === null) {
                onKeyClick?.(p.time, e);
              }
            }}
          />
        );
      })}

      {/* Marquee rectangle (). Rendered last so it draws over the
          keys. `pointerEvents="none"` keeps the captured backdrop in
          control of the gesture — the rect is purely visual. The
          fill colour follows the design lock: sky-500 at 15% opacity
          with a dashed sky-500 border. */}
      {marquee !== null && marquee.movedPastSlop && (
        <rect
          data-testid="curve-marquee"
          x={Math.min(marquee.startX, marquee.currX)}
          y={Math.min(marquee.startY, marquee.currY)}
          width={Math.abs(marquee.currX - marquee.startX)}
          height={Math.abs(marquee.currY - marquee.startY)}
          fill="rgb(14 165 233 / 0.15)"
          stroke="#0EA5E9"
          strokeDasharray="4 4"
          strokeWidth={1}
          pointerEvents="none"
        />
      )}
    </svg>
  );
}

// ─── Multi-channel overlay (Task 2.6 + hybrid focus-channel restore) ──
//
// Renders a single SVG with one layer per visible channel. Two modes:
//
//   1. View-only (focusChannel undefined) — every visible channel
//      renders the same way: curve line + small unstyled circles, all
//      `pointerEvents=none`. Matches the Task 2.6 behaviour.
//
//   2. Hybrid focus-channel (focusChannel set) — non-focus visible
//      channels render dimmed (opacity 0.4, no markers, no pointer
//      events). The focus channel renders emphasised: full-opacity
//      curve with thicker stroke, key circles (selectable, draggable),
//      and the SVG's pointer/click/context-menu handlers route to the
//      focus channel's keys. Marquee, Insert mode, and the per-key
//      right-click menu work the same as the single-track branch — the
//      interactive scaffolding is inlined here so the panel doesn't
//      need to switch render trees when toggling focus.

type MultiProps = {
  tracks: TrackDto[] | null;
  channels: readonly ChannelDef[];
  visibleChannels: Record<string, boolean>;
  focusChannel?: string;
  /** Unified Y-axis range across all visible channels. When set, the
   *  renderer projects every channel into this single space and uses
   *  it for pointer↔value conversions (drag, insert, marquee). When
   *  omitted, each channel falls back to its own per-track range
   *  (legacy behaviour). The drag VALUE-clamp uses the focus
   *  channel's own range regardless, so engine bounds aren't
   *  violated even when the visible canvas extends past them. */
  displayRange?: { min: number; max: number };
  width: number;
  height: number;
  timeMin: number;
  timeMax: number;
  // Forwarded interactive handlers — only used in focus mode.
  selectedKeyTimes?: ReadonlySet<number>;
  onKeyClick?: (time: number, event: React.MouseEvent | React.PointerEvent) => void;
  onCanvasClick?: (event: React.MouseEvent) => void;
  insertMode?: boolean;
  onCanvasAdd?: (time: number, value: number) => void;
  onCanvasContextMenu?: () => void;
  onKeyContextMenu?: (
    time: number,
    isBorder: boolean,
    clientX: number,
    clientY: number,
  ) => void;
  onKeyDragEnd?: (keyTime: number, newTime: number, newValue: number) => void;
  onKeyDragStart?: (keyTime: number) => void;
  onKeyDragMove?: (keyTime: number, currentTime: number, currentValue: number) => void;
  onKeyDragCancel?: () => void;
  /** an-audit-finding: a drag of one key within a multi-selection shifts the whole
   *  selection by (dTime, dValue). The parent applies it via applyGroupShift. */
  onGroupDragEnd?: (dTime: number, dValue: number) => void;
  /** Group-drag live move — fires every move past slop with the live
   *  (dTime, dValue) so the parent can live-update the average spinners. */
  onGroupDragMove?: (dTime: number, dValue: number) => void;
  onCanvasMarqueeSelect?: (times: number[], shift: boolean) => void;
  marqueeRef?: Ref<CurveMarqueeHandle>;
};

function MultiChannelCurves({
  tracks,
  channels,
  visibleChannels,
  focusChannel,
  displayRange,
  width: propWidth,
  height: propHeight,
  timeMin,
  timeMax,
  selectedKeyTimes,
  onKeyClick,
  onCanvasClick,
  insertMode,
  onCanvasAdd,
  onCanvasContextMenu,
  onKeyContextMenu,
  onKeyDragEnd,
  onKeyDragStart,
  onKeyDragMove,
  onKeyDragCancel,
  onGroupDragEnd,
  onGroupDragMove,
  onCanvasMarqueeSelect,
  marqueeRef,
}: MultiProps) {
  // Live-measured SVG dimensions. We can't simply pass a fixed 600×300
  // viewBox to a stretchy SVG (`preserveAspectRatio="none"`) without
  // distorting circles into ellipses and giving gridline strokes
  // non-uniform thickness — at a 2400×200 cell the 4× X / 0.65× Y
  // stretch is glaringly visible. The fix is to match viewBox to the
  // actual rendered CSS dimensions; that makes one viewBox unit equal
  // one CSS pixel, so strokes / radii are isotropic. Measurement runs
  // in a layout effect (synchronous post-DOM, pre-paint) so the user
  // never sees the default-size first frame. In jsdom (tests) the
  // ResizeObserver stub is a no-op and `getBoundingClientRect` returns
  // zeros, so the measurement is rejected and the prop fallback is
  // used — keeping the existing 600×300 test deterministic.
  const svgRef = useRef<SVGSVGElement>(null);
  const [measured, setMeasured] = useState<{ width: number; height: number }>(
    { width: propWidth, height: propHeight },
  );
  useLayoutEffect(() => {
    const el = svgRef.current;
    if (el === null) return;
    const update = () => {
      const rect = el.getBoundingClientRect();
      if (rect.width > 0 && rect.height > 0) {
        setMeasured((prev) =>
          prev.width === rect.width && prev.height === rect.height
            ? prev
            : { width: rect.width, height: rect.height },
        );
      }
    };
    update();
    const observer = new ResizeObserver(update);
    observer.observe(el);
    return () => observer.disconnect();
  }, []);
  const width = measured.width;
  const height = measured.height;

  // Grid: same layout as the single-track renderer so the visual
  // matches the design lock. 10 evenly-spaced cells per axis.
  const gridCells = 10;
  const verticalLines: number[] = [];
  for (let i = 0; i <= gridCells; i++) {
    verticalLines.push((i / gridCells) * width);
  }
  const horizontalLines: number[] = [];
  for (let i = 0; i <= gridCells; i++) {
    horizontalLines.push((i / gridCells) * height);
  }

  // For each visible channel, find the track by name + project its
  // keys. Tracks may be null (no emitter selected) — render the grid
  // only. When the parent supplies `displayRange` every channel
  // projects into the SAME Y space (so curves squish/stretch
  // together as the union of their ranges); when omitted each
  // channel falls back to its own per-track range (legacy
  // per-channel scaling). The per-channel `range` we keep on each
  // layer is still the channel's OWN engine-allowed range — the
  // drag value-clamp downstream reads it from `focusLayer.range`.
  const layers = (tracks ?? []).flatMap((t) => {
    const channel = channels.find((c) => c.trackName === t.name);
    if (channel === undefined) return [];
    if (!(visibleChannels[channel.id] ?? channel.defaultOn)) return [];
    const range = valueRangeForTrack(t);
    const projY = displayRange ?? range;
    const points = t.keys.map((k) => ({
      x: project(k.time, timeMin, timeMax, width),
      y: height - project(k.value, projY.min, projY.max, height),
      time: k.time,
      value: k.value,
    }));
    return [{ channel, track: t, points, range }];
  });

  // Locate the focus layer (when a focusChannel is set). Even when a
  // focus channel is set but its track isn't in `layers` (e.g. it's
  // hidden via the checkbox — we don't auto-show), focusLayer is null
  // and we render in view-only mode.
  const focusLayer = focusChannel === undefined
    ? null
    : (layers.find((l) => l.channel.id === focusChannel) ?? null);
  const focusEnabled = focusLayer !== null;
  // Read-only mirror: the focus channel is locked to another channel.
  // Derived from the DTO (lockedTo) — no prop threaded from the panel.
  const focusReadOnly = focusLayer !== null && focusLayer.track.lockedTo != null;

  // ── Drag state. Held in refs so pointer-move handlers don't trigger
  // a re-render on every pixel; setDragTick flushes a render when we
  // need the dragged circle to track the cursor.
  const dragRef = useRef<{
    keyTime: number;
    startTime: number;
    startValue: number;
    startClientX: number;
    startClientY: number;
    currentTime: number;
    currentValue: number;
    moved: boolean;
    pointerId: number;
    target: Element | null;
    // an-audit-finding group-drag: when the grabbed key is part of a multi-selection,
    // the whole selection shifts by (groupDTime, groupDValue).
    isGroup: boolean;
    groupDTime: number;
    groupDValue: number;
  } | null>(null);
  const [, setDragTick] = useState(0);

  // ── Morph animation. morphSuppressRef stays null this task (Task 4
  // wires the recording at the drag-commit site). dragRef must be
  // declared before this hook so the isDragging closure captures it.
  const morphSuppressRef = useRef<SuppressedMove>(null);
  const morph = useCurveMorph({
    channels: layers.map((l) => ({
      channelId: l.channel.id,
      color: l.channel.color,
      track: l.track,
      vMin: (displayRange ?? l.range).min,
      vMax: (displayRange ?? l.range).max,
      dashed: focusReadOnly && focusLayer !== null && l.channel.id === focusLayer.channel.id,
      strokeWidth: focusEnabled && focusLayer !== null && l.channel.id === focusLayer.channel.id ? 3 : 2,
      opacity: focusEnabled && (focusLayer === null || l.channel.id !== focusLayer.channel.id) ? 0.4 : 1,
      isFocus: focusLayer !== null && l.channel.id === focusLayer.channel.id,
    })),
    width,
    height,
    timeMin,
    timeMax,
    isDragging: () => dragRef.current !== null,
    suppressRef: morphSuppressRef,
  });

  // ── Marquee state (mirrors the single-track branch).
  type MarqueeState = {
    startX: number;
    startY: number;
    currX: number;
    currY: number;
    clientStartX: number;
    clientStartY: number;
    shift: boolean;
    pointerId: number;
    target: Element | null;
    movedPastSlop: boolean;
  };
  const [marquee, setMarquee] = useState<MarqueeState | null>(null);
  const marqueeConsumedClickRef = useRef(false);
  // After a key drag commits, the browser still fires a synthetic
  // `click` event whose target is determined by the document
  // hit-test at pointer-up location — pointer capture redirects
  // `pointer*` events but NOT the click. If the drag ended over the
  // canvas backdrop, the backdrop's onClick would run onCanvasClick
  // and wipe the selection we just established in handleKeyDragEnd.
  // This ref is set true on drag-end and consumed once by the
  // backdrop's click handler so the deselect is suppressed for that
  // single trailing click.
  const dragConsumedClickRef = useRef(false);

  // Border keys on the focus track. First + last by time order.
  const focusBorderTimes = new Set<number>();
  if (focusLayer !== null && focusLayer.track.keys.length > 0) {
    const ks = focusLayer.track.keys;
    focusBorderTimes.add(ks[0]!.time);
    focusBorderTimes.add(ks[ks.length - 1]!.time);
  }

  const focusRange = focusLayer?.range ?? { min: 0, max: 1 };
  // The focus channel's engine-allowed bounds — used for the drag
  // value-clamp so a drag can never push the focused key past what
  // the engine accepts (e.g. red stays in [0, 1] even when the
  // canvas extends to 0..20 because Scale is also visible).
  const focusVMin = focusRange.min;
  const focusVMax = focusRange.max;
  // Visual Y space the canvas paints in — the unified display range
  // when supplied, the focus channel's range otherwise. Pointer↔value
  // conversions for drag, insert, marquee, and drag-preview projection
  // all use this so "where the user clicks" matches "where the curve
  // is drawn".
  const canvasRange = displayRange ?? focusRange;
  const canvasVMin = canvasRange.min;
  const canvasVMax = canvasRange.max;

  /** Begin a drag on a focus-channel key. */
  const startDrag = (
    event: ReactPointerEvent<SVGCircleElement>,
    keyTime: number,
    keyValue: number,
  ) => {
    if (!focusEnabled) return;
    if (focusReadOnly) return;
    if (event.button !== 0) return;
    if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
    // Group drag when the grabbed key is one of several selected keys —
    // captured from the current selection BEFORE onKeyDragStart (which
    // would otherwise collapse it).
    const isGroup =
      (selectedKeyTimes?.size ?? 0) > 1 && (selectedKeyTimes?.has(keyTime) ?? false);
    dragRef.current = {
      keyTime,
      startTime: keyTime,
      startValue: keyValue,
      startClientX: event.clientX,
      startClientY: event.clientY,
      currentTime: keyTime,
      currentValue: keyValue,
      moved: false,
      pointerId: event.pointerId,
      target: event.currentTarget,
      isGroup,
      groupDTime: 0,
      groupDValue: 0,
    };
    const t = event.currentTarget;
    if (typeof t.setPointerCapture === "function") {
      try { t.setPointerCapture(event.pointerId); } catch { /* swallow */ }
    }
    // Pre-select the key so it paints with the selected ring the
    // moment the user grabs it — without this, a gesture that turns
    // into a drag (rather than a click) never lands on `onKeyClick`,
    // so the key would stay unselected throughout the drag and only
    // become selected on pointer-up via `onKeyDragEnd`.
    onKeyDragStart?.(keyTime);
  };

  /** Pointer-move during a drag OR marquee. */
  const onPointerMove = (event: ReactPointerEvent<SVGSVGElement>) => {
    // ── Marquee branch
    if (dragRef.current === null && marquee !== null
        && event.pointerId === marquee.pointerId) {
      const svg = event.currentTarget;
      const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const dx = event.clientX - marquee.clientStartX;
      const dy = event.clientY - marquee.clientStartY;
      const movedNow = Math.abs(dx) > DRAG_SLOP || Math.abs(dy) > DRAG_SLOP;
      setMarquee({
        ...marquee,
        currX: x,
        currY: y,
        movedPastSlop: marquee.movedPastSlop || movedNow,
      });
      return;
    }
    // ── Key-drag branch
    const drag = dragRef.current;
    if (drag === null || focusLayer === null) return;
    if (event.pointerId !== drag.pointerId) return;
    const svg = event.currentTarget;
    const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    // ── Group-drag branch (an-audit-finding): shift the whole selection by the
    // grabbed key's cursor delta. Border selected keys stay fixed in time;
    // the time-shift is clamped so interior selected keys never cross the
    // global endpoints. The per-key value clamp happens on commit
    // (applyGroupShift) and in the render preview below.
    if (drag.isGroup && selectedKeyTimes) {
      const rawTime = unproject(x, timeMin, timeMax, width);
      const rawValue = Math.max(
        focusVMin,
        Math.min(focusVMax, unproject(height - y, canvasVMin, canvasVMax, height)),
      );
      let dTime = rawTime - drag.startTime;
      const allKeys = focusLayer.track.keys;
      const firstT = allKeys[0]!.time;
      const lastT = allKeys[allKeys.length - 1]!.time;
      const eps = 1e-4;
      let minSel = Infinity;
      let maxSel = -Infinity;
      for (const t of selectedKeyTimes) {
        if (focusBorderTimes.has(t)) continue;
        if (t < minSel) minSel = t;
        if (t > maxSel) maxSel = t;
      }
      if (minSel === Infinity) {
        dTime = 0; // all-border selection — no time shift
      } else {
        dTime = Math.max((firstT + eps) - minSel, Math.min((lastT - eps) - maxSel, dTime));
      }
      drag.groupDTime = dTime;
      drag.groupDValue = rawValue - drag.startValue;
      const gdx = event.clientX - drag.startClientX;
      const gdy = event.clientY - drag.startClientY;
      if (Math.abs(gdx) > DRAG_SLOP || Math.abs(gdy) > DRAG_SLOP) drag.moved = true;
      // Live-update the average spinners (group analogue of the single-key
      // onKeyDragMove fired below) — only once past slop.
      if (drag.moved) {
        onGroupDragMove?.(drag.groupDTime, drag.groupDValue);
      }
      setDragTick((n) => n + 1);
      return;
    }
    let nextTime = unproject(x, timeMin, timeMax, width);
    // Pointer Y maps through the CANVAS range (so the cursor follows
    // the curve visually); the value is then clamped to the focus
    // channel's engine bounds below so the commit stays legal.
    let nextValue = unproject(height - y, canvasVMin, canvasVMax, height);
    const isBorder = focusBorderTimes.has(drag.startTime);
    if (isBorder) {
      nextTime = drag.startTime;
    } else {
      const keys = focusLayer.track.keys;
      const idx = keys.findIndex((k) => k.time === drag.startTime);
      if (idx > 0 && idx < keys.length - 1) {
        const prevT = keys[idx - 1]!.time;
        const nextT = keys[idx + 1]!.time;
        const eps = 1e-4;
        nextTime = Math.max(prevT + eps, Math.min(nextT - eps, nextTime));
      } else {
        nextTime = drag.startTime;
      }
    }
    nextValue = Math.max(focusVMin, Math.min(focusVMax, nextValue));
    drag.currentTime = nextTime;
    drag.currentValue = nextValue;
    const dx2 = event.clientX - drag.startClientX;
    const dy2 = event.clientY - drag.startClientY;
    if (Math.abs(dx2) > DRAG_SLOP || Math.abs(dy2) > DRAG_SLOP) {
      drag.moved = true;
    }
    // Fire the live-drag callback only once we're past the slop
    // threshold — jittery clicks shouldn't ripple a "drag move"
    // upward to the spinner panel.
    if (drag.moved) {
      onKeyDragMove?.(drag.keyTime, drag.currentTime, drag.currentValue);
    }
    setDragTick((n) => n + 1);
  };

  /** Pointer-up — commit drag, commit marquee, or treat as click. */
  const onPointerUp = (event: ReactPointerEvent<SVGSVGElement>) => {
    // Marquee branch
    if (dragRef.current === null && marquee !== null
        && event.pointerId === marquee.pointerId) {
      const { startX, startY, currX, currY, shift, movedPastSlop, target } = marquee;
      if (target !== null) {
        const el = target as Element & { releasePointerCapture?: (id: number) => void };
        if (typeof el.releasePointerCapture === "function") {
          try { el.releasePointerCapture(event.pointerId); } catch { /* swallow */ }
        }
      }
      setMarquee(null);
      marqueeConsumedClickRef.current = true;
      if (!movedPastSlop) {
        onCanvasClick?.(event as unknown as React.MouseEvent);
        return;
      }
      if (focusLayer === null) return;
      const xMin = Math.min(startX, currX);
      const xMax = Math.max(startX, currX);
      const yMin = Math.min(startY, currY);
      const yMax = Math.max(startY, currY);
      const hits: number[] = [];
      for (const p of focusLayer.points) {
        if (p.x >= xMin && p.x <= xMax && p.y >= yMin && p.y <= yMax) {
          hits.push(p.time);
        }
      }
      onCanvasMarqueeSelect?.(hits, shift);
      return;
    }
    // Key-drag branch
    const drag = dragRef.current;
    if (drag === null) return;
    if (event.pointerId !== drag.pointerId) return;
    const { keyTime, currentTime, currentValue, moved, target, isGroup, groupDTime, groupDValue } = drag;
    dragRef.current = null;
    if (target !== null) {
      const el = target as Element & { releasePointerCapture?: (id: number) => void };
      if (typeof el.releasePointerCapture === "function") {
        try { el.releasePointerCapture(event.pointerId); } catch { /* swallow */ }
      }
    }
    setDragTick((n) => n + 1);
    if (isGroup) {
      // an-audit-finding: commit the whole-selection shift (or treat a no-move as a
      // plain key click so it falls back to single-select).
      if (moved && onGroupDragEnd) {
        dragConsumedClickRef.current = true;
        // Record suppression BEFORE firing the callback so the hook
        // sees it when the parent re-renders with the committed tracks.
        if (focusLayer !== null && selectedKeyTimes) {
          const eps = 1e-4;
          const allKeys = focusLayer.track.keys;
          const suppMoves: Array<{ oldTime: number; newTime: number; newValue: number }> = [];
          for (const k of allKeys) {
            if (!selectedKeyTimes.has(k.time)) continue;
            const isBorder = focusBorderTimes.has(k.time);
            // NOTE: newTime/newValue must match the values committed by
            // applyGroupShift in CurveEditorPanel.tsx (~:883-884) within
            // KEY_MATCH_EPS; movesMatch() in use-curve-morph.ts uses them
            // to verify the incoming track change. Keep in sync.
            const newTime = isBorder
              ? k.time
              : Math.max(timeMin + eps, Math.min(timeMax - eps, k.time + groupDTime));
            const newValue = Math.max(focusVMin, Math.min(focusVMax, k.value + groupDValue));
            suppMoves.push({ oldTime: k.time, newTime, newValue });
          }
          if (suppMoves.length > 0) {
            morphSuppressRef.current = {
              channelId: focusLayer.channel.id,
              moves: suppMoves,
            };
          }
        }
        onGroupDragEnd(groupDTime, groupDValue);
      } else if (!moved && onKeyClick) {
        onKeyClick(keyTime, event);
      }
    } else if (moved && onKeyDragEnd) {
      // Suppress the trailing synthetic click — see
      // `dragConsumedClickRef`'s comment. Without this the backdrop
      // would clear the selection we set in handleKeyDragEnd.
      dragConsumedClickRef.current = true;
      // Record suppression BEFORE firing the callback so the hook
      // sees it when the parent re-renders with the committed tracks.
      if (focusLayer !== null) {
        // NOTE: newTime/newValue here must equal the committed key values
        // within KEY_MATCH_EPS; movesMatch() in use-curve-morph.ts uses
        // them to verify the incoming track change. Keep in sync with the
        // group-clamp in CurveEditorPanel.tsx applyGroupShift (~:883-884).
        morphSuppressRef.current = {
          channelId: focusLayer.channel.id,
          moves: [{ oldTime: keyTime, newTime: currentTime, newValue: currentValue }],
        };
      }
      onKeyDragEnd(keyTime, currentTime, currentValue);
    } else if (!moved && onKeyClick) {
      onKeyClick(keyTime, event);
    }
  };

  const onPointerCancel = (event: ReactPointerEvent<SVGSVGElement>) => {
    if (marquee !== null && event.pointerId === marquee.pointerId) {
      setMarquee(null);
      return;
    }
    const drag = dragRef.current;
    if (drag === null) return;
    if (event.pointerId !== drag.pointerId) return;
    dragRef.current = null;
    setDragTick((n) => n + 1);
    // Notify the parent so it can roll back any live-drag state
    // (Time / Value spinner overlay) that came from `onKeyDragMove`.
    onKeyDragCancel?.();
  };

  // Esc cancels an active marquee.
  useEffect(() => {
    if (marquee === null) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        e.preventDefault();
        setMarquee(null);
      }
    };
    window.addEventListener("keydown", handler);
    return () => { window.removeEventListener("keydown", handler); };
  }, [marquee]);

  // CRV: imperative entry so a marquee can BEGIN from outside this SVG (the
  // axis-label gutters). Maps the client point into viewBox space, CLAMPS it
  // to the plot edges (a left-gutter start anchors at time 0; a bottom-gutter
  // start at value min), seeds the marquee, and captures the pointer to this
  // SVG so the existing onPointerMove/Up/Cancel + Esc machinery drives the
  // rest. Cross-element capture is valid — the pointer is active from the
  // gutter pointerdown. No-op in the read-only (non-focus) overlay.
  const startMarquee = (clientX: number, clientY: number, shiftKey: boolean, pointerId: number) => {
    if (!focusEnabled) return;
    if (focusReadOnly) return;
    const svg = svgRef.current;
    if (svg === null) return;
    const { x, y } = eventToViewBox(svg, clientX, clientY, width, height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    if (typeof svg.setPointerCapture === "function") {
      try { svg.setPointerCapture(pointerId); } catch { /* swallow — capture is best-effort */ }
    }
    // Begin the marquee AT the press point, even when it's in a gutter
    // (x < 0 / y > height). The SVG's overflow="visible" renders the rectangle
    // into the margin, so it visibly starts where the user pressed — NOT
    // snapped to the plot edge. The inclusive hit-test still only matches
    // in-plot keys the rectangle covers, so an over-range origin is harmless.
    setMarquee({
      startX: x, startY: y, currX: x, currY: y,
      clientStartX: clientX, clientStartY: clientY,
      shift: shiftKey, pointerId, target: svg, movedPastSlop: false,
    });
  };
  useImperativeHandle(marqueeRef, () => ({ startMarquee }));

  const onCanvasPointerDown = (event: ReactPointerEvent<SVGRectElement>) => {
    if (!focusEnabled) return;
    if (event.button !== 0) return;
    // Read-only mirror: no pointer gestures — insert, marquee, and all
    // canvas-edit paths are suppressed. Plain clicks still reach the
    // backdrop's onClick → onCanvasClick (clear-selection UX preserved).
    if (focusReadOnly) return;
    const svg = event.currentTarget.ownerSVGElement;
    if (svg === null) return;
    if (insertMode) {
      if (!onCanvasAdd) return;
      const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
      if (!Number.isFinite(x) || !Number.isFinite(y)) return;
      const time = unproject(x, timeMin, timeMax, width);
      // Pointer Y → value through the CANVAS range. The host
      // (`emitters/add-track-key`) clamps to the channel's engine
      // bounds before committing.
      const value = unproject(height - y, canvasVMin, canvasVMax, height);
      event.stopPropagation();
      onCanvasAdd(time, value);
      return;
    }
    const { x, y } = eventToViewBox(svg, event.clientX, event.clientY, width, height);
    if (!Number.isFinite(x) || !Number.isFinite(y)) return;
    const t = event.currentTarget;
    if (typeof t.setPointerCapture === "function") {
      try { t.setPointerCapture(event.pointerId); } catch { /* swallow */ }
    }
    event.stopPropagation();
    setMarquee({
      startX: x,
      startY: y,
      currX: x,
      currY: y,
      clientStartX: event.clientX,
      clientStartY: event.clientY,
      shift: event.shiftKey,
      pointerId: event.pointerId,
      target: t,
      movedPastSlop: false,
    });
  };

  // Build the focus layer's render points, overriding the dragged
  // key's projected position so the circle tracks the cursor.
  const drag = dragRef.current;
  const focusRenderPoints = focusLayer === null ? [] : focusLayer.points.map((p) => {
    // an-audit-finding group-drag preview: every selected key shifts by the group
    // delta (border keys keep their time), clamped to the canvas bounds.
    if (drag !== null && drag.isGroup && (selectedKeyTimes?.has(p.time) ?? false)) {
      const isBorder = focusBorderTimes.has(p.time);
      const nt = isBorder
        ? p.time
        : Math.max(timeMin, Math.min(timeMax, p.time + drag.groupDTime));
      const nv = Math.max(focusVMin, Math.min(focusVMax, p.value + drag.groupDValue));
      return {
        ...p,
        x: project(nt, timeMin, timeMax, width),
        y: height - project(nv, canvasVMin, canvasVMax, height),
      };
    }
    if (drag !== null && !drag.isGroup && p.time === drag.keyTime) {
      const dx = project(drag.currentTime, timeMin, timeMax, width);
      // Drag preview projects the in-flight value through the CANVAS
      // range so the dragged circle tracks the cursor even when the
      // canvas extends beyond the focus channel's own range (e.g. red
      // key on a 0..20 canvas because Scale is also visible).
      const dy = height - project(drag.currentValue, canvasVMin, canvasVMax, height);
      return { ...p, x: dx, y: dy };
    }
    return p;
  });

  return (
    <svg
      ref={svgRef}
      data-testid="curve-editor-svg"
      data-multi-channel="true"
      data-visible-count={layers.length}
      data-focus-channel={focusChannel ?? ""}
      data-insert-mode={insertMode ? "true" : "false"}
      data-dragging={drag !== null ? "true" : "false"}
      role="img"
      aria-label={`Multi-channel curve overlay, ${layers.length} channels`}
      // viewBox is sized to the SVG's MEASURED CSS dimensions (see
      // the `useLayoutEffect` above), so one viewBox unit equals one
      // CSS pixel. This means `preserveAspectRatio` becomes a
      // no-op (any value works — viewBox already matches CSS dims
      // exactly), strokes draw at their declared CSS-pixel width,
      // and `r={5}` circles stay circular regardless of how the
      // cell stretches. The prior `preserveAspectRatio="none"` was
      // what made circles morph into ellipses and gridlines
      // thicken / thin along different axes at wide windows.
      viewBox={`0 0 ${width} ${height}`}
      // `block` removes the inline-baseline gap that a default-inline
      // <svg> would leave under it (descender space in the parent line
      // box), preventing a few-pixel vertical offset from the cell top.
      className="block h-full w-full select-none"
      // `overflow="visible"` lets the endpoint key circles at time=0,
      // time=100, value=min, value=max render their FULL body even
      // when their centre sits exactly on the grid edge. Without
      // this the SVG clips the half of the circle outside the
      // viewBox, making endpoint keys look bisected (half-moons
      // along the edges).
      overflow="visible"
      onPointerMove={focusEnabled ? onPointerMove : undefined}
      onPointerUp={focusEnabled ? onPointerUp : undefined}
      onPointerCancel={focusEnabled ? onPointerCancel : undefined}
      onClick={focusEnabled ? (e) => {
        if (e.target !== e.currentTarget) return;
        if (insertMode) return;
        // A gutter-initiated marquee captures THIS svg, so the synthetic
        // trailing click after the drag lands here (not on the backdrop).
        // Honour the marquee's click-suppression flag — otherwise the click
        // clears the selection the marquee just made (mirrors the backdrop).
        if (marqueeConsumedClickRef.current) {
          marqueeConsumedClickRef.current = false;
          return;
        }
        if (dragConsumedClickRef.current) {
          dragConsumedClickRef.current = false;
          return;
        }
        onCanvasClick?.(e);
      } : undefined}
    >
      {/* Backdrop for empty-canvas events (focus mode only — non-focus
          mode is view-only and doesn't need pointer routing). */}
      {focusEnabled && (
        <rect
          data-testid="curve-canvas-backdrop"
          x={0}
          y={0}
          width={width}
          height={height}
          fill="transparent"
          onPointerDown={onCanvasPointerDown}
          onContextMenu={(e) => {
            if (onCanvasContextMenu) {
              e.preventDefault();
              e.stopPropagation();
              onCanvasContextMenu();
            }
          }}
          onClick={(e) => {
            e.stopPropagation();
            if (insertMode) return;
            if (marqueeConsumedClickRef.current) {
              marqueeConsumedClickRef.current = false;
              return;
            }
            if (dragConsumedClickRef.current) {
              dragConsumedClickRef.current = false;
              return;
            }
            onCanvasClick?.(e);
          }}
          style={{ cursor: insertMode ? "crosshair" : undefined }}
        />
      )}

      {/* Grid — bounded to the canvas drawing area (0..width × 0..height).
          The grid does NOT shift with the focus channel's range —
          it's a fixed 10×10 reference. Per-channel value ranges
          are surfaced via the axis labels rendered below. */}
      <g data-testid="curve-grid" stroke="var(--curve-grid)" strokeWidth={1} pointerEvents="none">
        {verticalLines.map((x, i) => (
          <line key={`v${i}`} x1={x} y1={0} x2={x} y2={height} />
        ))}
        {horizontalLines.map((y, i) => (
          <line key={`h${i}`} x1={0} y1={y} x2={width} y2={y} />
        ))}
      </g>

      {/* Outer axes — left vertical + bottom horizontal form the
          "L" shape that bounds the grid. Axis tick labels render
          outside this box (in the SVG's margin area). */}
      <g data-testid="curve-axes" stroke="var(--curve-axis)" strokeWidth={1.5} pointerEvents="none">
        <line x1={0} y1={0} x2={0} y2={height} />
        <line x1={0} y1={height} x2={width} y2={height} />
      </g>

      {/* Background (non-focus) layers — dimmed lines + small
          non-interactive markers. In view-only mode (no focus
          channel) every layer renders the full-fidelity version
          with regular markers; in focus mode the markers shrink and
          drop their dark stroke so the focus layer's r=5 stroked
          circles stay visually primary. */}
      {layers.map(({ channel, track, points }) => {
        const isFocus = focusEnabled && focusLayer !== null && channel.id === focusLayer.channel.id;
        if (isFocus) return null; // focus layer rendered separately below
        const interp = track.interpolation;
        const layerOpacity = focusEnabled ? 0.4 : 1;
        const strokeW = 2;
        // Marker shape: in view-only mode, fully-styled circles
        // (matching Task 2.6 appearance). In focus mode, smaller
        // filled dots with no stroke — visible enough to read where
        // the control points sit on the dim curve, quiet enough that
        // the focus channel's keys remain the eye's target.
        const markerR = focusEnabled ? 3 : 4;
        const markerStroke = focusEnabled ? "none" : "#0a0a0a";
        const markerStrokeW = focusEnabled ? 0 : 1;
        // In focus mode the markers are non-interactive scenery — no
        // testid (the `curve-key` testid means "an interactive key
        // circle"), no key click handlers. View-only mode keeps the
        // legacy testid so callers can still query them.
        const markerTestId = focusEnabled ? undefined : "curve-key";
        return (
          <g
            key={channel.id}
            data-testid={`curve-layer-${channel.id}`}
            data-channel-id={channel.id}
            data-key-count={points.length}
            data-focus="false"
            style={{ opacity: layerOpacity, visibility: morph.isActive(channel.id) ? "hidden" : undefined }}
          >
            {points.length >= 2 && interp === "smooth" && (
              <path
                fill="none"
                stroke={channel.color}
                strokeWidth={strokeW}
                d={buildSmoothPath(points)}
                pointerEvents="none"
              />
            )}
            {points.length >= 2 && interp === "step" && (
              <polyline
                fill="none"
                stroke={channel.color}
                strokeWidth={strokeW}
                points={buildStepPolyline(points)}
                pointerEvents="none"
              />
            )}
            {points.length >= 2 && interp === "linear" && (
              <polyline
                fill="none"
                stroke={channel.color}
                strokeWidth={strokeW}
                points={points.map((p) => `${p.x},${p.y}`).join(" ")}
                pointerEvents="none"
              />
            )}
            {points.map((p, i) => (
              <circle
                key={i}
                {...(markerTestId !== undefined ? { "data-testid": markerTestId } : {})}
                data-channel-id={channel.id}
                data-key-time={p.time}
                cx={p.x}
                cy={p.y}
                r={markerR}
                fill={channel.color}
                stroke={markerStroke}
                strokeWidth={markerStrokeW}
                pointerEvents="none"
              />
            ))}
          </g>
        );
      })}

      {/* Focus layer — full opacity, thicker stroke, interactive
          circles. Rendered last so it draws above the dimmed
          background layers. A vertical gradient fill emanates from
          the curve down to the canvas floor (focus-only — keeping
          non-focus layers free of fills avoids stacked-translucent
          clutter when many channels are visible). */}
      {focusLayer !== null && (() => {
        const { channel, track } = focusLayer;
        const interp = track.interpolation;
        const fillGradId = `curve-fill-${channel.id}`;
        return (
          <g
            key={channel.id}
            data-testid={`curve-layer-${channel.id}`}
            data-channel-id={channel.id}
            data-key-count={focusRenderPoints.length}
            data-focus="true"
            data-readonly={focusReadOnly ? "true" : "false"}
            style={{ visibility: morph.isActive(channel.id) ? "hidden" : undefined }}
          >
            {/* Gradient definition — objectBoundingBox units mean the
                stops are positioned within the fill path's own bbox,
                so 0% lands at the top of the curve (highest point of
                the fill area) and 100% lands at the canvas floor.
                That produces the "colour emanating from the curve,
                fading to transparent" effect rather than a
                canvas-wide top-to-bottom wash. */}
            <defs>
              <linearGradient id={fillGradId} x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor={channel.color} stopOpacity="0.25" />
                <stop offset="100%" stopColor={channel.color} stopOpacity="0" />
              </linearGradient>
            </defs>
            {focusRenderPoints.length >= 2 && (
              <path
                data-testid="curve-fill"
                fill={`url(#${fillGradId})`}
                stroke="none"
                d={buildFillPath(focusRenderPoints, interp, height)}
                pointerEvents="none"
              />
            )}
            {focusRenderPoints.length >= 2 && interp === "smooth" && (
              <path
                data-testid="curve-path"
                fill="none"
                stroke={channel.color}
                strokeWidth={3}
                strokeDasharray={focusReadOnly ? READONLY_DASH : undefined}
                d={buildSmoothPath(focusRenderPoints)}
                pointerEvents="none"
              />
            )}
            {focusRenderPoints.length >= 2 && interp === "step" && (
              <polyline
                data-testid="curve-polyline"
                data-interpolation="step"
                fill="none"
                stroke={channel.color}
                strokeWidth={3}
                strokeDasharray={focusReadOnly ? READONLY_DASH : undefined}
                points={buildStepPolyline(focusRenderPoints)}
                pointerEvents="none"
              />
            )}
            {focusRenderPoints.length >= 2 && interp === "linear" && (
              <polyline
                data-testid="curve-polyline"
                data-interpolation="linear"
                fill="none"
                stroke={channel.color}
                strokeWidth={3}
                strokeDasharray={focusReadOnly ? READONLY_DASH : undefined}
                points={focusRenderPoints.map((p) => `${p.x},${p.y}`).join(" ")}
                pointerEvents="none"
              />
            )}
            {focusRenderPoints.map((p, i) => {
              const selected = selectedKeyTimes?.has(p.time) ?? false;
              const isBorder = focusBorderTimes.has(p.time);
              // Border keys (first / last in time order) used to render
              // with a slate fill + accent-blue stroke to signal "you
              // can't delete or drag-in-time this one". The visual
              // inconsistency (mid-curve red, edges blue) outweighed
              // the value — the same restrictions are still enforced
              // by the Delete handler + drag-time clamp, and the
              // `data-border` attribute below tags them for callers
              // that need the distinction programmatically.
              // Keys always render in their channel's colour; the
              // selected marker is intensified (saturate) + enlarged +
              // shadowed via CSS rather than recoloured to a flat blue.
              const fill = channel.color;
              const stroke = "none";
              const strokeWidth = 0;
              // Each focus key is rendered as a (hit-pad, visible)
              // pair: the hit pad is a transparent circle ~2× the
              // visible radius that owns every pointer handler +
              // data attribute (so test queries + click targets
              // land on it), and the visible circle on top is
              // decorative-only (`pointerEvents="none"`). This
              // makes keys comfortable to click without forcing a
              // larger visual marker that would clutter the curve.
              // hitR was bumped (10/12 → 14/16) because the old pad was
              // still fiddly to hit; visible dot stays at 5/6.
              const hitR = selected ? 18 : 14;
              const visR = selected ? 8 : 5;
              return (
                <g key={i}>
                  <circle
                    data-testid="curve-key"
                    data-channel-id={channel.id}
                    data-key-time={p.time}
                    data-selected={selected ? "true" : "false"}
                    data-border={isBorder ? "true" : "false"}
                    cx={p.x}
                    cy={p.y}
                    r={hitR}
                    fill="transparent"
                    stroke="transparent"
                    style={{ cursor: onKeyClick ? "pointer" : undefined }}
                    onPointerDown={(e) => startDrag(e, p.time, p.value)}
                    onContextMenu={(e) => {
                      if (focusReadOnly) return;
                      if (!onKeyContextMenu) return;
                      e.preventDefault();
                      e.stopPropagation();
                      onKeyContextMenu(p.time, isBorder, e.clientX, e.clientY);
                    }}
                    onClick={(e) => {
                      if (focusReadOnly) return;
                      e.stopPropagation();
                      // an-audit-finding: suppress the synthetic click that the browser
                      // fires after a drag. Without this, a group drag ends
                      // with this (the grabbed) key's trailing click calling
                      // onKeyClick, which collapses the multi-selection down
                      // to just this one key. The backdrop already guards on
                      // dragConsumedClickRef; the key circle must too.
                      if (dragConsumedClickRef.current) {
                        dragConsumedClickRef.current = false;
                        return;
                      }
                      if (dragRef.current === null) {
                        onKeyClick?.(p.time, e);
                      }
                    }}
                  />
                  <circle
                    className="curve-key-marker"
                    data-selected={selected ? "true" : "false"}
                    cx={p.x}
                    cy={p.y}
                    r={visR}
                    fill={focusReadOnly ? "none" : fill}
                    stroke={focusReadOnly ? channel.color : stroke}
                    strokeWidth={focusReadOnly ? 2 : strokeWidth}
                    pointerEvents="none"
                  />
                </g>
              );
            })}
          </g>
        );
      })()}

      {/* Morph overlays — one per channel that is currently morphing.
          React mounts/unmounts the group; the rAF loop writes into it
          imperatively. Rendered above the focus layer so the in-flight
          shape is always visible. */}
      {morph.activeIds.map((id) => (
        <g
          key={id}
          data-testid="curve-morph-overlay"
          data-channel-id={id}
          pointerEvents="none"
          ref={morph.attach(id)}
        />
      ))}

      {/* Marquee rectangle */}
      {marquee !== null && marquee.movedPastSlop && (
        <rect
          data-testid="curve-marquee"
          x={Math.min(marquee.startX, marquee.currX)}
          y={Math.min(marquee.startY, marquee.currY)}
          width={Math.abs(marquee.currX - marquee.startX)}
          height={Math.abs(marquee.currY - marquee.startY)}
          fill="rgb(14 165 233 / 0.15)"
          stroke="#0EA5E9"
          strokeDasharray="4 4"
          strokeWidth={1}
          pointerEvents="none"
        />
      )}
    </svg>
  );
}
