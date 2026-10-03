import type { InterpolationType } from "@particle-editor/bridge-schema";

/** Linear-interpolate a value into 0..1 then map into 0..length.
 *  Clamps NaN/Infinity at the bounds to prevent broken SVG output. */
export function project(value: number, min: number, max: number, length: number): number {
  if (!Number.isFinite(value) || max <= min) return 0;
  const t = (value - min) / (max - min);
  return Math.max(0, Math.min(1, t)) * length;
}

/** Inverse of project — pixel position back to data coordinate. */
export function unproject(px: number, min: number, max: number, length: number): number {
  if (length <= 0) return min;
  const t = px / length;
  return min + Math.max(0, Math.min(1, t)) * (max - min);
}

/** Build the SVG path `d` string for a smooth (cubic-Bezier) curve
 *  through the given points. Mirrors the legacy curve editor's formula:
 *  control points sit at 1/4 and
 *  3/4 of the horizontal distance, sharing y with the segment's
 *  start / end key respectively. Returns "" when there are fewer
 *  than 2 points (no segment to render). */
export function buildSmoothPath(points: ReadonlyArray<{ x: number; y: number }>): string {
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
 *  vertical jump to p2.y. In the legacy curve editor
 *  the horizontal leg uses the "line pen" and the vertical leg the
 *  "step pen" — visual differentiation is deferred; the
 *  shape is identical either way. */
export function buildStepPolyline(points: ReadonlyArray<{ x: number; y: number }>): string {
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
export function buildFillPath(
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
export function eventToViewBox(
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

