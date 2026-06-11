// useCurveMorph — drives the sample-and-tween morph for
// MultiChannelCurves. React's job: mount/unmount one overlay <g> per
// morphing channel and hide that channel's static layer. The hook's
// job: per-frame imperative drawing INTO the overlay group (polyline +
// fill) via one shared rAF loop — the FLIP/dock-anim direct-DOM-write
// idiom, no per-frame setState.
//
// Markers (glide/pop/ghost) are Task 3 — not in this file yet.
// This task covers line + fill only.
//
// Fill: only the FOCUS channel has a fill (matching the static layer
// which only draws the gradient under the focus curve). The overlay
// replicates the static gradient exactly: a self-contained <defs>
// <linearGradient id="morph-fill-<channelId>" x1=0 y1=0 x2=0 y2=1>
// with stop-opacity 0.25→0, using objectBoundingBox units so the
// gradient maps to the fill path's own bbox each frame — identical to
// the static curve-fill-<channelId> gradient in CurveEditor.tsx.
// Non-focus channels render a stroked line only, no fill path.

import { useEffect, useLayoutEffect, useRef, useState } from "react";
import type { TrackDto } from "@particle-editor/bridge-schema";
import {
  buildMorphGrid,
  classifyTrackChange,
  easeOutCubic,
  matchKeys,
  MORPH_MS,
  resampleOntoGrid,
  sampleTrackPx,
  KEY_MATCH_EPS,
} from "./curve-morph";

export type MorphChannelInput = {
  channelId: string;
  color: string;
  track: TrackDto;
  vMin: number;       // the channel's own projection (projY)
  vMax: number;
  dashed: boolean;    // READONLY_DASH carrier (locked focus channel)
  strokeWidth: number;
  opacity: number;    // dimmed background layers morph dimmed
  isFocus: boolean;   // markers only on the focus channel (Task 3)
};

export type SuppressedMove = {
  channelId: string;
  moves: Array<{ oldTime: number; newTime: number; newValue: number }>;
} | null;

/** A single key-marker descriptor. x0/y0 are pixel coords from the OLD
 *  projection; x1/y1 from the NEW projection. For "in" x0/y0 are unused;
 *  for "out" x1/y1 are unused. */
type Marker = {
  mode: "move" | "in" | "out";
  x0: number;
  y0: number;
  x1: number;
  y1: number;
};

type Job = {
  input: MorphChannelInput;   // latest target styling
  gridX: Float64Array;        // data-space xs
  from: Float64Array;         // pixel ys
  to: Float64Array;           // pixel ys
  start: number;              // performance.now at (re)start
  lastE: number;              // last eased progress, for interruption folding
  el: SVGGElement | null;     // overlay group (attached by React)
  // imperative children, created on first tick:
  line?: SVGPolylineElement;
  fill?: SVGPathElement;
  // Marker choreography (Task 3): only populated when input.isFocus.
  markers: Marker[];
  markerCircles: SVGCircleElement[];
};

// ─── Private helpers ───────────────────────────────────────────────────────

const SVG_NS = "http://www.w3.org/2000/svg";

/** Project data-space x-grid onto pixel-space x values for the canvas. */
function toPixelXs(
  gridX: Float64Array,
  timeMin: number,
  timeMax: number,
  width: number,
): Float64Array {
  const range = timeMax - timeMin;
  if (range <= 0) return new Float64Array(gridX.length);
  const out = new Float64Array(gridX.length);
  for (let i = 0; i < gridX.length; i++) {
    out[i] = ((gridX[i]! - timeMin) / range) * width;
  }
  return out;
}

/** Compute the currently-displayed pixel-y samples by lerping from→to
 *  at the job's last eased progress. Used for interruption folding so
 *  a retarget starts from the shape currently on screen. */
function currentSamples(job: Job): Float64Array {
  const e = job.lastE;
  const { from, to } = job;
  const out = new Float64Array(from.length);
  for (let i = 0; i < from.length; i++) {
    out[i] = from[i]! + (to[i]! - from[i]!) * e;
  }
  return out;
}

