import { cellRect } from "@/lib/atlas-grid";

export const GRID_GAP = 4;

export interface DrawOpts {
  cols: number;
  cell: number;
  side: number;
  totalCells: number;
  highlight: number | null;
  rovingTarget: number;
  deadCells: ReadonlySet<number>;
  focusVisible: boolean;
}

// Resolve a CSS custom property (or plain computed style) off a live element,
// with a fallback for jsdom / a missing token. Canvas draws need concrete color
// strings, not `var(--x)`. Only ever called once a 2d context exists (real
// browser), so getComputedStyle is safe.
function cssVar(el: Element, name: string, fallback: string): string {
  const v = getComputedStyle(el).getPropertyValue(name).trim();
  return v || fallback;
}

/** [#572] Paint the WHOLE atlas onto the grid canvas in ONE pass: every frame
 *  via a single drawImage, a dark scrim washing dead cells toward the panel
 *  gray, an always-on frame-index badge, the amber selection ring on
 *  `highlight`, and the blue focus outline on `rovingTarget` (when focused).
 *  Internal resolution = CSS size × min(dpr, 2). No-ops gracefully with no 2d
 *  context (jsdom) or before the image has loaded (the canvas's own bg-bg-2
 *  shows through until then). */
export function drawGrid(canvas: HTMLCanvasElement, img: HTMLImageElement | null, o: DrawOpts) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return; // jsdom / no 2d context — no-op
  const { cols, cell, side, totalCells, highlight, rovingTarget, deadCells, focusVisible } = o;
  const step = cell + GRID_GAP;
  const rows = Math.ceil(totalCells / cols);
  const cssW = cols * cell + (cols - 1) * GRID_GAP;
  const cssH = rows > 0 ? rows * cell + (rows - 1) * GRID_GAP : 0;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const W = Math.max(1, Math.round(cssW * dpr));
  const H = Math.max(1, Math.round(cssH * dpr));
  if (canvas.width !== W) canvas.width = W;
  if (canvas.height !== H) canvas.height = H;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0); // draw in CSS px; the backing is dpr-scaled
  ctx.clearRect(0, 0, cssW, cssH); // transparent → the canvas's bg-bg-2 shows through

  const amber = cssVar(canvas, "--atlas-selected", "#ffb000");
  const focusColor = cssVar(canvas, "--accent", "#4ea3ff");
  const scrimBg = cssVar(canvas, "--overlay-scrim", "rgba(0,0,0,0.55)");
  const scrimFg = cssVar(canvas, "--overlay-scrim-fg", "#ececec");
  const panelGray = getComputedStyle(canvas).backgroundColor || "rgb(30,30,30)";

  const at = (k: number) => ({ x: (k % cols) * step, y: Math.floor(k / cols) * step });

  // 1) Frames.
  if (img) {
    ctx.imageSmoothingEnabled = true;
    const iw = img.naturalWidth, ih = img.naturalHeight;
    for (let k = 0; k < totalCells; k++) {
      const { x, y } = at(k);
      const r = cellRect(k, side, iw, ih);
      try { ctx.drawImage(img, r.left, r.top, r.width, r.height, x, y, cell, cell); } catch { /* skip */ }
    }
  }

  // 2) Per-cell overlays: dead scrim + the always-on index badge.
  ctx.textBaseline = "top";
  ctx.font = "9px system-ui, -apple-system, sans-serif";
  for (let k = 0; k < totalCells; k++) {
    const { x, y } = at(k);
    const selected = k === highlight;
    if (deadCells.has(k)) {
      ctx.save();
      ctx.globalAlpha = 0.6;
      ctx.fillStyle = panelGray;
      ctx.fillRect(x, y, cell, cell);
      ctx.restore();
    }
    const label = String(k);
    const tw = Math.ceil(ctx.measureText(label).width);
    const bw = tw + 6, bh = 12, bx = x + 2, by = y + cell - bh - 2;
    ctx.fillStyle = selected ? amber : scrimBg;
    ctx.fillRect(bx, by, bw, bh);
    ctx.fillStyle = selected ? "#000000" : scrimFg;
    ctx.fillText(label, bx + 3, by + 2);
  }

  // 3) Selection ring (amber) on the assigned frame.
  if (highlight !== null && highlight >= 0 && highlight < totalCells) {
    const { x, y } = at(highlight);
    ctx.lineWidth = 2;
    ctx.strokeStyle = amber;
    ctx.strokeRect(x + 1, y + 1, cell - 2, cell - 2);
  }
  // 4) Focus outline (blue) on the roving cell while the grid holds focus.
  if (focusVisible && rovingTarget >= 0 && rovingTarget < totalCells) {
    const { x, y } = at(rovingTarget);
    ctx.lineWidth = 2;
    ctx.strokeStyle = focusColor;
    ctx.strokeRect(x + 2, y + 2, cell - 4, cell - 4);
  }
}

/** Paint frame `frame`'s crop of the atlas image onto the hero canvas at its
 *  display size — drawing ONLY the one frame (vs the old CSS background that
 *  rasterized the whole atlas upscaled to `side × hero`). The canvas is left
 *  TRANSPARENT where the frame isn't drawn (clearRect, no backing fill), so the
 *  hero element's own bg-bg-2 shows through transparent frame pixels — matching
 *  the cells' uniform gray in both modes (no checkerboard).
 *  No-ops gracefully if there's no 2d context (jsdom) or no image yet. */
export function drawHero(
  canvas: HTMLCanvasElement,
  img: HTMLImageElement,
  frame: number,
  side: number,
) {
  const ctx = canvas.getContext("2d");
  if (!ctx) return; // jsdom has no real 2d context — no-op
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const cssW = canvas.clientWidth || canvas.width;
  const cssH = canvas.clientHeight || canvas.height;
  const w = Math.max(1, Math.round(cssW * dpr));
  const h = Math.max(1, Math.round(cssH * dpr));
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  ctx.clearRect(0, 0, w, h); // transparent → reveals the hero element's bg-bg-2
  // Source rect = frame's cell in the atlas (same maths as the CSS crop).
  const r = cellRect(frame, side, img.naturalWidth, img.naturalHeight);
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(img, r.left, r.top, r.width, r.height, 0, 0, w, h);
}
