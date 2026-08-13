// CurveEditor — SVG renderer for a single track.
//
// Read-only foundation.
// Key selection:
//   - Key selection (click + Ctrl/Cmd+click toggle, click empty SVG to
//     clear). Selection state is OWNED by the parent (TrackEditor or
//     EmitterPropertyPanel) and identified by key TIME — not array
//     index — so a future drag-to-move can re-order keys in
//     the underlying multiset without invalidating the selection.
//   - Smooth (cubic-Bezier) + step (staircase) rendering branches.
//     The control-point formula matches the legacy implementation at
//     [src/UI/CurveEditor.cpp:289-292]:
//       cp1 = (p1.x + (p2.x - p1.x) / 4, p1.y)
//       cp2 = (p1.x + (p2.x - p1.x) * 3 / 4, p2.y)
//     Step expands each segment as [p1, (p2.x, p1.y), p2] so a single
//     <polyline> can render the staircase.
//
// Drag-to-move:
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
// Marquee select:
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
//     "click empty area to clear selection" UX). Insert
//     mode is unchanged: empty-canvas pointer-down still fires
//     `onCanvasAdd` and marquee is suppressed.

import { memo, useCallback, useEffect, useImperativeHandle, useLayoutEffect, useMemo, useRef, useState, type MutableRefObject, type PointerEvent as ReactPointerEvent, type Ref } from "react";
import type { InterpolationType, TrackDto, TrackName } from "@particle-editor/bridge-schema";
import { useCurveMorph, type SuppressedMove } from "../lib/use-curve-morph";
import { computeGroupMoves, valueRangeForTrack } from "@/lib/curve-model";
import { clampGroupTimeShift } from "@/lib/curve-group-shift";
import { snapToGrid, GRID_CELLS, GRID_SUBDIVISIONS } from "@/lib/curve-snap";

/** Channel definition for the multi-channel overlay branch.
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
  /** Multi-channel overlay branch: when `tracks` +
   *  `channels` + `visibleChannels` are all passed, the renderer
   *  ignores `track` / `valueRange` and instead draws one curve per
   *  visible channel in the channel's colour. Multi-channel mode is
   *  view-only — selection / drag / marquee paths are no-ops because
   *  no parent owns selection state across channels. */
  tracks?: TrackDto[] | null;
  channels?: readonly ChannelDef[];
  visibleChannels?: Record<string, boolean>;
  /** Id of the emitter `tracks` belong to. A change between renders marks an
   *  emitter SWITCH (vs an in-curve edit) so the morph rides keys along the line
   *  into place instead of popping them by time. */
  emitterId?: number | null;
  /** Hybrid focus-channel mode (restored edit surface). When set in the
   *  multi-channel branch, the renderer emphasises this channel (thick
   *  stroke + full opacity + key circles + interactive handlers) and
   *  dims the others (thinner stroke + reduced opacity + no markers).
   *  When unset, the multi-channel branch stays view-only and dims
   *  every channel equally (the view-only behavior). */
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
  /** Group-drag commit: a drag of one key within a multi-selection
   *  shifts the whole selection by (dTime, dValue). */
  onGroupDragEnd?: (dTime: number, dValue: number) => void;
  /** Group-drag live move: fires on every pointer-move past slop during
   *  a multi-selection group drag, carrying the accumulated
   *  (dTime, dValue). The parent uses it to live-update the Time/Value
   *  spinners (which show the selection AVERAGE) — the group analogue of
   *  `onKeyDragMove`. */
  onGroupDragMove?: (dTime: number, dValue: number) => void;
  /** Marquee-select handler. Fires at
   *  pointer-up when a Select-mode rubber-band drag has covered at
   *  least one key. `times` is the set of key TIMES inside the
   *  rectangle (inclusive on both axes in viewBox space). `shift`
   *  reflects whether Shift was held at marquee-start; the parent
   *  should append to the existing selection when true, replace when
   *  false. When the gesture is too short to qualify as a drag the
   *  marquee is treated as a click and `onCanvasClick` fires
   *  instead. */
  onCanvasMarqueeSelect?: (times: number[], shift: boolean) => void;
  /** Ref to drive the marquee imperatively so it can be STARTED from
   *  outside the plot SVG (the axis-label gutters). Only the multi-channel
   *  focus editor implements it; the single-track branch ignores it. */
  marqueeRef?: Ref<CurveMarqueeHandle>;
  /** Morph-suppress ref, owned by CurveEditorPanel so its spinner/commit
   *  handlers can SNAP (not glide) a value/time edit they already applied
   *  optimistically — the same mechanism the canvas drag uses (#610/#613).
   *  When provided it replaces the internal ref, so both the canvas drag
   *  (here) and the panel's spinners write to one shared suppress slot. */
  suppressRef?: MutableRefObject<SuppressedMove>;
  /** When true, dragged/inserted keys snap to the minor sub-grid on both
   *  axes (#618). Owned + persisted by CurveEditorPanel; the single-track
   *  legacy branch ignores it (production always renders the multi path). */
  snapEnabled?: boolean;
  /** Keyboard navigation on the focused plot SVG (design pass, B1). The SVG
   *  is a single Tab stop; arrows map to actions the PARENT executes against
   *  its selection + spinner-commit handlers (no mutation logic lives in the
   *  renderer): Left/Right cycle key selection in time order, Up/Down switch
   *  the focused channel, Ctrl+arrows nudge the selection's time/value by the
   *  spinner step, Home/End jump to the first/last key. */
  onKeyboardNav?: (action: CurveKeyboardNavAction) => void;
};

