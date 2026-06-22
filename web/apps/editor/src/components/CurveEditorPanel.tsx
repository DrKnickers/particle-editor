// CurveEditorPanel — hybrid focus-channel curve editor.
//
// Task 2.6 set up the always-on bottom panel with a multi-channel
// overlay and per-channel visibility checkboxes. The hybrid focus-
// channel restoration adds the edit surface that Task 2.6 dropped:
//
//   - Clicking a channel ROW (not just the checkbox) sets that
//     channel as the EDIT FOCUS. The focus row gets a visual
//     indicator (bg-accent-soft + data-focus="true"). Clicking a
//     hidden channel's row turns it ON before focusing — focus
//     conceptually requires visibility.
//
//   - The multi-channel SVG renders the focus channel emphasised
//     (thick stroke, opaque, key circles) and the other visible
//     channels dimmed (opacity 0.4, no markers) as background
//     context. All key interactions (click select, drag-to-move,
//     marquee, Insert add, right-click) route to the focus channel.
//
//   - A .ce-toolbar row above the .ce-body hosts the edit
//     affordances: Select/Insert mode toggle, Linear/Smooth/Step
//     interpolation, Lock-to combo, Time/Value spinners for the
//     selected key.
//
//   - The panel hosts a window-scoped Delete keypress handler that
//     fires delete-track-keys on the focus channel's selected keys,
//     filtering out border keys + any presses inside a typing surface
//     (input / textarea / select).
//
// Channel visibility AND focus are SESSION-SCOPED — every editor
// boot starts with the documented defaults (R / G / B visible,
// focus on Red). Selection (per focus channel) clears on focus
// change. Optimistic (time, value) override keeps spinners
// populated across the bridge round-trip (lessons.md: sticky
// override; don't clear on every `tracks` refresh).
//
// Y-axis range is UNIFIED across visible channels — every visible
// curve renders into the same Y space (union of per-channel
// ranges) so when you turn on Scale-at-20 alongside RGB, the canvas
// scales out and the RGB curves squish near the bottom. Axis
// labels reflect the unified range. Drag value-clamp stays scoped
// to the focus channel so engine bounds aren't violated even when
// the visible canvas extends past them.

import { useCallback, useEffect, useMemo, useRef, useState, type ReactElement } from "react";
import * as Select from "@radix-ui/react-select";
import { ChevronDown, Lock, MousePointer2, Plus, Trash2 } from "lucide-react";
import type {
  Bridge,
  InterpolationType,
  TrackDto,
  TrackName,
} from "@particle-editor/bridge-schema";
import { CurveEditor, type ChannelDef, type CurveMarqueeHandle } from "@/screens/CurveEditor";
import { Spinner } from "@/primitives/Spinner";
import { Tip } from "@/primitives/Tip";
import {
  getCurveKeysClipboard,
  setCurveKeysClipboard,
} from "@/lib/curve-key-clipboard";
import { isAtlasEligible } from "@/lib/atlas-grid";
import { publishAtlasContext } from "@/lib/atlas-context";
import { useAtlasAutoOpen } from "@/lib/use-atlas-autoopen";

/** Channel registry. Order matches the design's left-column list. The
 *  `trackName` field bridges the UI-facing id (e.g. "rotation") to the
 *  wire-level TrackName (e.g. "rotationSpeed"). Colour tokens map to
 *  the editor's existing palette so themes track the rest of the UI:
 *    - Scale     → --warning  (amber)
 *    - Red       → --x-axis   (warm red)
 *    - Green     → --y-axis   (green)
 *    - Blue      → --z-axis   (blue)
 *    - Alpha     → --text-2   (neutral grey, distinguishable from RGB)
 *    - Rotation  → --accent   (sky blue accent)
 *    - Index     → --text-3   (darker grey, defaults off)
 */
// Display order is grouped: the colour channels (R / G / B / A) sit
// at the top, and the transform-y channels (Scale / Index / Rotation)
// sit below, separated by a horizontal divider rendered in the
// list. Within the transform group, Scale and Index are exclusive —
// enabling either hides everything else (see `EXCLUSIVE_CHANNELS`,
// `handleRowClick`, and the checkbox onChange handler). The Index
// atlas sub-frame is read on its own integer scale, like Scale, so
// it's not meaningful to overlay it with the colour curves (F9).
export const CHANNELS: readonly ChannelDef[] = [
  { id: "red",      label: "Red",      color: "var(--x-axis)",  defaultOn: true,  trackName: "red" },
  { id: "green",    label: "Green",    color: "var(--y-axis)",  defaultOn: true,  trackName: "green" },
  { id: "blue",     label: "Blue",     color: "var(--z-axis)",  defaultOn: true,  trackName: "blue" },
  { id: "alpha",    label: "Alpha",    color: "var(--text-2)",  defaultOn: false, trackName: "alpha" },
  { id: "scale",    label: "Scale",    color: "var(--warning)", defaultOn: false, trackName: "scale" },
  { id: "index",    label: "Index",    color: "var(--text)",    defaultOn: false, trackName: "index" },
  { id: "rotation", label: "Rotation", color: "var(--accent)",  defaultOn: false, trackName: "rotationSpeed" },
] as const;

// Channels that are mutually exclusive with all others: turning one on
// (via row click or checkbox) hides every other channel ("solo mode"),
// and selecting any non-exclusive curve exits solo. F9 added Index to
// the set that previously held only Scale; Rotation joined later (its
// degrees/sec scale, like Index and Scale, doesn't share the 0..1 band).
const EXCLUSIVE_CHANNELS: ReadonlySet<string> = new Set(["scale", "index", "rotation"]);

/** DOM tag names that own their own keyboard handling. Delete events
 *  originating inside these MUST NOT be intercepted — typing Delete in
 *  a text field should delete a character, not a curve key. */
const TYPING_TAGS = new Set(["INPUT", "TEXTAREA", "SELECT"]);

const INTERP_KINDS: readonly InterpolationType[] = Object.freeze([
  "linear", "smooth", "step",
]);

/** Tiny inline glyphs for the three interpolation modes. Each is a
 *  16×16 SVG showing the curve shape between two endpoints — clearer
 *  than text at icon size, no lucide icon matches the semantics
 *  closely enough (lucide has `Spline` but not `Linear` / `Step`
 *  curve-editor glyphs). */
const INTERP_ICONS: Record<InterpolationType, ReactElement> = {
  linear: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <line x1="2" y1="12" x2="14" y2="4" strokeLinecap="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
  smooth: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <path d="M 2 12 C 5 12, 7 4, 14 4" strokeLinecap="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
  step: (
    <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.75" aria-hidden="true">
      <polyline points="2,12 8,12 8,4 14,4" strokeLinecap="round" strokeLinejoin="round" />
      <circle cx="2" cy="12" r="1.5" fill="currentColor" />
      <circle cx="14" cy="4" r="1.5" fill="currentColor" />
    </svg>
  ),
};

/** Lock-to combo options per track. Mirrors the legacy table at
 *  [src/UI/TrackEditor.cpp:90-98]. The leading "None" option is
 *  always present; "only None" disables the combo. */
const LOCK_TO_OPTIONS: Record<TrackName, readonly string[]> = {
  red:           ["None"],
  green:         ["None", "Red"],
  blue:          ["None", "Red", "Green"],
  alpha:         ["None", "Red", "Green", "Blue"],
  scale:         ["None"],
  index:         ["None"],
  rotationSpeed: ["None"],
};

type EditMode = "select" | "insert";
type Props = { bridge: Bridge };

function defaultVisibility(): Record<string, boolean> {
  const result: Record<string, boolean> = {};
  for (const c of CHANNELS) result[c.id] = c.defaultOn;
  return result;
}

/** Per-track value range — same logic as MultiChannelCurves' internal
 *  helper. Duplicated here because spinner value-range clamping needs
 *  it at the panel level and the helper isn't exported. */
