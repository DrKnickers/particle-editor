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
  /** A primary pointerdown landing in a label gutter (outside the plot
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
        // Gutter-marquee: a primary press landing OUTSIDE the plot SVG
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
            className="absolute pr-2 text-3xs leading-none text-text-2"
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
            className="absolute pt-1.5 text-3xs leading-none text-text-2"
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