/** Check whether the diff between prevTrack and nextTrack is fully
 *  explained by the recorded suppressed moves (within KEY_MATCH_EPS).
 *  Returns true → suppress (snap, no morph).
 *
 *  Reorder-tolerant: a GROUP drag can move a selected key past an
 *  unselected neighbour; the optimistic overlay re-sorts by time
 *  (CurveEditorPanel.tsx ~:902), so index-paired comparison silently
 *  fails. Instead we build the EXPECTED post-move multiset from prev's
 *  keys + the recorded moves and verify a bijection (order-independent)
 *  against next's keys. */
export function movesMatch(
  prevTrack: TrackDto,
  nextTrack: TrackDto,
  moves: Array<{ oldTime: number; newTime: number; newValue: number }>,
): boolean {
  // Same key count is required (a structural insert/delete is not a suppressed move).
  if (prevTrack.keys.length !== nextTrack.keys.length) return false;

  // Defensive: every recorded move's oldTime must match some prev key.
  // A stale / garbage suppression whose anchors have no match in prev
  // should never suppress a real morph.
  for (const m of moves) {
    const found = prevTrack.keys.some(
      (pk) => Math.abs(pk.time - m.oldTime) <= KEY_MATCH_EPS,
    );
    if (!found) return false;
  }

  // Build expected = prev.keys mapped through the recorded moves.
  const expected = prevTrack.keys.map((pk) => {
    const m = moves.find((mv) => Math.abs(pk.time - mv.oldTime) <= KEY_MATCH_EPS);
    return m
      ? { time: m.newTime, value: m.newValue }
      : { time: pk.time, value: pk.value };
  });

  // Require a bijection between expected and next.keys, matched within EPS
  // on BOTH time and value, order-independent (greedy on time-sorted lists).
  // n is tiny (typical: 2–8) so O(n²) is fine.
  const used = new Array<boolean>(expected.length).fill(false);
  for (const nk of nextTrack.keys) {
    let matched = false;
    for (let i = 0; i < expected.length; i++) {
      if (used[i]) continue;
      const ek = expected[i]!;
      if (
        Math.abs(nk.time - ek.time) <= KEY_MATCH_EPS &&
        Math.abs(nk.value - ek.value) <= KEY_MATCH_EPS
      ) {
        used[i] = true;
        matched = true;
        break;
      }
    }
    if (!matched) return false;
  }
  return true;
}

/** Build the `points` attribute string for a polyline from parallel x/y arrays. */
function buildPointsString(xs: Float64Array, ys: Float64Array): string {
  const parts: string[] = [];
  for (let i = 0; i < xs.length; i++) {
    parts.push(`${xs[i]!.toFixed(2)},${ys[i]!.toFixed(2)}`);
  }
  return parts.join(" ");
}

/** Build the fill-path `d` string: traces from left to right along the
 *  lerped polyline, then drops to `height` and returns left. Matches
 *  the `buildFillPath` shape logic in CurveEditor.tsx. */
function buildFillD(xs: Float64Array, ys: Float64Array, height: number): string {
  if (xs.length < 2) return "";
  const parts: string[] = [`M ${xs[0]!.toFixed(2)} ${ys[0]!.toFixed(2)}`];
  for (let i = 1; i < xs.length; i++) {
    parts.push(`L ${xs[i]!.toFixed(2)} ${ys[i]!.toFixed(2)}`);
  }
  const lastX = xs[xs.length - 1]!;
  const firstX = xs[0]!;
  parts.push(`L ${lastX.toFixed(2)} ${height.toFixed(2)}`);
  parts.push(`L ${firstX.toFixed(2)} ${height.toFixed(2)}`);
  parts.push("Z");
  return parts.join(" ");
}


/** Project a key's (time, value) to pixel coordinates using the same
 *  formula as sampleTrackPx / the curve sampler:
 *   x = (time - timeMin) / (timeMax - timeMin) * width
 *   y = height - clamp01((value - vMin) / (vMax - vMin)) * height  */