function valueRangeForTrack(track: TrackDto): { min: number; max: number } {
  switch (track.name) {
    case "red":
    case "green":
    case "blue":
    case "alpha":
      return { min: 0, max: 1 };
    case "scale": {
      // Lower bound always 0. Upper bound auto-grows to the highest
      // key value so the curve actually reaches the top of the canvas
      // at its max. Floor at 1 so a flat-zero curve isn't a degenerate
      // single-point range (and renders as a flat line at the bottom).
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "index": {
      // Same shape as scale: lower bound 0, upper bound auto-grows
      // to the highest key. Floor at 1.
      let max = 0;
      for (const k of track.keys) {
        if (k.value > max) max = k.value;
      }
      return { min: 0, max: Math.max(max, 1) };
    }
    case "rotationSpeed": {
      // Default display range 0..1. Expands in BOTH directions to
      // include the highest and lowest keys — no caps. User can
      // input any value and the grid scales accordingly.
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

/** Spinner clamp bounds per track. These are the engine-allowed
 *  bounds the user can enter — different from the *display* range
 *  computed by `valueRangeForTrack` (which is derived from current
 *  keys and adapts as keys change). Spinner bounds are constant per
 *  channel so the user can push key values past the current display
 *  range to grow it. */
function spinnerBoundsForTrack(name: TrackName): {
  min: number;
  max: number;
  step: number;
} {
  switch (name) {
    case "red":
    case "green":
    case "blue":
    case "alpha":
      // Hard-clamped 0..1 — the engine enforces this at file-load
      // (`Verify(key.value >= 0.0f && key.value <= 1.0f)` in
      // [ParticleSystem.cpp:420](src/ParticleSystem.cpp:420)), so
      // letting the user enter out-of-range values would just get
      // rejected on save.
      return { min: 0, max: 1, step: 0.01 };
    case "scale":
      // 0..∞ in concept; using a very large but finite ceiling so
      // the Spinner's clamp logic has something to compare against.
      return { min: 0, max: 1e6, step: 0.1 };
    case "index":
      // Integer particle-index. Step 1 enforces whole-number nudges
      // via the spinner arrows / wheel / keyboard ↑↓. Users CAN
      // still type a fractional value into the field; the engine
      // accepts it but the spinner UX nudges in whole units.
      return { min: 0, max: 1e6, step: 1 };
    case "rotationSpeed":
      // Unbounded in both directions. Step 0.1 for fine control.
      return { min: -1e6, max: 1e6, step: 0.1 };
  }
}

/** Per-key result of shifting a multi-selection by (dTime, dValue):
 *  border keys keep their time but shift value; interior keys shift
 *  both, with time clamped strictly inside the track endpoints and
 *  value clamped to the channel's spinner bounds. SINGLE SOURCE of the
 *  group-shift transform — both `applyGroupShift` (the commit) and the
 *  live multi-select average (the spinner readout during a drag)
 *  consume it, so the displayed numbers always match what will commit.
 *  Keep in sync with the morph recorder in CurveEditor.tsx per the note
 *  in `applyGroupShift`. */
function computeGroupMoves(
  trackKeys: ReadonlyArray<{ time: number; value: number }>,
  selectedTimes: ReadonlySet<number>,
  borderTimes: ReadonlySet<number>,
  dTime: number,
  dValue: number,
  bounds: { min: number; max: number },
): Array<{ oldTime: number; newTime: number; newValue: number }> {
  if (trackKeys.length === 0) return [];
  const firstTime = trackKeys[0]!.time;
  const lastTime = trackKeys[trackKeys.length - 1]!.time;
  const eps = (lastTime - firstTime) / 10000 || 1e-4;
  const out: Array<{ oldTime: number; newTime: number; newValue: number }> = [];
  for (const k of trackKeys) {
    if (!selectedTimes.has(k.time)) continue;
    const isBorder = borderTimes.has(k.time);
    let nt = isBorder ? k.time : k.time + dTime;
    if (!isBorder) nt = Math.min(lastTime - eps, Math.max(firstTime + eps, nt));
    const nv = Math.min(bounds.max, Math.max(bounds.min, k.value + dValue));
    out.push({ oldTime: k.time, newTime: nt, newValue: nv });
  }
  return out;
}

/** Format an axis-label number. Integer ranges show as integers,
 *  non-integers show one decimal place. Avoids "12.0" noise on
 *  integer ranges (Scale 0..24, Index -3..10) while keeping precision
 *  for tight ranges (Rotation -1..1 → "0.5" mid). */
function fmtAxis(n: number): string {
  if (Number.isInteger(n)) return n.toFixed(0);
  return n.toFixed(1);
}

/** HTML-overlay axis labels around the curve canvas. The SVG fills
 *  the inner cell of a CSS grid; Y labels live in the left track,
 *  X labels in the bottom track. Labels are HTML <span>, NOT
 *  SVG <text>, because the SVG uses `preserveAspectRatio="none"`
 *  to stretch the curve+grid to fill the cell, which would distort
 *  text glyphs too. HTML labels stay at their CSS font size and
 *  remain legible at any cell aspect ratio.
 *
 *  Y labels: always min (bottom), max (top), midpoint (middle).
 *  If `0` falls strictly inside the range and isn't already the
 *  midpoint, a fourth label at `0` is added at its actual position
 *  — so ranges like Index `-3..10` show `-3 / 0 / 3.5 / 10`, making
 *  the value=0 baseline easy to find visually.
 *
 *  X labels: 0 / 25 / 50 / 75 / 100 (time percentage), fixed. */
export function CanvasWithAxisLabels({
  yMin,
  yMax,
  children,
  onGutterPointerDown,
}: {
  yMin: number;
  yMax: number;
  children: React.ReactNode;
  /** CRV: a primary pointerdown landing in a label gutter (outside the plot
   *  SVG) routes here so the curve marquee can start from the margins. */
  onGutterPointerDown?: (e: React.PointerEvent) => void;
}) {
  // pct = fraction of the grid box height measured from the TOP.
  // value=yMax → pct=0 (top), value=yMin → pct=1 (bottom).
  const yLabels: Array<{ key: string; value: number; pct: number }> = [
    { key: "max", value: yMax, pct: 0 },
    { key: "min", value: yMin, pct: 1 },
    { key: "mid", value: (yMax + yMin) / 2, pct: 0.5 },
  ];
  // Add a "0" label when 0 is strictly inside the range and the
  // midpoint isn't already 0 (avoids two labels overlapping).
  if (yMin < 0 && yMax > 0 && Math.abs((yMax + yMin) / 2) > 1e-6) {
    const zeroPct = yMax / (yMax - yMin);
    yLabels.push({ key: "zero", value: 0, pct: zeroPct });
  }

  return (
    <div
      data-testid="curve-canvas-with-axes"
      className="grid h-full w-full"
      onPointerDown={(e) => {
        // CRV gutter-marquee: a primary press landing OUTSIDE the plot SVG
        // (in a label gutter) starts a marquee via the parent. A press inside
        // the SVG belongs to the plot's own handlers (whose backdrop also
        // stopPropagations, so this guard is belt-and-suspenders).
        if (e.button !== 0) return;
        if ((e.target as Element).closest('[data-testid="curve-editor-svg"]') !== null) return;
        onGutterPointerDown?.(e);
      }}
      // Wider Y-label column (36 → 36px, was 32px) gives endpoint-key
      // circles that extend past the grid via `overflow="visible"`
      // (≈5px radius) breathing room before they crowd the labels.
      // Taller X-label row (22px, was 18px) does the same vertically.
      //
      // Both tracks use `minmax(0, …)` instead of bare `1fr` / `36px`
      // so the SVG inside the canvas cell can't push the row to its
      // intrinsic aspect-ratio height (`preserveAspectRatio="none"`
      // + no explicit height → browser falls back to `viewBox`
      // 600×300 aspect → 2443px wide canvas would want 1221px tall).
      // The `0` lower bound is the only mechanism that lets the grid
      // shrink the cell below the SVG's intrinsic content size.
      style={{ gridTemplateColumns: "36px minmax(0, 1fr)", gridTemplateRows: "minmax(0, 1fr) 22px" }}
    >
      {/* Y-axis label column. Labels are absolutely-positioned within
          this cell at their respective `pct` so they align with grid
          rows even when the cell is stretched. `pr-2` pulls labels
          4px left of the SVG edge so an endpoint-key circle (~5px
          radius) extending leftward doesn't sit on top of the label
          text. */}
      {/* pointer-events-none: axis labels are decorative — without this an
          endpoint key (value 0/1, drawn into the label gutter via the SVG's
          overflow:visible) gets its click stolen by the label span, which
          (being outside the plot SVG) the grid's onPointerDown treats as a
          gutter press → starts a marquee instead of selecting the key. With
          the labels click-through, the press reaches the SVG key; an empty
          gutter press still falls through to the grid → marquee (unchanged). */}
      <div className="relative pointer-events-none" style={{ gridColumn: 1, gridRow: 1 }}>
        {yLabels.map((l) => (
          <span
            key={l.key}
            className="absolute pr-2 text-[10px] leading-none text-text-2"
            style={{
              top: `${l.pct * 100}%`,
              right: 0,
              transform:
                l.pct === 0
                  ? "translateY(0)"
                  : l.pct === 1
                    ? "translateY(-100%)"
                    : "translateY(-50%)",
              whiteSpace: "nowrap",
            }}
          >
            {fmtAxis(l.value)}
          </span>
        ))}
      </div>
      {/* SVG cell — receives the grid+curve children unchanged.
          `min-h-0` + `min-w-0` are required so the cell can actually
          shrink below the SVG's intrinsic aspect-ratio content-size
          (in concert with the row template's `minmax(0, …)`).
          NOT `overflow-hidden`: the SVG sets `overflow="visible"`
          specifically so endpoint key circles at time=0 / time=100 /
          value=min / value=max can draw their full body (rather than
          being bisected by the cell edge). Clipping here would
          re-introduce the half-moon corner keys the prior polish
          session fixed. */}
      <div style={{ gridColumn: 2, gridRow: 1 }} className="min-w-0 min-h-0">
        {children}
      </div>
      {/* X-axis labels — 5 fixed stops at 0/25/50/75/100% of the
          time range. End labels (0 and 100) anchor at the cell
          edges so they don't clip; intermediates centre on their
          percentage. */}
      <div className="relative pointer-events-none" style={{ gridColumn: 2, gridRow: 2 }}>
        {[0, 25, 50, 75, 100].map((t) => (
          <span
            key={`xl-${t}`}
            className="absolute pt-1.5 text-[10px] leading-none text-text-2"
            style={{
              left: `${t}%`,
              transform:
                t === 0
                  ? "translateX(0)"
                  : t === 100
                    ? "translateX(-100%)"
                    : "translateX(-50%)",
              whiteSpace: "nowrap",
            }}
          >
            {/* Only the rightmost tick carries the % suffix — putting
                it on every label would make the axis read busy. The
                100% anchor establishes that the whole axis is a
                percentage range. */}
            {t === 100 ? `${t}%` : t}
          </span>
        ))}
      </div>
    </div>
  );
}

export function CurveEditorPanel({ bridge }: Props) {
  const [selectedId, setSelectedId] = useState<number | null>(null);
  const [tracks, setTracks] = useState<TrackDto[] | null>(null);
  // The emitter id the CURRENT `tracks` belong to — set together with `tracks`
  // so it never leads the async fetch the way `selectedId` does. Drives the
  // curve morph's switch-vs-edit decision (a change ⇒ emitter switch ⇒ keys glide).
  const [tracksOwnerId, setTracksOwnerId] = useState<number | null>(null);
  const [visible, setVisible] = useState<Record<string, boolean>>(defaultVisibility);
  // Focus channel — session-scoped. Defaults to "red", the first
  // channel visible by default. Picking "scale" (the first row in
  // display order) would land the focus on a hidden channel and
  // trigger the auto-recover effect on first paint; starting on a
  // visible channel avoids that one-tick churn.
  const [focusChannel, setFocusChannel] = useState<string>("red");
  const [mode, setMode] = useState<EditMode>("select");
  // CRV: handle to start a marquee from the axis-label gutters (see
  // CanvasWithAxisLabels.onGutterPointerDown wiring below).
  const curveRef = useRef<CurveMarqueeHandle>(null);
  // Selection state — keyed by key TIME (not array index). Per focus
  // channel; cleared on focus change.
  const [selectedKeyTimes, setSelectedKeyTimes] = useState<Set<number>>(
    () => new Set(),
  );
  // sticky optimistic override — see lessons.md.
  const [optimisticSelected, setOptimisticSelected] = useState<
    { time: number; value: number } | null
  >(null);
  // Live-drag state: populated on every pointer-move during an
  // active key drag (once movement crosses DRAG_SLOP in the
  // renderer). Cleared by `handleKeyDragEnd` (when the drag commits)
  // and `handleKeyDragCancel` (when the drag is aborted). The
  // Time / Value spinners prefer this over the committed selection
  // value, so the user sees the key's in-flight position update
  // continuously as they drag.
  const [liveDrag, setLiveDrag] = useState<
    { keyTime: number; time: number; value: number } | null
  >(null);
  // Live group-drag delta — set on every group-drag move so the
  // multi-select AVERAGE spinners track the shift in real time (the
  // group analogue of `liveDrag`). Null when no group drag is active.
  const [liveGroup, setLiveGroup] = useState<{ dTime: number; dValue: number } | null>(null);
  const [keyContextMenu, setKeyContextMenu] = useState<
    { time: number; isBorder: boolean; x: number; y: number } | null
  >(null);

  // Track which id we last fetched for, so a late-arriving response
  // for a stale selection doesn't clobber current data.
  const inFlightFor = useRef<number | null>(null);

  // Snapshot seed + live selection subscription.
  useEffect(() => {
    let cancelled = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((snap) => {
        if (cancelled) return;
        setSelectedId(snap.selectedEmitterId);
      })
      .catch(() => { /* ignore — placeholder branch covers it */ });
    return () => { cancelled = true; };
  }, [bridge]);

  useEffect(() => {
    const off = bridge.on("emitters/selected", (e) => {
      setSelectedId(e.payload.id);
    });
    return off;
  }, [bridge]);

  // Track fetch + tree-mutation re-fetch. Stale-response guard via
  // inFlightFor matches the pattern from the deleted EmitterPropertyPanel.
  useEffect(() => {
    if (selectedId === null) {
      setTracks(null);
      setTracksOwnerId(null);
      inFlightFor.current = null;
      return;
    }
    let cancelled = false;
    const fetchTracks = (id: number) => {
      inFlightFor.current = id;
      bridge
        .request({ kind: "emitters/get-tracks", params: { id } })
        .then((res) => {
          if (cancelled) return;
          if (inFlightFor.current !== id) return;
          setTracks(res.tracks);
          setTracksOwnerId(id);
        })
        .catch(() => {
          if (cancelled) return;
          if (inFlightFor.current !== id) return;
          setTracks([]);
          setTracksOwnerId(id);
        });
    };
    fetchTracks(selectedId);
    const off = bridge.on("emitters/tree/changed", () => {
      if (selectedId !== null) fetchTracks(selectedId);
    });
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge, selectedId]);

  // Resolve the focus channel's track + range.
  const focusedChannel = useMemo(
    () => CHANNELS.find((c) => c.id === focusChannel) ?? CHANNELS[0]!,
    [focusChannel],
  );
  const focusedTrack = useMemo<TrackDto | null>(() => {
    if (tracks === null) return null;
    return tracks.find((t) => t.name === focusedChannel.trackName) ?? null;
  }, [tracks, focusedChannel]);

  // Atlas eligibility (): the picker only auto-opens when the emitter's
  // texture is an atlas (gridSide ≥ 2). CurveEditorPanel doesn't otherwise
  // read emitter properties, so fetch just `textureSize` keyed on the
  // selection. Default 1 (ineligible) until the fetch resolves; a failed /
  // unanswered fetch leaves it ineligible so the controller stays inert.
  const [textureSize, setTextureSize] = useState<number>(1);
  useEffect(() => {
    if (selectedId === null) { setTextureSize(1); return; }
    let live = true;
    bridge
      .request({ kind: "emitters/get-properties", params: { id: selectedId } })
      .then((res) => { if (live) setTextureSize(res.properties.textureSize); })
      .catch(() => { /* leave at default (ineligible) */ });
    return () => { live = false; };
  }, [bridge, selectedId]);
  const atlasEligible = isAtlasEligible(textureSize);

  // Publish the curve-editor-owned slice of atlas context (focus / selection /
  // emitter / interpolation). `frame` is the shared floor(value) when every
  // selected key floors to the same integer, else null.
  // Both `frame` and `keyTimes` are derived from the same resolved set
  // (times that actually exist on the focused track after any emitter switch)
  // so stale selections from a previous emitter can never leak into the picker.
  const lastKeyTimesRef = useRef<number[]>([]);
  useEffect(() => {
    const resolved = [...selectedKeyTimes].filter(
      (t) => focusedTrack?.keys.some((k) => k.time === t),
    );
    const floors = resolved
      .map((t) => focusedTrack?.keys.find((k) => k.time === t)?.value)
      .filter((v): v is number => v !== undefined)
      .map((v) => Math.floor(v));
    const frame = floors.length > 0 && floors.every((f) => f === floors[0]) ? floors[0]! : null;
    // Stabilise the array reference: reuse the previous array when the contents
    // are identical so atlas-context consumers don't re-render unnecessarily
    // on every tree/changed refetch that produces a fresh focusedTrack object.
    const prev = lastKeyTimesRef.current;
    const same =
      prev.length === resolved.length && prev.every((v, i) => v === resolved[i]);
    const resolvedStable = same ? prev : resolved;
    lastKeyTimesRef.current = resolvedStable;
    publishAtlasContext({
      emitterId: selectedId,
      focusedTrack: focusedChannel.trackName,
      interpolation: focusedTrack?.interpolation ?? null,
      selection: { frame, keyTimes: resolvedStable },
    });
  }, [selectedId, focusedChannel.trackName, focusedTrack, selectedKeyTimes]);

  // Mount the auto-open controller (reads the published context + dock store).
  useAtlasAutoOpen({ atlasEligible });

  // Locked-now flag: when the focus channel is currently a read-only
  // alias of another channel, every edit affordance (Insert mode,
  // interpolation toggle, Delete, drag, marquee) should be disabled.
  // Lock dropdown itself stays enabled so the user can unlock.
  const focusLocked = focusedTrack !== null && focusedTrack.lockedTo !== null;

  // Read-only mirror: never leave Insert active on a locked focus —
  // covers lock-while-insert AND focus-switch-onto-locked (spec §3.3).
  useEffect(() => {
    if (focusLocked && mode === "insert") setMode("select");
  }, [focusLocked, mode]);

  // an-audit-finding robustness: after a tree/changed refetch the engine's float32 key
  // times can land an ULP away from the times we optimistically put in
  // `selectedKeyTimes` (e.g. a group move commits N keys; the real engine
  // rounds each to float32). Without this, every moved key EXCEPT the
  // dragged one falls out of the `selectedKeyTimes.has(p.time)` membership
  // test and loses its highlight. Snap each selected time onto the nearest
  // actual key time within a tiny tolerance (float32 drift is ~1e-5; key
  // spacing is ≥ ~1, so this never grabs a different key).
  useEffect(() => {
    if (focusedTrack === null) return;
    const SNAP_EPS = 1e-3;
    setSelectedKeyTimes((prev) => {
      if (prev.size === 0) return prev;
      const keyTimes = focusedTrack.keys.map((k) => k.time);
      let changed = false;
      const next = new Set<number>();
      for (const t of prev) {
        if (keyTimes.includes(t)) {
          next.add(t);
          continue;
        }
        let best = t;
        let bestD = Infinity;
        for (const kt of keyTimes) {
          const d = Math.abs(kt - t);
          if (d < bestD) {
            bestD = d;
            best = kt;
          }
        }
        if (bestD > 0 && bestD <= SNAP_EPS) {
          next.add(best);
          changed = true;
        } else {
          next.add(t);
        }
      }
      return changed ? next : prev;
    });
  }, [focusedTrack]);

  // Unified Y-axis range across all VISIBLE channels' tracks. When
  // multiple channels are visible the canvas extends to encompass
  // the most extreme keys on any of them — so turning on Scale-at-20
  // alongside RGB stretches the canvas to 0..20 and the RGB curves
  // squish near the bottom. Falls back to {0, 1} when nothing is
  // visible (which also covers the no-emitter / no-tracks branch).
  const unifiedRange = useMemo<{ min: number; max: number }>(() => {
    if (tracks === null) return { min: 0, max: 1 };
    let min = Number.POSITIVE_INFINITY;
    let max = Number.NEGATIVE_INFINITY;
    for (const t of tracks) {
      const channel = CHANNELS.find((c) => c.trackName === t.name);
      if (channel === undefined) continue;
      if (!(visible[channel.id] ?? channel.defaultOn)) continue;
      const r = valueRangeForTrack(t);
      if (r.min < min) min = r.min;
      if (r.max > max) max = r.max;
    }
    if (!Number.isFinite(min) || !Number.isFinite(max)) return { min: 0, max: 1 };
    return { min, max };
  }, [tracks, visible]);


  // Border keys on the focus track (first + last in time order).
  const borderKeyTimes = useMemo<ReadonlySet<number>>(() => {
    if (focusedTrack === null || focusedTrack.keys.length === 0) return new Set();
    const ks = focusedTrack.keys;
    return new Set<number>([ks[0]!.time, ks[ks.length - 1]!.time]);
  }, [focusedTrack]);

  // Solo an exclusive channel (Scale or Index): turning it on hides
  // every other channel. Selecting a different curve via row click
  // exits solo by hiding the exclusive one(s) and showing the target.
  // Checkbox toggles on non-exclusive channels do NOT exit solo — the
  // checkbox is granular control; the user can manually keep an
  // exclusive channel alongside others for unified-range comparison.
  const enableExclusively = useCallback((soloId: string) => {
    const next: Record<string, boolean> = {};
    for (const ch of CHANNELS) next[ch.id] = ch.id === soloId;
    setVisible(next);
  }, []);

  // Row-click handler: set focus + ensure visibility. Clears selection +
  // optimistic override when switching focus to a different channel.
  const handleRowClick = useCallback((id: string) => {
    setFocusChannel((prev) => {
      if (prev !== id) {
        setSelectedKeyTimes(new Set());
        setOptimisticSelected(null);
      }
      return id;
    });
    setVisible((v) => {
      if (EXCLUSIVE_CHANNELS.has(id) && !v[id]) {
        // Off→on click on an exclusive channel (Scale/Index) enters solo.
        const next: Record<string, boolean> = {};
        for (const ch of CHANNELS) next[ch.id] = ch.id === id;
        return next;
      }
      if (!EXCLUSIVE_CHANNELS.has(id)) {
        const soloOn = [...EXCLUSIVE_CHANNELS].some((e) => v[e]);
        if (soloOn) {
          // Selecting a non-exclusive curve exits solo: hide every
          // exclusive channel, show the target.
          const next = { ...v, [id]: true };
          for (const e of EXCLUSIVE_CHANNELS) next[e] = false;
          return next;
        }
      }
      return v[id] ? v : { ...v, [id]: true };
    });
  }, []);

  // Auto-recover focus when the user hides the currently-focused
  // channel via its checkbox. Otherwise the focus layer would
  // disappear from the canvas (the focus channel filtered out of the
  // `layers` set in MultiChannelCurves) and the user would see no
  // interactive surface. Pick the first visible channel as the new
  // focus; if none are visible, leave focus where it is (the
  // placeholder branch covers it via the no-selection state, and
  // re-checking any channel restores focus).
  useEffect(() => {
    if (visible[focusChannel]) return;
    const nextVisible = CHANNELS.find((c) => visible[c.id]);
    if (nextVisible === undefined) return;
    setFocusChannel(nextVisible.id);
    setSelectedKeyTimes(new Set());
    setOptimisticSelected(null);
  }, [visible, focusChannel]);

  // ── Curve interactions ────────────────────────────────────────────

  const handleKeyClick = useCallback(
    (time: number, event: React.MouseEvent | React.PointerEvent) => {
      const additive = event.ctrlKey || event.metaKey;
      setOptimisticSelected(null);
      setSelectedKeyTimes((prev) => {
        if (additive) {
          const next = new Set(prev);
          if (next.has(time)) next.delete(time);
          else next.add(time);
          return next;
        }
        return new Set([time]);
      });
    },
    [],
  );

  const handleCanvasClick = useCallback(() => {
    setOptimisticSelected(null);
    setSelectedKeyTimes((prev) => (prev.size === 0 ? prev : new Set()));
  }, []);

  const handleCanvasMarqueeSelect = useCallback(
    (times: number[], shift: boolean) => {
      setOptimisticSelected(null);
      setSelectedKeyTimes((prev) => {
        if (shift) {
          if (times.length === 0) return prev;
          const next = new Set(prev);
          for (const t of times) next.add(t);
          return next;
        }
        return new Set(times);
      });
    },
    [],
  );

  const handleKeyDragEnd = useCallback(
    (keyTime: number, newTime: number, newValue: number) => {
      if (selectedId === null || focusedTrack === null || focusLocked) return;
      // The engine stores track key times as float32; a JS-side
      // double like 49.476439790575924 comes back from the bridge
      // refetch as 49.476440429... — equal at float32 precision but
      // not under ===. Pre-round our committed time to float32 here
      // so the value we put in `selectedKeyTimes` / optimistic
      // tracks / optimistic spinner IS the same value the engine
      // returns on the next refetch. Without this, the trailing
      // tree/changed refetch silently drifts the focused track's
      // key time by ~1e-6, the renderer's
      // `selectedKeyTimes.has(p.time)` check misses, and the key
      // paints unselected (the bug this fixes).
      const engineNewTime = Math.fround(newTime);
      setSelectedKeyTimes(new Set([engineNewTime]));
      setOptimisticSelected({ time: engineNewTime, value: newValue });
      setLiveDrag(null);
      setTracks((prev) => {
        if (prev === null) return prev;
        return prev.map((t) => {
          if (t.name !== focusedChannel.trackName) return t;
          const keys = t.keys
            .map((k) => (k.time === keyTime ? { time: engineNewTime, value: newValue } : k))
            .sort((a, b) => a.time - b.time);
          return { ...t, keys };
        });
      });
      void bridge.request({
        kind: "emitters/set-track-key",
        params: {
          id: selectedId,
          track: focusedChannel.trackName,
          oldTime: keyTime,
          newTime,
          newValue,
        },
      }).catch(() => { /* silent — re-fetch on tree/changed */ });
    },
    [bridge, selectedId, focusedTrack, focusLocked, focusedChannel.trackName],
  );

  const handleKeyDragStart = useCallback(
    (keyTime: number) => {
      // Pre-select the dragged key. This is what makes a select-mode
      // drag show the selected ring while the user holds the key —
      // the renderer's `selectedKeyTimes.has(p.time)` check is keyed
      // off the rendered point's time, which stays equal to the
      // dragged key's start time throughout the drag (the renderer
      // only shifts the rendered POSITION via dragRef, not the
      // logical time). Sticky-optimistic is cleared so it can't
      // pull stale spinner values over the incoming live-drag data.
      // an-audit-finding: keep a multi-selection intact when the grabbed key is one of
      // its members — that's the start of a group drag, not a re-select.
      setSelectedKeyTimes((prev) =>
        prev.has(keyTime) && prev.size > 1 ? prev : new Set([keyTime]),
      );
      setOptimisticSelected(null);
    },
    [],
  );

  const handleKeyDragMove = useCallback(
    (keyTime: number, currentTime: number, currentValue: number) => {
      setLiveDrag({ keyTime, time: currentTime, value: currentValue });
    },
    [],
  );

  // Group-drag live move: stash the live (dTime, dValue) so the
  // multi-select average spinners reflect the shift mid-drag.
  const handleGroupDragMove = useCallback((dTime: number, dValue: number) => {
    setLiveGroup({ dTime, dValue });
  }, []);

  const handleKeyDragCancel = useCallback(() => {
    setLiveDrag(null);
    setLiveGroup(null);
  }, []);

  const handleCanvasAdd = useCallback(
    (time: number, value: number) => {
      if (selectedId === null || focusLocked) return;
      void bridge.request({
        kind: "emitters/add-track-key",
        params: { id: selectedId, track: focusedChannel.trackName, time, value },
      }).then((res) => {
        const insertedTime = res.time ?? time;
        setSelectedKeyTimes(new Set([insertedTime]));
        setOptimisticSelected({ time: insertedTime, value });
      }).catch(() => { /* silent */ });
    },
    [bridge, selectedId, focusLocked, focusedChannel.trackName],
  );

  // Delete — filters border keys + fires bridge call.
  const handleDelete = useCallback(() => {
    if (selectedId === null || focusLocked) return;
    const candidates: number[] = [];
    for (const t of selectedKeyTimes) {
      if (!borderKeyTimes.has(t)) candidates.push(t);
    }
    if (candidates.length === 0) return;
    void bridge.request({
      kind: "emitters/delete-track-keys",
      params: { id: selectedId, track: focusedChannel.trackName, times: candidates },
    }).then(() => {
      setSelectedKeyTimes(new Set());
      setOptimisticSelected(null);
    }).catch(() => { /* silent */ });
  }, [bridge, selectedId, focusLocked, focusedChannel.trackName, selectedKeyTimes, borderKeyTimes]);

  const handleInterpolationClick = useCallback(
    (kind: InterpolationType) => {
      if (selectedId === null || focusedTrack === null) return;
      if (focusedTrack.interpolation === kind) return;
      void bridge.request({
        kind: "emitters/set-track-interpolation",
        params: { id: selectedId, track: focusedChannel.trackName, interpolation: kind },
      }).catch(() => { /* silent */ });
    },
    [bridge, selectedId, focusedChannel.trackName, focusedTrack],
  );

  // ── Spinner sync ─────────────────────────────────────────────────

  const singleSelected = useMemo<{ time: number; value: number; isBorder: boolean } | null>(() => {
    // Live drag wins — while the user is mid-drag the spinner
    // tracks the dragged key's in-flight (time, value) regardless of
    // what the committed selection currently holds. `keyTime` is the
    // ORIGINAL time of the dragged key (border-ness is keyed off
    // that, not the live time, since border-ness is a structural
    // property of the key not its current display time).
    if (liveDrag !== null) {
      return {
        time: liveDrag.time,
        value: liveDrag.value,
        isBorder: borderKeyTimes.has(liveDrag.keyTime),
      };
    }
    if (selectedKeyTimes.size !== 1) return null;
    const onlyTime = selectedKeyTimes.values().next().value as number;
    if (optimisticSelected !== null && optimisticSelected.time === onlyTime) {
      return {
        time: optimisticSelected.time,
        value: optimisticSelected.value,
        isBorder: borderKeyTimes.has(optimisticSelected.time),
      };
    }
    if (focusedTrack === null) return null;
    const key = focusedTrack.keys.find((k) => k.time === onlyTime);
    if (key === undefined) return null;
    return {
      time: key.time,
      value: key.value,
      isBorder: borderKeyTimes.has(key.time),
    };
  }, [liveDrag, selectedKeyTimes, focusedTrack, borderKeyTimes, optimisticSelected]);

  // F8: multi-key selection (>1) shows the AVERAGE time/value of the
  // selected keys; editing shifts the whole group by the delta
  // (preserve spread). Mirrors legacy TrackEditor.cpp / CurveEditor.cpp
  // CurveEditor_MoveSelection: the average is over ALL selected keys
  // (borders included), Time is editable as long as ≥1 interior
  // (non-border) key is selected, and a Time shift moves only the
  // interior keys (borders are pinned in time but DO shift in value).
  const multiSelected = useMemo<
    { keys: { time: number; value: number }[]; avgTime: number; avgValue: number; editable: boolean } | null
  >(() => {
    if (selectedKeyTimes.size <= 1 || focusedTrack === null) return null;
    const keys = focusedTrack.keys.filter((k) => selectedKeyTimes.has(k.time));
    if (keys.length <= 1) return null;
    let anyInterior = false;
    for (const k of keys) {
      if (!borderKeyTimes.has(k.time)) anyInterior = true;
    }
    // During a group drag, average over the LIVE shifted positions
    // (same per-key transform the commit uses) so the spinners track
    // the drag; otherwise average the committed keys. Mean is over the
    // same selected-key set either way.
    const positions =
      liveGroup !== null
        ? computeGroupMoves(
            focusedTrack.keys, selectedKeyTimes, borderKeyTimes,
            liveGroup.dTime, liveGroup.dValue,
            spinnerBoundsForTrack(focusedChannel.trackName),
          ).map((m) => ({ time: m.newTime, value: m.newValue }))
        : keys;
    let st = 0;
    let sv = 0;
    for (const p of positions) {
      st += p.time;
      sv += p.value;
    }
    const n = positions.length || 1;
    return { keys, avgTime: st / n, avgValue: sv / n, editable: anyInterior };
  }, [selectedKeyTimes, focusedTrack, borderKeyTimes, liveGroup, focusedChannel.trackName]);

  // Apply a (dTime, dValue) shift to every selected key. Borders are
  // pinned in time (value still shifts); interior keys clamp to stay
  // strictly inside the endpoints (legacy eps = range/10000). Values
  // clamp to the channel's engine bounds. The bridge has no batch move,
  // so we issue ordered single-key set-track-key calls — descending
  // oldTime for a rightward shift, ascending for leftward — so each
  // find(oldTime) on the host's multiset never resolves to a key a
  // prior move just parked there.
  const applyGroupShift = useCallback(
    (dTime: number, dValue: number) => {
      if (selectedId === null || focusedTrack === null) return;
      const keys = focusedTrack.keys;
      if (keys.length === 0) return;
      const sb = spinnerBoundsForTrack(focusedChannel.trackName);
      // NOTE: the clamped nt/nv are the values the engine commits.
      // CurveEditor.tsx (~:1499-1500) records the same clamped values
      // into morphSuppressRef; movesMatch() in use-curve-morph.ts
      // verifies them. `computeGroupMoves` is the single source of this
      // clamp logic (shared with the live spinner average) — keep it in
      // sync with the morph recorder.
      const moves = computeGroupMoves(
        keys, selectedKeyTimes, borderKeyTimes, dTime, dValue, sb,
      ).map((m) => ({
        oldTime: m.oldTime,
        wireTime: m.newTime,
        engineTime: Math.fround(m.newTime),
        newValue: m.newValue,
      }));
      // Optimistic overlay (same idiom as handleKeyDragEnd).
      const moveMap = new Map(moves.map((m) => [m.oldTime, m]));
      setTracks((prev) =>
        prev === null
          ? prev
          : prev.map((t) =>
              t.name !== focusedChannel.trackName
                ? t
                : {
                    ...t,
                    keys: t.keys
                      .map((k) => {
                        const m = moveMap.get(k.time);
                        return m ? { time: m.engineTime, value: m.newValue } : k;
                      })
                      .sort((a, b) => a.time - b.time),
                  },
            ),
      );
      setSelectedKeyTimes(new Set(moves.map((m) => m.engineTime)));
      setOptimisticSelected(null);
      const ordered = [...moves].sort((a, b) =>
        dTime > 0 ? b.oldTime - a.oldTime : a.oldTime - b.oldTime,
      );
      for (const m of ordered) {
        void bridge
          .request({
            kind: "emitters/set-track-key",
            params: {
              id: selectedId,
              track: focusedChannel.trackName,
              // Commit the float32 (engineTime) value, NOT the raw double
              // wireTime, so the time the engine stores + returns on refetch
              // is EXACTLY what we put in selectedKeyTimes — otherwise the
              // moved keys lose their selected highlight (an-audit-finding polish).
              oldTime: m.oldTime,
              newTime: m.engineTime,
              newValue: m.newValue,
            },
          })
          .catch(() => { /* silent — re-fetch on tree/changed */ });
      }
    },
    [bridge, selectedId, focusedTrack, focusedChannel.trackName, selectedKeyTimes, borderKeyTimes],
  );

  // an-audit-finding: a multi-key canvas drag commits as a single group shift, reusing
  // the same path the Time/Value spinners use for multi-selections.
  const handleGroupDragEnd = useCallback(
    (dTime: number, dValue: number) => {
      // Drag is over — drop the live delta so the average snaps to the
      // committed (optimistic) positions instead of the in-flight shift.
      setLiveGroup(null);
      if (focusLocked || (dTime === 0 && dValue === 0)) return;
      applyGroupShift(dTime, dValue);
    },
    [focusLocked, applyGroupShift],
  );

  // Spinner display values + enablement. Single-key wins; otherwise the
  // multi-key average. Time is disabled for a border single-key, and in
  // multi-select when no interior key is selected (all-borders).
  const spinnerTimeValue = singleSelected?.time ?? multiSelected?.avgTime ?? 0;
  const spinnerValueValue = singleSelected?.value ?? multiSelected?.avgValue ?? 0;
  const spinnersDisabled =
    selectedId === null || (singleSelected === null && multiSelected === null) || focusLocked;
  const timeSpinnerDisabled =
    spinnersDisabled ||
    (singleSelected !== null
      ? singleSelected.isBorder
      : multiSelected !== null
        ? !multiSelected.editable
        : true);

  const handleTimeSpinner = useCallback(
    (nextTime: number) => {
      if (selectedId === null || focusedTrack === null || focusLocked) return;
      // F8: multi-key → shift the group's times by (new avg − old avg).
      if (multiSelected !== null) {
        if (!multiSelected.editable) return;
        const dTime = nextTime - multiSelected.avgTime;
        if (dTime === 0) return;
        applyGroupShift(dTime, 0);
        return;
      }
      if (singleSelected === null) return;
      if (singleSelected.isBorder) return;
      if (nextTime === singleSelected.time) return;
      const oldTime = singleSelected.time;
      const keys = focusedTrack.keys;
      const idx = keys.findIndex((k) => k.time === oldTime);
      let clampedTime = nextTime;
      if (idx > 0 && idx < keys.length - 1) {
        const eps = 1e-4;
        clampedTime = Math.max(
          keys[idx - 1]!.time + eps,
          Math.min(keys[idx + 1]!.time - eps, clampedTime),
        );
      }
      setSelectedKeyTimes(new Set([clampedTime]));
      setOptimisticSelected({ time: clampedTime, value: singleSelected.value });
      void bridge.request({
        kind: "emitters/set-track-key",
        params: {
          id: selectedId,
          track: focusedChannel.trackName,
          oldTime,
          newTime: clampedTime,
          newValue: singleSelected.value,
        },
      }).catch(() => { /* silent */ });
    },
    [multiSelected, applyGroupShift, singleSelected, bridge, selectedId, focusLocked, focusedChannel.trackName, focusedTrack],
  );

  const handleValueSpinner = useCallback(
    (nextValue: number) => {
      if (selectedId === null || focusLocked) return;
      // F8: multi-key → shift the group's values by (new avg − old avg).
      if (multiSelected !== null) {
        const dValue = nextValue - multiSelected.avgValue;
        if (dValue === 0) return;
        applyGroupShift(0, dValue);
        return;
      }
      if (singleSelected === null) return;
      if (nextValue === singleSelected.value) return;
      // No clamp to the *display* range — that range is derived from
      // current keys and we want it to GROW when the user inputs a
      // larger / more negative value. The Spinner already clamps to
      // the channel's engine-allowed bounds (spinnerBoundsForTrack).
      setOptimisticSelected({ time: singleSelected.time, value: nextValue });
      void bridge.request({
        kind: "emitters/set-track-key",
        params: {
          id: selectedId,
          track: focusedChannel.trackName,
          oldTime: singleSelected.time,
          newTime: singleSelected.time,
          newValue: nextValue,
        },
      }).catch(() => { /* silent */ });
    },
    [multiSelected, applyGroupShift, singleSelected, bridge, selectedId, focusLocked, focusedChannel.trackName],
  );

  // ── Delete keyboard handler (window-scoped, TYPING_TAGS guard) ────
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== "Delete") return;
      const target = e.target as HTMLElement | null;
      if (target !== null && TYPING_TAGS.has(target.tagName)) return;
      if (selectedKeyTimes.size === 0) return;
      e.preventDefault();
      handleDelete();
    };
    window.addEventListener("keydown", onKeyDown);
    return () => { window.removeEventListener("keydown", onKeyDown); };
  }, [handleDelete, selectedKeyTimes]);

  // Lock-to options.
  const lockToOptions = LOCK_TO_OPTIONS[focusedChannel.trackName];
  const lockToDisabled = lockToOptions.length <= 1;

  // Lock-to dropdown value: derived from the host's TrackDto.lockedTo
  // (NOT from local state — pointer-equality on the engine side is
  // the source of truth, and a successful set-track-lock dispatches
  // a tree/changed that triggers our get-tracks refetch). Display
  // values are the capitalised label strings used by LOCK_TO_OPTIONS
  // ("None", "Red", "Green", "Blue").
  const lockToValue = useMemo<string>(() => {
    if (focusedTrack?.lockedTo == null) return "None";
    return focusedTrack.lockedTo[0]!.toUpperCase() + focusedTrack.lockedTo.slice(1);
  }, [focusedTrack]);

  // Dispatch set-track-lock when the user picks a new lock target.
  // Maps the display label ("Red") back to the wire TrackName ("red")
  // and "None" → null. The C++ host validates further (only RGBA
  // channels, only earlier-channel targets); invalid choices land as
  // an unlock per the dispatcher.
  const handleLockToChange = useCallback((next: string) => {
    if (selectedId === null) return;
    if (next === lockToValue) return; // no-op
    const lockTo: TrackName | null = next === "None"
      ? null
      : (next.toLowerCase() as TrackName);
    void bridge.request({
      kind: "emitters/set-track-lock",
      params: {
        id: selectedId,
        channel: focusedChannel.trackName,
        lockTo,
      },
    });
    // Selection makes no sense while a track is locked; clear it so
    // unlock doesn't snap back to a phantom previous selection.
    setSelectedKeyTimes(new Set());
    setOptimisticSelected(null);
  }, [bridge, selectedId, focusedChannel.trackName, lockToValue]);

  // Delete-button disabled state for the toolbar.
  const deletableCount = useMemo(() => {
    let n = 0;
    for (const t of selectedKeyTimes) if (!borderKeyTimes.has(t)) n++;
    return n;
  }, [selectedKeyTimes, borderKeyTimes]);
  const deleteDisabled = selectedId === null || deletableCount === 0 || focusLocked;

  // Bridge mutation guards — disable interp buttons when there's no
  // selected emitter, no focused track, OR the focused track is
  // currently locked to another channel (read-only).
  const interpDisabled = selectedId === null || focusedTrack === null || focusLocked;

  // ── Key clipboard (an-audit-finding) ─────────────────────────────────────────
  // Copy the selected keys' {time,value} (borders included — legacy
  // CopyKeys copies the whole selection). Returns whether anything was
  // copied so Cut can bail before deleting.
  const handleCopyKeys = useCallback((): boolean => {
    if (focusedTrack === null || selectedKeyTimes.size === 0) return false;
    const keys = focusedTrack.keys
      .filter((k) => selectedKeyTimes.has(k.time))
      .map((k) => ({ time: k.time, value: k.value }));
    if (keys.length === 0) return false;
    setCurveKeysClipboard(keys);
    return true;
  }, [focusedTrack, selectedKeyTimes]);

  // Cut = Copy then Delete (border keys are filtered server-side by
  // delete-track-keys, matching legacy WM_CUT → WM_CLEAR).
  const handleCutKeys = useCallback(() => {
    if (!handleCopyKeys()) return;
    handleDelete();
  }, [handleCopyKeys, handleDelete]);

  // Paste re-adds each clipboard key on the FOCUS track via add-track-key
  // (host dedupes by epsilon + returns the real inserted time). We select
  // the returned times so the pasted keys highlight — same auto-select-
  // the-returned-time path handleCanvasAdd uses (and the same reason it
  // survives float32 drift natively,). Blocked on a locked focus.
  const handlePasteKeys = useCallback(() => {
    if (selectedId === null || focusLocked) return;
    const clip = getCurveKeysClipboard();
    if (clip.length === 0) return;
    const track = focusedChannel.trackName;
    setOptimisticSelected(null);
    void Promise.all(
      clip.map((k) =>
        bridge
          .request({
            kind: "emitters/add-track-key",
            params: { id: selectedId, track, time: k.time, value: k.value },
          })
          .then((res) => (res as { time?: number }).time ?? k.time)
          .catch(() => null),
      ),
    ).then((times) => {
      const valid = times.filter((t): t is number => t !== null);
      if (valid.length > 0) setSelectedKeyTimes(new Set(valid));
    });
  }, [bridge, selectedId, focusLocked, focusedChannel.trackName]);

  // Window-scoped Ctrl/Cmd + C / X / V. Window-scoped (not panel-focus-
  // scoped) because clicking an SVG key never moves DOM focus into the
  // panel — same reason the Delete handler above is window-scoped. Two
  // guards keep it from colliding with the emitter tree's own clipboard:
  // a TYPING_TAGS guard, and a tree-origin guard (the tree owns its
  // focus-scoped Ctrl+C/X/V — when the keydown comes from inside it, we
  // stand down).
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (!(e.ctrlKey || e.metaKey)) return;
      const key = e.key.toLowerCase();
      if (key !== "c" && key !== "x" && key !== "v") return;
      const target = e.target as HTMLElement | null;
      if (target !== null) {
        if (TYPING_TAGS.has(target.tagName)) return;
        if (target.closest('[data-testid="emitter-tree"]')) return;
      }
      if (key === "c") {
        if (selectedKeyTimes.size === 0) return;
        e.preventDefault();
        handleCopyKeys();
      } else if (key === "x") {
        if (selectedKeyTimes.size === 0) return;
        e.preventDefault();
        handleCutKeys();
      } else {
        if (getCurveKeysClipboard().length === 0) return;
        e.preventDefault();
        handlePasteKeys();
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => { window.removeEventListener("keydown", onKeyDown); };
  }, [handleCopyKeys, handleCutKeys, handlePasteKeys, selectedKeyTimes]);

  // Read-only mirror (spec §2.1): a locked focus channel gets NO
  // interactive handlers — drag, insert, key-click, marquee, and the
  // key context menu are all selection/mutation gateways. onCanvasClick
  // and onCanvasContextMenu stay wired (clear-selection / mode-drop UX).
  // Renderer-side focusReadOnly gates are the second layer of the same
  // defense (CurveEditor.tsx).
  const interactiveHandlers = focusLocked
    ? {}
    : {
        onKeyClick: handleKeyClick,
        insertMode: mode === "insert",
        onCanvasAdd: handleCanvasAdd,
        onKeyContextMenu: (time: number, isBorder: boolean, x: number, y: number) =>
          setKeyContextMenu({ time, isBorder, x, y }),
        onKeyDragEnd: handleKeyDragEnd,
        onKeyDragStart: handleKeyDragStart,
        onKeyDragMove: handleKeyDragMove,
        onKeyDragCancel: handleKeyDragCancel,
        onGroupDragEnd: handleGroupDragEnd,
        onGroupDragMove: handleGroupDragMove,
        onCanvasMarqueeSelect: handleCanvasMarqueeSelect,
      };

  return (
    <div
      data-testid="curve-editor-panel"
      data-selected-id={selectedId === null ? "null" : String(selectedId)}
      data-focus-channel={focusChannel}
      data-mode={mode}
      data-selected-key-count={selectedKeyTimes.size}
      className="panel h-full w-full"
    >
      <div className="panel-header">
        <span>Curve editor</span>
      </div>
      <div className="curve-editor">
        {/* Edit-affordances toolbar. Lives above .ce-body so the
            `.ce-toolbar` rule in components.css (36px row, padded,
            border-bottom) gives us the design's intended slot. When
            no emitter is selected the toolbar still renders but each
            control disables — the user sees the affordance surface
            without it doing anything until a selection lands. */}
        <div
          data-testid="curve-editor-toolbar"
          className="ce-toolbar"
        >
          {/* Mode toggle (Select / Insert) */}
          {/* T6: disabled buttons fire no pointer events, so the Tip rides an
              inline-block span wrapper. Copy flips to the lock hint while
              the channel is locked, mirroring the Delete button pattern. */}
          <Tip
            content={focusLocked ? "Channel is locked — unlock to edit" : "Select (click a key to select; click empty area to clear)"}
          >
            <span className="inline-block">
              <button
                type="button"
                aria-label="Select tool"
                aria-pressed={mode === "select"}
                data-state={mode === "select" ? "on" : "off"}
                data-testid="ce-tool-select"
                disabled={focusLocked}
                onClick={() => setMode("select")}
                className={
                  mode === "select"
                    ? "grid h-6 w-6 place-items-center rounded border border-accent bg-accent-soft text-accent disabled:cursor-not-allowed disabled:opacity-40"
                    : "grid h-6 w-6 place-items-center rounded border border-border-2 bg-bg-2 text-text-2 hover:border-border-2 disabled:cursor-not-allowed disabled:opacity-40"
                }
              >
                <MousePointer2 className="size-3.5" aria-hidden="true" />
              </button>
            </span>
          </Tip>
          <Tip
            content={focusLocked ? "Channel is locked — unlock to edit" : "Insert (click empty canvas to add a key)"}
          >
            <span className="inline-block">
              <button
                type="button"
                aria-label="Insert tool"
                aria-pressed={mode === "insert"}
                data-state={mode === "insert" ? "on" : "off"}
                data-testid="ce-tool-insert"
                disabled={focusLocked}
                onClick={() => setMode("insert")}
                className={
                  mode === "insert"
                    ? "grid h-6 w-6 place-items-center rounded border border-accent bg-accent-soft text-accent disabled:cursor-not-allowed disabled:opacity-40"
                    : "grid h-6 w-6 place-items-center rounded border border-border-2 bg-bg-2 text-text-2 hover:border-border-2 disabled:cursor-not-allowed disabled:opacity-40"
                }
              >
                <Plus className="size-3.5" aria-hidden="true" />
              </button>
            </span>
          </Tip>

          <span className="mx-1 h-4 w-px bg-panel-2" aria-hidden />

          {/* Interpolation toggle — applies to the focus channel's
              underlying track. */}
          {INTERP_KINDS.map((kind) => {
            const isActive = focusedTrack?.interpolation === kind;
            const label = kind[0]!.toUpperCase() + kind.slice(1);
            return (
              <Tip key={kind} content={`${label} interpolation`}>
                <button
                  type="button"
                  disabled={interpDisabled}
                  aria-label={`Interpolation ${kind}`}
                  aria-pressed={isActive}
                  data-state={isActive ? "on" : "off"}
                  data-testid={`ce-interp-${kind}`}
                  onClick={() => handleInterpolationClick(kind)}
                  className={
                    isActive
                      ? "grid h-6 w-6 place-items-center rounded border border-accent bg-accent-soft text-accent"
                      : "grid h-6 w-6 place-items-center rounded border border-border-2 bg-bg-2 text-text-2 hover:border-border-2 disabled:cursor-not-allowed disabled:opacity-40"
                  }
                >
                  {INTERP_ICONS[kind]}
                </button>
              </Tip>
            );
          })}

          <span className="mx-1 h-4 w-px bg-panel-2" aria-hidden />

          {/* Lock-to combo. Disabled only when the focus channel has
              no possible targets (Red / Scale / Index / Rotation —
              all of which can only be "None"). For Green/Blue/Alpha
              the dropdown stays enabled even while locked so the user
              can change the lock target or unlock. */}
          <label className="text-xs text-text-2" htmlFor="ce-lock-to-trigger">
            Lock to:&nbsp;
          </label>
          <Select.Root
            value={lockToValue}
            onValueChange={handleLockToChange}
            disabled={lockToDisabled || selectedId === null}
          >
            <Select.Trigger
              id="ce-lock-to-trigger"
              data-testid="ce-lock-to-trigger"
              data-locked={focusLocked ? "true" : "false"}
              className="flex h-6 min-w-[80px] items-center justify-between gap-1 rounded border border-border-2 bg-bg-2 px-2 text-xs text-text outline-none hover:border-border-2 focus:border-accent disabled:cursor-not-allowed disabled:opacity-40 data-[locked=true]:border-accent data-[locked=true]:text-accent"
              aria-label="Lock-to track"
            >
              <Select.Value placeholder="None" />
              <Select.Icon>
                <ChevronDown className="size-3 text-text-3" />
              </Select.Icon>
            </Select.Trigger>
            <Select.Portal>
              <Select.Content
                position="popper"
                sideOffset={4}
                className="z-50 min-w-[120px] rounded-md border border-border-2 bg-bg-2 p-1 shadow-xl"
              >
                <Select.Viewport>
                  {lockToOptions.map((opt) => (
                    <Select.Item
                      key={opt}
                      value={opt}
                      data-testid={`ce-lock-to-option-${opt.toLowerCase()}`}
                      className="cursor-pointer rounded px-2 py-0.5 text-xs text-text outline-none data-[highlighted]:bg-accent-soft data-[highlighted]:text-accent"
                    >
                      <Select.ItemText>{opt}</Select.ItemText>
                    </Select.Item>
                  ))}
                </Select.Viewport>
              </Select.Content>
            </Select.Portal>
          </Select.Root>

          {/* Read-only mirror cue: a deliberate info glyph naming the master.
              The dashed curve is the in-canvas signal; this is the worded one.
              Non-interactive span (not a button) — the Tip carries the
              explanation; the aria-label is the always-on accessible name. */}
          {focusLocked && (
            <Tip
              content={`${focusedChannel.label} is locked to ${lockToValue} and shows ${lockToValue}'s curve. Unlock to edit.`}
            >
              <span
                data-testid="ce-lock-glyph"
                role="img"
                aria-label={`${focusedChannel.label} is locked to ${lockToValue} — read-only`}
                className="inline-flex h-6 items-center text-accent"
              >
                <Lock className="size-3.5" aria-hidden="true" />
              </span>
            </Tip>
          )}

          <span className="mx-1 h-4 w-px bg-panel-2" aria-hidden />

          {/* Delete action — useful as a visible affordance even
              with the Delete key wired (discoverability + works in
              browsers with the key intercepted by extensions). */}
          {/* T6: the "Select a non-border key first" hint only matters while
              the button is DISABLED — disabled elements fire no pointer
              events, so the Tip rides an inline-block span wrapper. */}
          <Tip
            content={deleteDisabled ? "Select a non-border key first" : "Delete selected key(s)"}
          >
            <span className="inline-block">
              <button
                type="button"
                disabled={deleteDisabled}
                aria-label="Delete selected keys"
                data-testid="ce-action-delete"
                onClick={handleDelete}
                className={
                  deleteDisabled
                    ? "grid h-6 w-6 place-items-center rounded border border-border bg-bg-2/60 text-text-3"
                    : "grid h-6 w-6 place-items-center rounded border border-border-2 bg-bg-2 text-text-2 hover:border-danger hover:text-danger-fg"
                }
              >
                <Trash2 className="size-3.5" aria-hidden="true" />
              </button>
            </span>
          </Tip>

          <div className="flex-1" />

          {/* Time / Value spinners — populated from the focus channel's
              selected key, or the AVERAGE of the selected keys when >1 is
              selected (F8). Disabled when nothing is selected. A border
              single-key disables Time (value-only edit); a multi-select of
              all-border keys does the same. The Spinner `key` binds to
              track + selection size + displayed value so the input
              remounts when the selection or its committed value changes. */}
          <label className="text-xs text-text-2" htmlFor="ce-spinner-time">Time:&nbsp;</label>
          <div style={{ width: 84 }} data-testid="ce-spinner-time-wrapper">
            <Spinner
              key={`time:${focusedChannel.trackName}:${selectedKeyTimes.size}:${spinnerTimeValue}`}
              aria-label="Selected key time"
              value={spinnerTimeValue}
              onChange={handleTimeSpinner}
              min={0}
              max={100}
              // an-audit-finding: legacy used a 0.1 time step; wheel/arrows nudge in
              // tenths of a percent. Display picks up the app-wide 2dp
              // default () — decoupled from step — so the Time field
              // reads consistently with the Value spinner beside it.
              step={0.1}
              unit="%"
              disabled={timeSpinnerDisabled}
              density="tight"
            />
          </div>
          <label className="text-xs text-text-2 ml-1" htmlFor="ce-spinner-value">Value:&nbsp;</label>
          <div style={{ width: 84 }} data-testid="ce-spinner-value-wrapper">
            {(() => {
              const sb = spinnerBoundsForTrack(focusedChannel.trackName);
              return (
                <Spinner
                  key={`value:${focusedChannel.trackName}:${selectedKeyTimes.size}:${spinnerValueValue}`}
                  aria-label="Selected key value"
                  value={spinnerValueValue}
                  onChange={handleValueSpinner}
                  min={sb.min}
                  max={sb.max}
                  step={sb.step}
                  // Integer-grained tracks (Index, step 1) display as whole
                  // numbers; fractional tracks (colour 0.01, scale/rotation
                  // 0.1) inherit the 2dp default.
                  decimals={sb.step >= 1 ? 0 : undefined}
                  disabled={spinnersDisabled}
                  density="tight"
                />
              );
            })()}
          </div>
        </div>

        <div className="ce-body">
          <div
            className="curve-list"
            role="group"
            aria-label="Curve channels"
            data-testid="curve-channel-list"
          >
            {CHANNELS.flatMap((c) => {
              const isOn = visible[c.id] ?? c.defaultOn;
              const isFocus = c.id === focusChannel;
              const row = (
                <div
                  key={c.id}
                  className={
                    isFocus
                      ? "curve-row !bg-accent-soft"
                      : "curve-row"
                  }
                  role="button"
                  tabIndex={0}
                  data-testid={`curve-channel-row-${c.id}`}
                  data-on={isOn ? "true" : "false"}
                  data-focus={isFocus ? "true" : "false"}
                  onClick={() => handleRowClick(c.id)}
                  onKeyDown={(e) => {
                    // Enter/Space activate the row (focus) for
                    // keyboard users.
                    if (e.key === "Enter" || e.key === " ") {
                      e.preventDefault();
                      handleRowClick(c.id);
                    }
                  }}
                  aria-pressed={isFocus}
                >
                  <input
                    type="checkbox"
                    checked={isOn}
                    onChange={(e) => {
                      if (EXCLUSIVE_CHANNELS.has(c.id) && e.target.checked) {
                        enableExclusively(c.id);
                      } else {
                        setVisible((v) => ({ ...v, [c.id]: e.target.checked }));
                      }
                    }}
                    onClick={(e) => {
                      // Don't propagate to the row click — the
                      // checkbox toggles visibility only; the row body
                      // handles focus.
                      e.stopPropagation();
                    }}
                    aria-label={`Toggle ${c.label} curve`}
                    data-testid={`curve-channel-checkbox-${c.id}`}
                  />
                  <span
                    className="swatch"
                    style={{ background: c.color }}
                    aria-hidden="true"
                  />
                  <span className="min-w-0 flex-1 truncate">{c.label}</span>
                </div>
              );
              // Render a horizontal divider before the first
              // transform-y channel (Scale) so the colour group
              // visually separates from the transform group.
              if (c.id === "scale") {
                return [
                  <div
                    key="curve-channel-group-divider"
                    className="section-divider"
                    data-testid="curve-channel-group-divider"
                    role="separator"
                    aria-hidden="true"
                  />,
                  row,
                ];
              }
              return [row];
            })}
          </div>
          <div className="curve-canvas-wrap">
            {selectedId === null ? (
              <div
                data-testid="curve-editor-placeholder"
                className="flex h-full items-center justify-center text-xs text-text-3"
              >
                Select an emitter to edit its tracks
              </div>
            ) : (
              <CanvasWithAxisLabels
                yMin={unifiedRange.min}
                yMax={unifiedRange.max}
                onGutterPointerDown={(e) => {
                  // Gutter press starts a marquee only in Select mode (Insert
                  // needs an in-plot coordinate, so a gutter press is a no-op).
                  // Locked focus: withheld entirely (no mutation on a read-only mirror).
                  if (mode === "select" && !focusLocked) {
                    curveRef.current?.startMarquee(e.clientX, e.clientY, e.shiftKey, e.pointerId);
                  }
                }}
              >
                <CurveEditor
                  marqueeRef={curveRef}
                  tracks={tracks}
                  channels={CHANNELS}
                  visibleChannels={visible}
                  emitterId={tracksOwnerId}
                  focusChannel={focusChannel}
                  valueRange={unifiedRange}
                  selectedKeyTimes={selectedKeyTimes}
                  onCanvasClick={handleCanvasClick}
                  onCanvasContextMenu={() => {
                    // an-audit-finding: mirror legacy WM_RBUTTONDOWN. In Insert mode a
                    // right-click drops back to Select mode without
                    // deselecting; in Select mode it clears the selection.
                    if (mode === "insert") {
                      setMode("select");
                      return;
                    }
                    setOptimisticSelected(null);
                    setSelectedKeyTimes((prev) => (prev.size === 0 ? prev : new Set()));
                  }}
                  {...interactiveHandlers}
                />
              </CanvasWithAxisLabels>
            )}
          </div>
        </div>
      </div>
      {/* Per-key right-click menu (floating, fixed-position). */}
      {keyContextMenu !== null && (
        <KeyContextMenu
          time={keyContextMenu.time}
          isBorder={keyContextMenu.isBorder}
          x={keyContextMenu.x}
          y={keyContextMenu.y}
          onClose={() => setKeyContextMenu(null)}
          onDelete={() => {
            const t = keyContextMenu.time;
            setKeyContextMenu(null);
            if (selectedId === null || focusLocked) return;
            if (borderKeyTimes.has(t)) return;
            void bridge.request({
              kind: "emitters/delete-track-keys",
              params: {
                id: selectedId,
                track: focusedChannel.trackName,
                times: [t],
              },
            }).then(() => {
              setOptimisticSelected((prev) =>
                prev !== null && prev.time === t ? null : prev,
              );
              setSelectedKeyTimes((prev) => {
                if (!prev.has(t)) return prev;
                const next = new Set(prev);
                next.delete(t);
                return next;
              });
            }).catch(() => { /* silent */ });
          }}
        />
      )}
    </div>
  );
}