/** Keyboard actions the plot SVG can request from its parent (B1). */
export type CurveKeyboardNavAction =
  | { kind: "select-step"; dir: 1 | -1 }
  | { kind: "select-edge"; edge: "first" | "last" }
  | { kind: "channel-step"; dir: 1 | -1 }
  | { kind: "nudge-time"; dir: 1 | -1 }
  | { kind: "nudge-value"; dir: 1 | -1 };

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

// Curve marker / line colours route through the --curve-* token family
// (styles/tokens.css) so they flip with the theme — SVG presentation
// attributes resolve var() natively, same as --curve-grid / --curve-axis.
const UNSELECTED_FILL = "var(--curve-marker)";
const BORDER_FILL = "var(--curve-marker-border)";        // anchor-key slate fill
const BORDER_STROKE = "var(--curve-marker-border-stroke)"; // accent ring on anchor keys
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
 *  "step pen" — visual differentiation is deferred; the
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

export function CurveEditor({
  track,
  valueRange,
  tracks,
  channels,
  visibleChannels,
  emitterId,
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
  suppressRef,
  snapEnabled,
  onKeyboardNav,
}: Props) {
  // Multi-channel overlay branch. Triggered when the caller provides
  // `tracks` + `channels` + `visibleChannels`. When `focusChannel` is
  // also set, the focus channel renders emphasised + interactive (key
  // circles, drag, marquee, insert, context menu) while the other
  // visible channels render dimmed as background context. When
  // `focusChannel` is unset the branch stays view-only.
  if (tracks !== undefined && channels !== undefined && visibleChannels !== undefined) {
    return (
      <MultiChannelCurves
        tracks={tracks}
        channels={channels}
        visibleChannels={visibleChannels}
        emitterId={emitterId}
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
        suppressRef={suppressRef}
        snapEnabled={snapEnabled}
        onKeyboardNav={onKeyboardNav}
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

  // Marquee state. Held as React state
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
  const verticalLines: number[] = [];
  for (let i = 0; i <= GRID_CELLS; i++) {
    verticalLines.push((i / GRID_CELLS) * width);
  }
  const horizontalLines: number[] = [];
  for (let i = 0; i <= GRID_CELLS; i++) {
    horizontalLines.push((i / GRID_CELLS) * height);
  }
  // Faint minor sub-grid (#618) — same layout as the multi-channel renderer
  // so the two paths stay visually consistent. Major-coincident lines skipped.
  const minorCells = GRID_CELLS * GRID_SUBDIVISIONS;
  const minorVerticalLines: number[] = [];
  const minorHorizontalLines: number[] = [];
  for (let i = 1; i < minorCells; i++) {
    if (i % GRID_SUBDIVISIONS === 0) continue;
    minorVerticalLines.push((i / minorCells) * width);
    minorHorizontalLines.push((i / minorCells) * height);
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
   *  Select mode, starts a marquee — pointer-move grows the
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
        // In Insert mode, pointer-down on
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
          // Right-click on empty canvas
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

      {/* Faint minor sub-grid (#618) — before the major grid so majors win. */}
      <g data-testid="curve-subgrid" stroke="var(--curve-subgrid)" strokeWidth={0.5} pointerEvents="none">
        {minorVerticalLines.map((x, i) => (
          <line key={`mv${i}`} x1={x} y1={0} x2={x} y2={height} />
        ))}
        {minorHorizontalLines.map((y, i) => (
          <line key={`mh${i}`} x1={0} y1={y} x2={width} y2={y} />
        ))}
      </g>
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
          stroke="var(--curve-marker)"
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
          stroke="var(--curve-marker)"
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
          stroke="var(--curve-marker)"
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

      {/* Marquee rectangle. Rendered last so it draws over the
          keys. `pointerEvents="none"` keeps the captured backdrop in
          control of the gesture — the rect is purely visual. The
          fill + dashed border use the accent token (--accent-soft fill,
          --accent stroke) so the marquee themes with the rest of the UI. */}
      {marquee !== null && marquee.movedPastSlop && (
        <rect
          data-testid="curve-marquee"
          x={Math.min(marquee.startX, marquee.currX)}
          y={Math.min(marquee.startY, marquee.currY)}
          width={Math.abs(marquee.currX - marquee.startX)}
          height={Math.abs(marquee.currY - marquee.startY)}
          fill="var(--accent-soft)"
          stroke="var(--accent)"
          strokeDasharray="4 4"
          strokeWidth={1}
          pointerEvents="none"
        />
      )}
    </svg>
  );
}

// ─── Multi-channel overlay (with hybrid focus-channel restore) ──
//
// Renders a single SVG with one layer per visible channel. Two modes:
//
//   1. View-only (focusChannel undefined) — every visible channel
//      renders the same way: curve line + small unstyled circles, all
//      `pointerEvents=none`. Matches the view-only behaviour.
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
  /** Id of the emitter `tracks` belong to (drives the morph's switch-vs-edit
   *  decision — see useCurveMorph). */
  emitterId?: number | null;
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
  /** A drag of one key within a multi-selection shifts the whole
   *  selection by (dTime, dValue). The parent applies it via applyGroupShift. */
  onGroupDragEnd?: (dTime: number, dValue: number) => void;
  /** Group-drag live move — fires every move past slop with the live
   *  (dTime, dValue) so the parent can live-update the average spinners. */
  onGroupDragMove?: (dTime: number, dValue: number) => void;
  onCanvasMarqueeSelect?: (times: number[], shift: boolean) => void;
  marqueeRef?: Ref<CurveMarqueeHandle>;
  /** Shared morph-suppress ref (see Props.suppressRef). */
  suppressRef?: MutableRefObject<SuppressedMove>;
  /** Snap dragged/inserted keys to the minor sub-grid (see Props.snapEnabled). */
  snapEnabled?: boolean;
  /** Keyboard navigation requests (see Props.onKeyboardNav). */
  onKeyboardNav?: (action: CurveKeyboardNavAction) => void;
};


type CurvePoint = { x: number; y: number; time: number; value: number };

type CurveLayerModel = {
  channel: ChannelDef;
  track: TrackDto;
  points: CurvePoint[];
  range: { min: number; max: number };
};

type CurveLayerCacheEntry = {
  channel: ChannelDef;
  track: TrackDto;
  width: number;
  height: number;
  timeMin: number;
  timeMax: number;
  displayMin: number;
  displayMax: number;
  layer: CurveLayerModel;
};

type CurveDragState = {
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
  isGroup: boolean;
  groupDTime: number;
  groupDValue: number;
};

function useLayerRenderCount(): number {
  const countRef = useRef(0);
  countRef.current += 1;
  return countRef.current;
}

type StaticChannelLayerProps = {
  layer: CurveLayerModel;
  focusEnabled: boolean;
  hidden: boolean;
};

const StaticChannelLayer = memo(function StaticChannelLayer({
  layer,
  focusEnabled,
  hidden,
}: StaticChannelLayerProps) {
  const { channel, track, points } = layer;
  const renderCount = useLayerRenderCount();
  const interp = track.interpolation;
  const smoothPath = useMemo(
    () => (points.length >= 2 && interp === "smooth" ? buildSmoothPath(points) : ""),
    [interp, points],
  );
  const stepPoints = useMemo(
    () => (points.length >= 2 && interp === "step" ? buildStepPolyline(points) : ""),
    [interp, points],
  );
  const linearPoints = useMemo(
    () => (points.length >= 2 && interp === "linear" ? points.map((p) => String(p.x) + "," + String(p.y)).join(" ") : ""),
    [interp, points],
  );
  const layerOpacity = focusEnabled ? 0.4 : 1;
  const strokeW = 2;
  const markerR = focusEnabled ? 3 : 4;
  const markerStroke = focusEnabled ? "none" : "var(--curve-marker-stroke)";
  const markerStrokeW = focusEnabled ? 0 : 1;
  const markerTestId = focusEnabled ? undefined : "curve-key";
  return (
    <g
      data-testid={"curve-layer-" + channel.id}
      data-channel-id={channel.id}
      data-key-count={points.length}
      data-focus="false"
      data-render-count={renderCount}
      style={{ opacity: layerOpacity, visibility: hidden ? "hidden" : undefined }}
    >
      {smoothPath !== "" && (
        <path fill="none" stroke={channel.color} strokeWidth={strokeW} d={smoothPath} pointerEvents="none" />
      )}
      {stepPoints !== "" && (
        <polyline fill="none" stroke={channel.color} strokeWidth={strokeW} points={stepPoints} pointerEvents="none" />
      )}
      {linearPoints !== "" && (
        <polyline fill="none" stroke={channel.color} strokeWidth={strokeW} points={linearPoints} pointerEvents="none" />
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
});

type FocusChannelLayerProps = {
  layer: CurveLayerModel;
  renderPoints: CurvePoint[];
  focusReadOnly: boolean;
  selectedKeyTimes?: ReadonlySet<number>;
  focusBorderTimes: ReadonlySet<number>;
  hidden: boolean;
  height: number;
  onKeyClick?: (time: number, event: React.MouseEvent | React.PointerEvent) => void;
  onKeyContextMenu?: (
    time: number,
    isBorder: boolean,
    clientX: number,
    clientY: number,
  ) => void;
  startDrag: (
    event: ReactPointerEvent<SVGCircleElement>,
    keyTime: number,
    keyValue: number,
  ) => void;
  dragRef: MutableRefObject<CurveDragState | null>;
  dragConsumedClickRef: MutableRefObject<boolean>;
};

const FocusChannelLayer = memo(function FocusChannelLayer({
  layer,
  renderPoints,
  focusReadOnly,
  selectedKeyTimes,
  focusBorderTimes,
  hidden,
  height,
  onKeyClick,
  onKeyContextMenu,
  startDrag,
  dragRef,
  dragConsumedClickRef,
}: FocusChannelLayerProps) {
  const { channel, track } = layer;
  const renderCount = useLayerRenderCount();
  const interp = track.interpolation;
  const fillGradId = "curve-fill-" + channel.id;
  const fillPath = useMemo(
    () => (renderPoints.length >= 2 ? buildFillPath(renderPoints, interp, height) : ""),
    [height, interp, renderPoints],
  );
  const smoothPath = useMemo(
    () => (renderPoints.length >= 2 && interp === "smooth" ? buildSmoothPath(renderPoints) : ""),
    [interp, renderPoints],
  );
  const stepPoints = useMemo(
    () => (renderPoints.length >= 2 && interp === "step" ? buildStepPolyline(renderPoints) : ""),
    [interp, renderPoints],
  );
  const linearPoints = useMemo(
    () => (renderPoints.length >= 2 && interp === "linear" ? renderPoints.map((p) => String(p.x) + "," + String(p.y)).join(" ") : ""),
    [interp, renderPoints],
  );
  return (
    <g
      data-testid={"curve-layer-" + channel.id}
      data-channel-id={channel.id}
      data-key-count={renderPoints.length}
      data-focus="true"
      data-readonly={focusReadOnly ? "true" : "false"}
      data-render-count={renderCount}
      style={{ visibility: hidden ? "hidden" : undefined }}
    >
      <defs>
        <linearGradient id={fillGradId} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={channel.color} stopOpacity="0.25" />
          <stop offset="100%" stopColor={channel.color} stopOpacity="0" />
        </linearGradient>
      </defs>
      {fillPath !== "" && (
        <path data-testid="curve-fill" fill={"url(#" + fillGradId + ")"} stroke="none" d={fillPath} pointerEvents="none" />
      )}
      {smoothPath !== "" && (
        <path data-testid="curve-path" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} d={smoothPath} pointerEvents="none" />
      )}
      {stepPoints !== "" && (
        <polyline data-testid="curve-polyline" data-interpolation="step" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} points={stepPoints} pointerEvents="none" />
      )}
      {linearPoints !== "" && (
        <polyline data-testid="curve-polyline" data-interpolation="linear" fill="none" stroke={channel.color} strokeWidth={3} strokeDasharray={focusReadOnly ? READONLY_DASH : undefined} points={linearPoints} pointerEvents="none" />
      )}
      {renderPoints.map((p, i) => {
        const selected = selectedKeyTimes?.has(p.time) ?? false;
        const isBorder = focusBorderTimes.has(p.time);
        const hitR = selected ? 18 : 14;
        const visR = selected ? 6.5 : 5;
        const markerFill = focusReadOnly ? "none" : selected ? "var(--curve-marker-core)" : channel.color;
        const markerStroke = focusReadOnly ? channel.color : selected ? channel.color : "none";
        const markerStrokeWidth = focusReadOnly ? 2 : selected ? 2.5 : 0;
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
              fill={markerFill}
              stroke={markerStroke}
              strokeWidth={markerStrokeWidth}
              pointerEvents="none"
            />
          </g>
        );
      })}
    </g>
  );
});
function MultiChannelCurves({
  tracks,
  channels,
  visibleChannels,
  emitterId,
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
  suppressRef,
  snapEnabled,
  onKeyboardNav,
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
  // [design pass B1] True while the plot SVG itself holds keyboard focus —
  // gates the sr-only status region below so screen readers hear selection
  // changes during keyboard nav, but mouse drags (which also change the
  // selection) stay silent.
  const [kbdFocused, setKbdFocused] = useState(false);
  // Track the SVG's box size every frame — INCLUDING during the dock slide — so
  // the viewBox follows the container and the curve/grid re-projects crisply at
  // each width. (We do NOT hold the viewBox during the slide: a held viewBox is
  // stretched via the 100%-size SVG and then snaps at the settle — the visible
  // "jump". The per-frame re-measure is cheap now that the atlas grid is frozen
  // + pre-mounted, so it no longer contends with the flex tween.)
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
  // matches the design lock.
  const verticalLines: number[] = [];
  for (let i = 0; i <= GRID_CELLS; i++) {
    verticalLines.push((i / GRID_CELLS) * width);
  }
  const horizontalLines: number[] = [];
  for (let i = 0; i <= GRID_CELLS; i++) {
    horizontalLines.push((i / GRID_CELLS) * height);
  }
  // Faint minor sub-grid (#618): GRID_SUBDIVISIONS minor cells per major
  // cell. Skip the indices that coincide with a major line (i % subdiv === 0)
  // so the majors aren't double-stroked (and read at full weight).
  const minorCells = GRID_CELLS * GRID_SUBDIVISIONS;
  const minorVerticalLines: number[] = [];
  const minorHorizontalLines: number[] = [];
  for (let i = 1; i < minorCells; i++) {
    if (i % GRID_SUBDIVISIONS === 0) continue;
    minorVerticalLines.push((i / minorCells) * width);
    minorHorizontalLines.push((i / minorCells) * height);
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
  const layerCacheRef = useRef<Map<string, CurveLayerCacheEntry>>(new Map());
  const layers = useMemo<CurveLayerModel[]>(() => {
    const nextCache = new Map<string, CurveLayerCacheEntry>();
    const nextLayers: CurveLayerModel[] = [];
    for (const t of tracks ?? []) {
      const channel = channels.find((c) => c.trackName === t.name);
      if (channel === undefined) continue;
      if (!(visibleChannels[channel.id] ?? channel.defaultOn)) continue;
      const range = valueRangeForTrack(t);
      const projY = displayRange ?? range;
      const prev = layerCacheRef.current.get(channel.id);
      if (
        prev !== undefined
        && prev.channel === channel
        && prev.track === t
        && prev.width === width
        && prev.height === height
        && prev.timeMin === timeMin
        && prev.timeMax === timeMax
        && prev.displayMin === projY.min
        && prev.displayMax === projY.max
      ) {
        nextCache.set(channel.id, prev);
        nextLayers.push(prev.layer);
        continue;
      }
      const points = t.keys.map((k) => ({
        x: project(k.time, timeMin, timeMax, width),
        y: height - project(k.value, projY.min, projY.max, height),
        time: k.time,
        value: k.value,
      }));
      const layer = { channel, track: t, points, range };
      nextCache.set(channel.id, {
        channel,
        track: t,
        width,
        height,
        timeMin,
        timeMax,
        displayMin: projY.min,
        displayMax: projY.max,
        layer,
      });
      nextLayers.push(layer);
    }
    layerCacheRef.current = nextCache;
    return nextLayers;
  }, [channels, displayRange, height, timeMax, timeMin, tracks, visibleChannels, width]);

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
  const dragRef = useRef<CurveDragState | null>(null);
  const [, setDragTick] = useState(0);
  const dragFrameRef = useRef<number | null>(null);
  const scheduleDragTick = useCallback(() => {
    if (dragFrameRef.current !== null) return;
    if (typeof requestAnimationFrame !== "function") {
      setDragTick((n) => n + 1);
      return;
    }
    dragFrameRef.current = requestAnimationFrame(() => {
      dragFrameRef.current = null;
      setDragTick((n) => n + 1);
    });
  }, []);
  const flushDragTick = useCallback(() => {
    if (dragFrameRef.current !== null && typeof cancelAnimationFrame === "function") {
      cancelAnimationFrame(dragFrameRef.current);
    }
    dragFrameRef.current = null;
    setDragTick((n) => n + 1);
  }, []);
  useEffect(() => () => {
    if (dragFrameRef.current !== null && typeof cancelAnimationFrame === "function") {
      cancelAnimationFrame(dragFrameRef.current);
    }
  }, []);

  // ── Morph animation. The suppress slot is SHARED: the canvas drag (below)
  // records into it, and — when CurveEditorPanel passes its own `suppressRef` —
  // so do the panel's spinner/commit handlers, so a value/time edit that was
  // already applied optimistically SNAPS instead of gliding (#610/#613). A
  // local fallback keeps standalone callers (no suppressRef) working. dragRef
  // must be declared before this hook so the isDragging closure captures it.
  const localSuppressRef = useRef<SuppressedMove>(null);
  const morphSuppressRef = suppressRef ?? localSuppressRef;
  const morph = useCurveMorph({
    channels: layers.map((l) => {
      const isFocus = focusLayer !== null && l.channel.id === focusLayer.channel.id;
      // Mirror the STATIC key-dot styling so a switch's gliding markers hand off
      // to the static dots with no size/stroke jump: the focus channel's keys are
      // r=5 / no stroke; background keys are r=3 / no stroke in focus mode and the
      // full-fidelity r=4 / dark stroke in view-only mode (CurveEditor static layer).
      return {
        channelId: l.channel.id,
        color: l.channel.color,
        track: l.track,
        vMin: (displayRange ?? l.range).min,
        vMax: (displayRange ?? l.range).max,
        dashed: focusReadOnly && isFocus,
        strokeWidth: focusEnabled && isFocus ? 3 : 2,
        opacity: focusEnabled && !isFocus ? 0.4 : 1,
        isFocus,
        markerRadius: isFocus ? 5 : focusEnabled ? 3 : 4,
        markerStroke: isFocus || focusEnabled ? "none" : "var(--curve-marker-stroke)",
        markerStrokeWidth: isFocus || focusEnabled ? 0 : 1,
      };
    }),
    width,
    height,
    timeMin,
    timeMax,
    isDragging: () => dragRef.current !== null,
    suppressRef: morphSuppressRef,
    emitterId,
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
  const focusBorderTimes = useMemo(() => {
    const times = new Set<number>();
    if (focusLayer !== null && focusLayer.track.keys.length > 0) {
      const ks = focusLayer.track.keys;
      times.add(ks[0]!.time);
      times.add(ks[ks.length - 1]!.time);
    }
    return times;
  }, [focusLayer])

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
    // ── Group-drag branch: shift the whole selection by the
    // grabbed key's cursor delta. Border selected keys stay fixed in time;
    // the time-shift is clamped so interior selected keys never cross the
    // global endpoints. The per-key value clamp happens on commit
    // (applyGroupShift) and in the render preview below.
    if (drag.isGroup && selectedKeyTimes) {
      const rawTime = unproject(x, timeMin, timeMax, width);
      // Snap (#618) is ANCHOR-based: snap the grabbed key's own target
      // position to the grid, so the whole selection shifts by that snapped
      // delta (non-anchor keys keep their relative spacing). Value snaps to
      // the visible canvas range; time to [timeMin,timeMax]. Snap happens
      // BEFORE the endpoint clamp below — if the clamp adjusts it the anchor
      // may land slightly off-grid (accepted; keeps the selection in bounds).
      let anchorValue = unproject(height - y, canvasVMin, canvasVMax, height);
      if (snapEnabled) anchorValue = snapToGrid(anchorValue, canvasVMin, canvasVMax);
      const rawValue = Math.max(focusVMin, Math.min(focusVMax, anchorValue));
      let dTime = rawTime - drag.startTime;
      if (snapEnabled) {
        dTime = snapToGrid(drag.startTime + dTime, timeMin, timeMax) - drag.startTime;
      }
      // Bound the rigid shift against the nearest keys that stay put (unselected
      // keys, selected borders, endpoints) so no moving key lands on another
      // key's time (#619). Same clamp the commit uses (computeGroupMoves), so
      // this preview and the commit agree. Returns 0 for an all-border selection.
      dTime = clampGroupTimeShift(
        focusLayer.track.keys.map((k) => k.time),
        selectedKeyTimes,
        focusBorderTimes,
        dTime,
      );
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
      scheduleDragTick();
      return;
    }
    let nextTime = unproject(x, timeMin, timeMax, width);
    // Pointer Y maps through the CANVAS range (so the cursor follows
    // the curve visually); the value is then clamped to the focus
    // channel's engine bounds below so the commit stays legal.
    let nextValue = unproject(height - y, canvasVMin, canvasVMax, height);
    // Snap (#618): snap to the VISIBLE grid — time to [timeMin,timeMax],
    // value to the canvas range (what the grid draws) — BEFORE the
    // border/neighbour and focus clamps so multiset invariants still hold.
    // When a snapped stop collides with a neighbour the eps-clamp wins and the
    // key lands valid-but-slightly-off-grid (accepted; rare).
    if (snapEnabled) {
      nextValue = snapToGrid(nextValue, canvasVMin, canvasVMax);
    }
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
        if (snapEnabled) nextTime = snapToGrid(nextTime, timeMin, timeMax);
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
    scheduleDragTick();
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
    flushDragTick();
    if (isGroup) {
      // Commit the whole-selection shift (or treat a no-move as a
      // plain key click so it falls back to single-select).
      if (moved && onGroupDragEnd) {
        dragConsumedClickRef.current = true;
        // Record suppression BEFORE firing the callback so the hook
        // sees it when the parent re-renders with the committed tracks.
        if (focusLayer !== null && selectedKeyTimes) {
          const suppMoves = computeGroupMoves(
            focusLayer.track.keys,
            selectedKeyTimes,
            focusBorderTimes,
            groupDTime,
            groupDValue,
            { min: focusVMin, max: focusVMax },
          );
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
    flushDragTick();
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

  // Imperative entry so a marquee can BEGIN from outside this SVG (the
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
      let time = unproject(x, timeMin, timeMax, width);
      // Pointer Y → value through the CANVAS range (the visible grid), then
      // clamp to the focus channel's engine bounds below — the host inserts
      // the received value verbatim (it does NOT clamp), so an out-of-range
      // value would otherwise create an illegal key (#618 review).
      let value = unproject(height - y, canvasVMin, canvasVMax, height);
      if (snapEnabled) {
        const snappedTime = snapToGrid(time, timeMin, timeMax);
        // Border keys always occupy timeMin/timeMax; the host resolves a
        // colliding insert by nudging +0.001, which at timeMax lands OUT of
        // range (#618 review). Only take the snapped time when its stop is
        // free; otherwise keep the raw drop time (which the host can nudge
        // safely inward).
        if (focusLayer === null || !focusLayer.track.keys.some((k) => k.time === snappedTime)) {
          time = snappedTime;
        }
        value = snapToGrid(value, canvasVMin, canvasVMax);
      }
      // Insert must honour the focus channel's value range like drag does.
      value = Math.max(focusVMin, Math.min(focusVMax, value));
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
    // Group-drag preview: every selected key shifts by the group
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

  // [design pass B1] sr-only polite status: what the keyboard selection is,
  // announced only while the SVG holds focus (see kbdFocused above).
  const statusChannel = focusChannel != null ? channels.find((c) => c.id === focusChannel) ?? null : null;
  const statusKeys = statusChannel !== null
    ? tracks?.find((t) => t.name === statusChannel.trackName)?.keys ?? []
    : [];
  let kbdStatus = "";
  if (kbdFocused && statusChannel !== null) {
    const sel = selectedKeyTimes ?? new Set<number>();
    if (statusKeys.length === 0) {
      kbdStatus = `${statusChannel.label}: no keys.`;
    } else if (sel.size === 0) {
      kbdStatus = `${statusChannel.label}: ${statusKeys.length} keys. Press Left or Right to select one.`;
    } else if (sel.size === 1) {
      const t = [...sel][0]!;
      const idx = statusKeys.findIndex((k) => k.time === t);
      const key = idx >= 0 ? statusKeys[idx]! : null;
      kbdStatus = key
        ? `${statusChannel.label} key ${idx + 1} of ${statusKeys.length}: time ${key.time.toFixed(1)}%, value ${key.value.toFixed(2)}.`
        : `${statusChannel.label}: 1 key selected.`;
    } else {
      kbdStatus = `${statusChannel.label}: ${sel.size} keys selected.`;
    }
  }

  return (
    <>
    <svg
      ref={svgRef}
      data-testid="curve-editor-svg"
      data-multi-channel="true"
      data-visible-count={layers.length}
      data-focus-channel={focusChannel ?? ""}
      data-insert-mode={insertMode ? "true" : "false"}
      data-dragging={drag !== null ? "true" : "false"}
      // role=group + tabIndex: the plot is a single Tab stop with its own
      // keyboard nav (design pass, B1) — it stopped being a static image.
      role="group"
      tabIndex={focusEnabled && onKeyboardNav ? 0 : undefined}
      aria-label={`Multi-channel curve plot, ${layers.length} channels. Arrow keys select keys and switch channels; hold Ctrl to nudge.`}
      onFocus={() => setKbdFocused(true)}
      onBlur={() => setKbdFocused(false)}
      onKeyDown={
        focusEnabled && onKeyboardNav
          ? (e) => {
              const ctrl = e.ctrlKey || e.metaKey;
              let action: CurveKeyboardNavAction | null = null;
              switch (e.key) {
                case "ArrowRight": action = ctrl ? { kind: "nudge-time", dir: 1 } : { kind: "select-step", dir: 1 }; break;
                case "ArrowLeft":  action = ctrl ? { kind: "nudge-time", dir: -1 } : { kind: "select-step", dir: -1 }; break;
                case "ArrowUp":    action = ctrl ? { kind: "nudge-value", dir: 1 } : { kind: "channel-step", dir: -1 }; break;
                case "ArrowDown":  action = ctrl ? { kind: "nudge-value", dir: -1 } : { kind: "channel-step", dir: 1 }; break;
                case "Home":       action = { kind: "select-edge", edge: "first" }; break;
                case "End":        action = { kind: "select-edge", edge: "last" }; break;
                default: return;
              }
              e.preventDefault();
              onKeyboardNav(action);
            }
          : undefined
      }
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
      className="block h-full w-full select-none focus-ring-inset"
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
      {/* Faint minor sub-grid (#618) — rendered BEFORE the major grid so the
          major lines paint on top at full weight. */}
      <g data-testid="curve-subgrid" stroke="var(--curve-subgrid)" strokeWidth={0.5} pointerEvents="none">
        {minorVerticalLines.map((x, i) => (
          <line key={`mv${i}`} x1={x} y1={0} x2={x} y2={height} />
        ))}
        {minorHorizontalLines.map((y, i) => (
          <line key={`mh${i}`} x1={0} y1={y} x2={width} y2={y} />
        ))}
      </g>
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
      {layers.map((layer) => {
        const isFocus = focusEnabled && focusLayer !== null && layer.channel.id === focusLayer.channel.id;
        if (isFocus) return null;
        return (
          <StaticChannelLayer
            key={layer.channel.id}
            layer={layer}
            focusEnabled={focusEnabled}
            hidden={morph.isActive(layer.channel.id)}
          />
        );
      })}

      {/* Morph overlays — one per channel that is currently morphing.
          React mounts/unmounts the group; the rAF loop writes into it
          imperatively. Rendered BELOW the focus layer so a morphing
          NON-focus channel (e.g. a locked follower catching up to a
          master edit) never paints over the focus channel's curve or
          key markers — overlays take the same stacking position as the
          static layers they replace (the morphing channel's own static
          layer is visibility-hidden, so for the focus channel the
          overlay effectively stands in at the right z-position).
          Within the overlays, the focus channel sorts LAST so a
          simultaneously-morphing focus channel still draws above
          morphing followers. */}
      {[...morph.activeIds]
        .sort((a, b) =>
          (a === focusLayer?.channel.id ? 1 : 0) - (b === focusLayer?.channel.id ? 1 : 0))
        .map((id) => (
          <g
            key={id}
            data-testid="curve-morph-overlay"
            data-channel-id={id}
            pointerEvents="none"
            ref={morph.attach(id)}
          />
        ))}

      {/* Focus layer — full opacity, thicker stroke, interactive
          circles. Rendered last so it draws above the dimmed
          background layers. */}
      {focusLayer !== null && (
        <FocusChannelLayer
          key={focusLayer.channel.id}
          layer={focusLayer}
          renderPoints={focusRenderPoints}
          focusReadOnly={focusReadOnly}
          selectedKeyTimes={selectedKeyTimes}
          focusBorderTimes={focusBorderTimes}
          hidden={morph.isActive(focusLayer.channel.id)}
          height={height}
          onKeyClick={onKeyClick}
          onKeyContextMenu={onKeyContextMenu}
          startDrag={startDrag}
          dragRef={dragRef}
          dragConsumedClickRef={dragConsumedClickRef}
        />
      )}

      {/* Marquee rectangle */}
      {marquee !== null && marquee.movedPastSlop && (
        <rect
          data-testid="curve-marquee"
          x={Math.min(marquee.startX, marquee.currX)}
          y={Math.min(marquee.startY, marquee.currY)}
          width={Math.abs(marquee.currX - marquee.startX)}
          height={Math.abs(marquee.currY - marquee.startY)}
          fill="var(--accent-soft)"
          stroke="var(--accent)"
          strokeDasharray="4 4"
          strokeWidth={1}
          pointerEvents="none"
        />
      )}
    </svg>
    <div role="status" aria-live="polite" className="sr-only" data-testid="curve-kbd-status">
      {kbdStatus}
    </div>
    </>
  );
}