function projectKey(
  time: number,
  value: number,
  timeMin: number,
  timeMax: number,
  width: number,
  proj: { vMin: number; vMax: number; height: number },
): { x: number; y: number } {
  const tRange = timeMax - timeMin;
  const x = tRange > 0 ? ((time - timeMin) / tRange) * width : 0;
  const vRange = proj.vMax - proj.vMin;
  const tnorm = vRange > 0 ? (value - proj.vMin) / vRange : 0;
  const clamped = Math.max(0, Math.min(1, tnorm));
  const y = proj.height - clamped * proj.height;
  return { x, y };
}

/** Compute the marker list for a focus channel.
 *  prevTrack/prevProj describe the old shape; nextInput.track/vMin/vMax
 *  describe the new target. Returns [] for non-focus channels. */
function computeMarkers(
  prevTrack: TrackDto,
  prevProj: { vMin: number; vMax: number },
  nextInput: MorphChannelInput,
  timeMin: number,
  timeMax: number,
  width: number,
  height: number,
): Marker[] {
  if (!nextInput.isFocus) return [];

  const oldProj = { vMin: prevProj.vMin, vMax: prevProj.vMax, height };
  const newProj = { vMin: nextInput.vMin, vMax: nextInput.vMax, height };

  const result = matchKeys(prevTrack, nextInput.track);
  const markers: Marker[] = [];

  for (const { from, to } of result.moved) {
    const p0 = projectKey(from.time, from.value, timeMin, timeMax, width, oldProj);
    const p1 = projectKey(to.time, to.value, timeMin, timeMax, width, newProj);
    markers.push({ mode: "move", x0: p0.x, y0: p0.y, x1: p1.x, y1: p1.y });
  }
  for (const k of result.added) {
    const p1 = projectKey(k.time, k.value, timeMin, timeMax, width, newProj);
    markers.push({ mode: "in", x0: 0, y0: 0, x1: p1.x, y1: p1.y });
  }
  for (const k of result.removed) {
    const p0 = projectKey(k.time, k.value, timeMin, timeMax, width, oldProj);
    markers.push({ mode: "out", x0: p0.x, y0: p0.y, x1: 0, y1: 0 });
  }

  return markers;
}

/** Draw a single morph frame into job.el, creating the imperative SVG
 *  children on the first call. Only the focus channel gets a fill path;
 *  non-focus channels render a stroked line only (matching the static
 *  layer which draws the gradient fill only under the focus curve). The
 *  focus fill uses a self-contained linearGradient (morph-fill-<id>)
 *  that is identical to the static curve-fill-<id> gradient: stopOpacity
 *  0.25→0, x1/y1/x2/y2 = 0/0/0/1, objectBoundingBox units. */