/** Floating per-key right-click menu. Tiny enough to inline; matches
 *  the pattern from the deleted TrackEditor. */
function KeyContextMenu({
  time,
  isBorder,
  x,
  y,
  onClose,
  onDelete,
}: {
  time: number;
  isBorder: boolean;
  x: number;
  y: number;
  onClose: () => void;
  onDelete: () => void;
}) {
  void time; // surfaced for future entries (Snap to grid uses it).
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    const onMouseDown = (e: MouseEvent) => {
      const el = e.target as HTMLElement | null;
      if (el && el.closest("[data-key-menu]")) return;
      onClose();
    };
    window.addEventListener("keydown", onKey);
    document.addEventListener("mousedown", onMouseDown);
    return () => {
      window.removeEventListener("keydown", onKey);
      document.removeEventListener("mousedown", onMouseDown);
    };
  }, [onClose]);

  return (
    <div
      data-key-menu="true"
      data-testid="ce-key-context-menu"
      role="menu"
      aria-label="Curve key actions"
      className="fixed z-50 min-w-[140px] rounded-md border border-border-2 bg-bg-2 p-1 text-xs text-text shadow-xl"
      style={{ left: x, top: y }}
    >
      {/* T6 + T4: the tooltip only exists while the item is DISABLED
          (border key) — disabled elements fire no pointer events, so the
          Tip rides a block span wrapper (block, not inline-block, to keep
          the menu item full-width). */}
      <Tip
        content={isBorder ? "Border keys cannot be deleted" : undefined}
      >
        <span className="block w-full">
          <button
            type="button"
            role="menuitem"
            data-testid="ce-key-context-menu-delete"
            onClick={onDelete}
            disabled={isBorder}
            className="block w-full rounded px-2 py-1 text-left hover:bg-panel-2 disabled:cursor-not-allowed disabled:text-text-3 disabled:hover:bg-transparent outline-none"
          >
            Delete
          </button>
        </span>
      </Tip>
    </div>
  );
}
