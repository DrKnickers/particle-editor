// scene-rect.ts — the centre-quadrant viewport rect in the host's
// device-pixel "scene rect" space.
//
// The host crops the D3D9 engine visual to this rect (the DComp engine-visual
// transform clips to it). ViewportSlot dispatches it on every layout change via
// `layout/scene-rect`; PanelLayout reads it to seed the dock-slide animation's
// `from`/`to` (Item 3). Both callers MUST agree to the pixel — otherwise the
// host's interpolation target would drift from what ViewportSlot reports at
// rest, leaving a visible snap at the end of the slide. Keeping the math in one
// place is the guarantee.
//
// The viewport slot paints no border (SLOT_BORDER_PX was 0), so the device rect
// is just the client box × DPR, rounded to match the host's integer scene rect.

export type SceneRect = { x: number; y: number; w: number; h: number };

/** Device-pixel scene rect for an element's current client box. */
export function computeSceneRect(el: HTMLElement): SceneRect {
  const r = el.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  return {
    x: Math.round(r.left * dpr),
    y: Math.round(r.top * dpr),
    w: Math.round(Math.max(0, r.width) * dpr),
    h: Math.round(Math.max(0, r.height) * dpr),
  };
}

/**
 * The viewport scene rect at the END of a right-dock slide (Item 3).
 *
 * The right dock sits on the far side of the centre column, so the viewport's
 * left edge (x), top (y), and height (h) are fixed; only the width changes. On
 * OPEN the dock steals `dockWidthDev` device px from the centre (viewport
 * shrinks); on CLOSE that width returns (viewport grows). Width is clamped to ≥ 0.
 *
 * This is the COMMON-CASE prediction, deliberately NOT the full layout solve.
 * It does not re-clamp to the library's centre-min (30%) / dock-max (40%)
 * constraints, nor account for the percentage-vs-pixel divergence when the
 * window was resized while the dock was closed (the library restores a
 * remembered PERCENTAGE, not the pixels we shadow). In the common path — no
 * resize between close and open, not at a constraint boundary — the prediction
 * is exact (the whole dock delta moves to/from the centre; Phase-0 confirmed
 * 658↔918). At a constraint boundary or after a resize-while-closed the real
 * settled width diverges by a small amount; rather than re-implement the
 * library's solver on the web side (no clean handle on the group pixel width),
 * we let the caller's authoritative settle send pin the true rest rect — the
 * residual is a sub-pixel-to-few-px snap at the very end of the slide. Same
 * mechanism absorbs the narrow-window edge where the LEFT pane also moves.
 */
export function dockSlideTarget(
  from: SceneRect,
  dockWidthDev: number,
  opening: boolean,
): SceneRect {
  return {
    x: from.x,
    y: from.y,
    w: Math.max(0, from.w + (opening ? -dockWidthDev : dockWidthDev)),
    h: from.h,
  };
}