function drawJob(
  job: Job,
  e: number,
  dims: { width: number; height: number },
  timeMin: number,
  timeMax: number,
): void {
  const { el, from, to, input } = job;
  if (el === null) return;

  // Compute lerped pixel-space y values.
  const ys = new Float64Array(from.length);
  for (let i = 0; i < from.length; i++) {
    ys[i] = from[i]! + (to[i]! - from[i]!) * e;
  }
  const xs = toPixelXs(job.gridX, timeMin, timeMax, dims.width);

  // Create imperative children on first call.
  // Only the focus channel gets a fill — matching the static layer.
  if (!job.fill && input.isFocus) {
    const gradId = `morph-fill-${input.channelId}`;
    // Self-contained <defs> with gradient identical to the static
    // curve-fill-<channelId> gradient in CurveEditor.tsx.
    const defs = document.createElementNS(SVG_NS, "defs") as SVGDefsElement;
    const grad = document.createElementNS(SVG_NS, "linearGradient") as SVGLinearGradientElement;
    grad.setAttribute("id", gradId);
    grad.setAttribute("x1", "0");
    grad.setAttribute("y1", "0");
    grad.setAttribute("x2", "0");
    grad.setAttribute("y2", "1");
    const stop0 = document.createElementNS(SVG_NS, "stop") as SVGStopElement;
    stop0.setAttribute("offset", "0%");
    stop0.setAttribute("stop-color", input.color);
    stop0.setAttribute("stop-opacity", "0.25");
    const stop1 = document.createElementNS(SVG_NS, "stop") as SVGStopElement;
    stop1.setAttribute("offset", "100%");
    stop1.setAttribute("stop-color", input.color);
    stop1.setAttribute("stop-opacity", "0");
    grad.appendChild(stop0);
    grad.appendChild(stop1);
    defs.appendChild(grad);
    el.appendChild(defs);

    const fill = document.createElementNS(SVG_NS, "path") as SVGPathElement;
    fill.setAttribute("stroke", "none");
    fill.setAttribute("pointer-events", "none");
    fill.setAttribute("fill", `url(#${gradId})`);
    el.appendChild(fill);
    job.fill = fill;
  }

  if (!job.line) {
    const line = document.createElementNS(SVG_NS, "polyline") as SVGPolylineElement;
    line.setAttribute("fill", "none");
    line.setAttribute("stroke", input.color);
    line.setAttribute("stroke-width", String(input.strokeWidth));
    if (input.dashed) {
      line.setAttribute("stroke-dasharray", "7 5");
    }
    line.setAttribute("opacity", String(input.opacity));
    line.setAttribute("pointer-events", "none");
    el.appendChild(line);
    job.line = line;
  }

  // Update geometry every frame.
  if (job.fill) job.fill.setAttribute("d", buildFillD(xs, ys, dims.height));
  job.line.setAttribute("points", buildPointsString(xs, ys));

  // ── Marker circles (focus channel only) ─────────────────────────────────
  // On the first tick (or after a retarget that changed the marker count),
  // create/recreate the circle elements. On subsequent ticks, just update
  // their attributes.
  const { markers, input: { color } } = job;
  if (job.markerCircles.length !== markers.length) {
    // Remove any stale circles from the DOM and recreate.
    // Guard with try/catch: if the overlay <g> was re-created between ticks
    // the node may no longer be a child → NotFoundError. Matches the
    // defensive posture of the classification-effect's pre-clear path.
    for (const c of job.markerCircles) {
      try { el.removeChild(c); } catch { /* already detached */ }
    }
    job.markerCircles = [];
    for (let i = 0; i < markers.length; i++) {
      const circle = document.createElementNS(SVG_NS, "circle") as SVGCircleElement;
      circle.setAttribute("fill", color);
      circle.setAttribute("stroke", "none");
      el.appendChild(circle);
      job.markerCircles.push(circle);
    }
  }
  for (let i = 0; i < markers.length; i++) {
    const m = markers[i]!;
    const circle = job.markerCircles[i]!;
    if (m.mode === "move") {
      circle.setAttribute("cx", String((m.x0 + (m.x1 - m.x0) * e).toFixed(2)));
      circle.setAttribute("cy", String((m.y0 + (m.y1 - m.y0) * e).toFixed(2)));
      circle.setAttribute("r", "5");
      circle.setAttribute("fill-opacity", "1");
    } else if (m.mode === "in") {
      circle.setAttribute("cx", String(m.x1.toFixed(2)));
      circle.setAttribute("cy", String(m.y1.toFixed(2)));
      circle.setAttribute("r", String((5 * e).toFixed(3)));
      circle.setAttribute("fill-opacity", String(e.toFixed(3)));
    } else {
      // "out"
      circle.setAttribute("cx", String(m.x0.toFixed(2)));
      circle.setAttribute("cy", String(m.y0.toFixed(2)));
      circle.setAttribute("r", String((5 * (1 - e)).toFixed(3)));
      circle.setAttribute("fill-opacity", String((1 - e).toFixed(3)));
    }
  }
}

// ─── Hook ────────────────────────────────────────────────────────────────────

