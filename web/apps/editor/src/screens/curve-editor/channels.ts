import type { TrackName } from "@particle-editor/bridge-schema";
import type { ChannelDef } from "./CurveEditor";

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
// it's not meaningful to overlay it with the colour curves.
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
// and selecting any non-exclusive curve exits solo. Index was added to
// the set that previously held only Scale; Rotation joined later (its
// degrees/sec scale, like Index and Scale, doesn't share the 0..1 band).
export const EXCLUSIVE_CHANNELS: ReadonlySet<string> = new Set(["scale", "index", "rotation"]);

/** localStorage key for the curve-editor snap-to-grid toggle. */
export const SNAP_PREF_KEY = "curveEditor.snapToGrid";

/** Lock-to combo options per track. Mirrors the legacy Win32 editor's
 *  track-editor table. The leading "None" option is
 *  always present; "only None" disables the combo. */
export const LOCK_TO_OPTIONS: Record<TrackName, readonly string[]> = {
  red:           ["None"],
  green:         ["None", "Red"],
  blue:          ["None", "Red", "Green"],
  alpha:         ["None", "Red", "Green", "Blue"],
  scale:         ["None"],
  index:         ["None"],
  rotationSpeed: ["None"],
};

export function defaultVisibility(): Record<string, boolean> {
  const result: Record<string, boolean> = {};
  for (const c of CHANNELS) result[c.id] = c.defaultOn;
  return result;
}

/** Spinner clamp bounds per track. These are the engine-allowed
 *  bounds the user can enter — different from the *display* range
 *  computed by `valueRangeForTrack` (which is derived from current
 *  keys and adapts as keys change). Spinner bounds are constant per
 *  channel so the user can push key values past the current display
 *  range to grow it. */
export function spinnerBoundsForTrack(name: TrackName): {
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
      // ParticleSystemSerialization.cpp), so
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

