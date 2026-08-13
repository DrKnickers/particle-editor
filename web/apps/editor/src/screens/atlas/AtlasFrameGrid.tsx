import { memo, useCallback, useEffect, useRef, useState } from "react";
import type { FocusEvent, KeyboardEvent, MouseEvent, RefObject } from "react";
import { fitGridLayout } from "@/lib/atlas-grid";
import { drawGrid, GRID_GAP } from "./atlas-canvas";
import { useDecodedImage } from "./useDecodedImage";

type OkPreviewState = {
  kind: "ok";
  dataUri: string;
  srcW: number;
  srcH: number;
};
type AtlasGridLayout = ReturnType<typeof fitGridLayout>;

function useRenderCount(): number {
  const count = useRef(0);
  count.current += 1;
  return count.current;
}

export const AtlasFrameGrid = memo(function AtlasFrameGrid({
  gridRef,
  okPreview,
  totalCells,
  side,
  highlight,
  rovingTarget,
  deadCells,
  layout,
  onGridKeyDown,
  onFocusCapture,
  onBlurCapture,
  onHover,
  onClick,
}: {
  gridRef: RefObject<HTMLCanvasElement | null>;
  okPreview: OkPreviewState | null;
  totalCells: number;
  side: number;
  highlight: number | null;
  rovingTarget: number;
  deadCells: ReadonlySet<number>;
  layout: AtlasGridLayout;
  onGridKeyDown: (e: KeyboardEvent<HTMLElement>) => void;
  onFocusCapture: () => void;
  onBlurCapture: (e: FocusEvent<HTMLElement>) => void;
  onHover: (k: number | null) => void;
  onClick: (k: number) => void;
}) {
  const renderCount = useRenderCount();
  const cols = Math.max(1, layout.cols);
  const cell = layout.cell;
  const rows = Math.ceil(totalCells / cols);
  const contentH = rows > 0 ? rows * cell + (rows - 1) * GRID_GAP : 0;
  const innerW = cols * cell + (cols - 1) * GRID_GAP;

  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const overlayRef = useRef<HTMLDivElement | null>(null);
  const dataUri = okPreview?.dataUri ?? null;
  const { imageRef, imageReady } = useDecodedImage(dataUri);
  const [focused, setFocused] = useState(false);

  // Store the canvas on BOTH the local ref (draw/hit-test) and the parent's
  // gridRef (focus + scroll-into-view live in the parent).
  const setCanvas = useCallback((el: HTMLCanvasElement | null) => {
    canvasRef.current = el;
    gridRef.current = el;
  }, [gridRef]);

  // [design pass] The canvas samples tokens via getComputedStyle at draw time,
  // so a theme flip would leave it painted in the OLD palette. Watch
  // <html data-theme> and bump a revision: once immediately (custom properties
  // flip in the same frame), and once after the ~220ms theme-transition window
  // (theme.ts) — getComputedStyle(canvas).backgroundColor transitions during
  // the flip, so the settle redraw picks up the final panel gray.
  const [themeRev, setThemeRev] = useState(0);
  useEffect(() => {
    // Settle-timer is tracked so rapid flips coalesce and unmount can't
    // leak a pending setState (pre-PR review).
    let settleTimer: number | undefined;
    const mo = new MutationObserver(() => {
      setThemeRev((n) => n + 1);
      window.clearTimeout(settleTimer);
      settleTimer = window.setTimeout(() => setThemeRev((n) => n + 1), 260);
    });
    mo.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
    return () => {
      mo.disconnect();
      window.clearTimeout(settleTimer);
    };
  }, []);

  // Redraw whenever the image, layout, selection, roving target, dead set,
  // focus, or theme changes. Scrolling does NOT change any of these → no
  // redraw on scroll.
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    drawGrid(canvas, imageReady ? imageRef.current : null, {
      cols, cell, side, totalCells, highlight, rovingTarget, deadCells, focusVisible: focused,
    });
  }, [imageReady, okPreview, cols, cell, side, totalCells, highlight, rovingTarget, deadCells, focused, themeRev]);

  // Hit-test: map a pointer position to a frame index, or null when the point is
  // outside the grid or lands in an inter-cell gap.
  const frameFromEvent = (e: MouseEvent): number | null => {
    const canvas = canvasRef.current;
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    const x = e.clientX - rect.left;
    const y = e.clientY - rect.top;
    if (x < 0 || y < 0) return null;
    const step = cell + GRID_GAP;
    const col = Math.floor(x / step);
    const row = Math.floor(y / step);
    if (col < 0 || col >= cols || row < 0) return null;
    if (x - col * step >= cell || y - row * step >= cell) return null; // in the gap
    const k = row * cols + col;
    return k >= 0 && k < totalCells ? k : null;
  };

  const handleClick = (e: MouseEvent) => {
    const k = frameFromEvent(e);
    if (k !== null) onClick(k);
  };
  const handleMove = (e: MouseEvent) => {
    const k = frameFromEvent(e);
    onHover(k); // drives the hero preview (via hoverRef; no re-render)
    const ov = overlayRef.current;
    if (!ov) return;
    // [design pass] Opacity (not display) so the highlight fades via the
    // .atlas-hover-fade transition instead of popping; the position jump
    // between cells stays instant (only opacity transitions).
    if (k === null || deadCells.has(k)) { ov.style.opacity = "0"; return; }
    const step = cell + GRID_GAP;
    ov.style.opacity = "1";
    ov.style.left = `${(k % cols) * step}px`;
    ov.style.top = `${Math.floor(k / cols) * step}px`;
    ov.style.width = `${cell}px`;
    ov.style.height = `${cell}px`;
  };
  const handleLeave = () => {
    onHover(null);
    if (overlayRef.current) overlayRef.current.style.opacity = "0";
  };

  const optId = `atlas-opt-${rovingTarget}`;
  const activeDead = deadCells.has(rovingTarget);

  return (
    // Fixed-size box (width = the grid's columns, height = ALL rows) centred in
    // the scroll panel via mx-auto. Holds the single <canvas> listbox, the
    // imperatively-positioned hover highlight, and the one active a11y option.
    <div
      data-testid="atlas-grid-box"
      className="relative mx-auto"
      style={{ width: `${innerW}px`, height: `${contentH}px` }}
    >
      <canvas
        ref={setCanvas}
        role="listbox"
        aria-label="Atlas frames"
        aria-multiselectable={false}
        aria-activedescendant={optId}
        aria-owns={optId}
        tabIndex={0}
        data-testid="atlas-canvas"
        data-atlas-cols={cols}
        data-atlas-cell={cell}
        data-atlas-gap={GRID_GAP}
        data-atlas-total={totalCells}
        data-render-count={renderCount}
        className="block h-full w-full rounded-[var(--radius-sm)] bg-bg-2 focus-ring"
        onKeyDown={onGridKeyDown}
        onFocus={() => { setFocused(true); onFocusCapture(); }}
        onBlur={(e) => { setFocused(false); onBlurCapture(e); }}
        onClick={handleClick}
        onMouseMove={handleMove}
        onMouseLeave={handleLeave}
      />
      {/* Hover highlight, positioned imperatively on mousemove so a hover never
          triggers a React re-render (the atlas grid stays put). */}
      <div
        ref={overlayRef}
        aria-hidden
        data-testid="atlas-hover-overlay"
        className="atlas-hover-fade pointer-events-none absolute rounded-[var(--radius-sm)] border-2 border-[var(--overlay-hover)]"
        style={{ opacity: 0 }}
      />
      {/* The ONE active option, referenced by aria-activedescendant (+ aria-owns)
          so a screen reader announces "Frame N of totalCells" — and "empty" for a
          dead frame — without one option element per frame. */}
      <div
        role="option"
        id={optId}
        data-testid="atlas-active-option"
        aria-selected={rovingTarget === highlight}
        aria-disabled={activeDead || undefined}
        aria-label={activeDead ? `Frame ${rovingTarget}, empty` : `Frame ${rovingTarget}`}
        aria-posinset={rovingTarget + 1}
        aria-setsize={totalCells}
        className="sr-only"
      />
    </div>
  );
});