export function useCurveMorph(args: {
  channels: MorphChannelInput[];
  width: number;
  height: number;
  timeMin: number;
  timeMax: number;
  isDragging: () => boolean;
  suppressRef: React.MutableRefObject<SuppressedMove>;
}): {
  activeIds: string[];
  attach: (channelId: string) => (el: SVGGElement | null) => void;
  isActive: (id: string) => boolean;
} {
  const { channels, width, height, timeMin, timeMax, isDragging, suppressRef } = args;
  const jobs = useRef(new Map<string, Job>());
  const prev = useRef(new Map<string, { track: TrackDto; vMin: number; vMax: number }>());
  const raf = useRef(0);
  const fallback = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [activeIds, setActiveIds] = useState<string[]>([]);
  // Capture dims in a ref so the rAF loop closure always reads the latest values.
  const dimsRef = useRef({ width, height });
  dimsRef.current = { width, height };

  const motionOk = (): boolean =>
    typeof window.matchMedia === "function" &&
    !window.matchMedia("(prefers-reduced-motion: reduce)").matches;

  // ─── Classify + start/retarget jobs on every render ──────────────────
  // This effect runs every render (no deps array) so it picks up every
  // tracks-prop change. It is cheap when nothing changed: classify() is
  // O(n_keys) with an early return on "none".
  useEffect(() => {
    const next = new Map<string, { track: TrackDto; vMin: number; vMax: number }>();
    let anyStarted = false;

    for (const c of channels) {
      next.set(c.channelId, { track: c.track, vMin: c.vMin, vMax: c.vMax });
      const p = prev.current.get(c.channelId);
      if (!p || isDragging() || !motionOk()) continue;

      const change = classifyTrackChange(p.track, c.track);
      if (change === "none") continue;

      // Drag-commit suppression (Task 4 wires the recording; here we only
      // consume. If suppressRef.current is null, this is a no-op).
      const sup = suppressRef.current;
      if (
        sup &&
        sup.channelId === c.channelId &&
        movesMatch(p.track, c.track, sup.moves)
      ) {
        // Snap: the change is explained by a recorded drag — don't morph.
        suppressRef.current = null;
        continue;
      }
      // Stale non-matching suppression: clear it so it can't block a future
      // legitimate morph on this channel.
      if (sup && sup.channelId === c.channelId) {
        suppressRef.current = null;
      }

      const gridX = buildMorphGrid(p.track, c.track, timeMin, timeMax);
      const to = sampleTrackPx(c.track, gridX, { vMin: c.vMin, vMax: c.vMax, height });

      const existing = jobs.current.get(c.channelId);
      let from: Float64Array;
      if (existing) {
        // Interruption folding: resample the currently-displayed shape
        // (a known polyline in old pixel-x space) onto the new grid.
        const oldPixelXs = toPixelXs(existing.gridX, timeMin, timeMax, width);
        const displayedYs = currentSamples(existing);
        const newPixelXs = toPixelXs(gridX, timeMin, timeMax, width);
        from = resampleOntoGrid(oldPixelXs, displayedYs, newPixelXs);
      } else {
        from = sampleTrackPx(p.track, gridX, { vMin: p.vMin, vMax: p.vMax, height });
      }

      // Compute markers for focus channel; retarget rebuilds the list.
      const markers = computeMarkers(
        p.track,
        { vMin: p.vMin, vMax: p.vMax },
        c,
        timeMin,
        timeMax,
        width,
        height,
      );
      // On retarget, clear existing marker circles so drawJob recreates them
      // (count may have changed).
      const existingCircles = existing?.markerCircles ?? [];
      if (existingCircles.length > 0 && existing?.el) {
        for (const circle of existingCircles) {
          try { existing.el.removeChild(circle); } catch { /* already removed */ }
        }
      }

      jobs.current.set(c.channelId, {
        input: c,
        gridX,
        from,
        to,
        start: performance.now(),
        lastE: 0,
        el: existing?.el ?? null,
        line: existing?.line,
        fill: existing?.fill,
        markers,
        markerCircles: [],
      });
      anyStarted = true;
    }

    // Prune jobs for channels that have disappeared (visibility toggle,
    // emitter switch) — spec §2.3.
    for (const id of Array.from(jobs.current.keys())) {
      if (!channels.some((c) => c.channelId === id)) {
        jobs.current.delete(id);
      }
    }

    prev.current = next;

    if (anyStarted) {
      setActiveIds(Array.from(jobs.current.keys()));
      // Refresh the fallback deadline every time a job starts or retargets.
      // This prevents a sustained stream of retargets (interruption folding)
      // from hitting a stale deadline that was set at the FIRST loop start.
      // The fallback is always MORPH_MS+250 from the MOST RECENT (re)start,
      // so an actively-morphing channel is never killed early; a truly stuck
      // loop (rAF stops firing, no new starts) still gets swept after the
      // grace period.
      if (fallback.current !== null) clearTimeout(fallback.current);
      fallback.current = setTimeout(() => {
        jobs.current.clear();
        finish();
      }, MORPH_MS + 250);
      ensureLoop();
    }
    // Width/height change (canvas resize) re-snapshots via dep change
    // but does NOT restart morphs — they were already cleared by the
    // channel-disappearance prune above if the emitter switched.
  });

  // ─── rAF loop ────────────────────────────────────────────────────────

  function ensureLoop(): void {
    if (raf.current) return; // already running
    const tick = (now: number) => {
      let any = false;
      const dims = dimsRef.current;
      for (const [id, j] of jobs.current) {
        const p = Math.min(1, (now - j.start) / MORPH_MS);
        const e = easeOutCubic(p);
        j.lastE = e;
        drawJob(j, e, dims, timeMin, timeMax);
        if (p < 1) {
          any = true;
        } else {
          jobs.current.delete(id);
        }
      }
      if (any) {
        raf.current = requestAnimationFrame(tick);
      } else {
        finish();
      }
    };
    raf.current = requestAnimationFrame(tick);
    // Note: the fallback deadline is NOT set here. It is set (and refreshed)
    // in the anyStarted block of the classification effect, so it always
    // reflects the most-recent (re)start time. ensureLoop() only starts the
    // rAF; the leak-guard is owned by the caller.
  }

  function finish(): void {
    cancelAnimationFrame(raf.current);
    raf.current = 0;
    if (fallback.current !== null) {
      clearTimeout(fallback.current);
      fallback.current = null;
    }
    // Update React state: activeIds = [] when all jobs are done,
    // or the remaining set if some jobs are still running (shouldn't
    // happen from finish(), but be defensive).
    setActiveIds(Array.from(jobs.current.keys()));
  }

  // Fix B — synchronous first frame (no empty-overlay flash).
  // Runs post-commit, pre-paint, whenever activeIds changes (i.e. when a
  // new overlay <g> has just been mounted by React). Draws e≈0 immediately
  // so the first painted frame shows the old shape — seamless handoff from
  // the hidden static layer. The rAF loop continues from the next frame;
  // both write the same attributes, no conflict.
  useLayoutEffect(() => {
    const now = performance.now();
    for (const j of jobs.current.values()) {
      if (!j.el) continue;
      const e = easeOutCubic(Math.min(1, (now - j.start) / MORPH_MS));
      drawJob(j, e, { width, height }, timeMin, timeMax);
    }
  }, [activeIds]); // eslint-disable-line react-hooks/exhaustive-deps — drawJob/dims read fresh

  // Cleanup on unmount.
  useEffect(() => {
    return () => {
      cancelAnimationFrame(raf.current);
      if (fallback.current !== null) clearTimeout(fallback.current);
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  return {
    activeIds,
    isActive: (id) => jobs.current.has(id) || activeIds.includes(id),
    attach: (id) => (el) => {
      const j = jobs.current.get(id);
      if (j) j.el = el;
    },
  };
}
