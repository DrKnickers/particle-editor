// atlas-dead-cells.ts — detect "dead" atlas frames (empty slots).
//
// The shared master particle atlases (e.g. p_particle_master.tga) contain cells that
// carry RGB but ZERO alpha — leftover/empty slots. The Atlas Frames picker previews the
// RGB channel, so such a cell looks like real imagery in the grid; but particles blend by
// ALPHA, so assigning that frame renders nothing (the emitter stays alive, but no pixels
// draw). This flags those cells so the picker can dim + disable them.
//
// Detection keys on the ALPHA-mode preview (`rawPrev`, flattenAlpha:false), whose PNG
// preserves real transparency (see AtlasPickerPanel.tsx / PaletteThumbs.cpp). A cell is
// "dead" when the MAX alpha across its pixels is below the threshold — max (not mean) so a
// faint-but-real sprite (low average alpha) is never mislabeled.

/** Max per-pixel alpha (0..255) at/below which a cell counts as "effectively empty". This is
 *  deliberately "effectively empty", not strictly zero: a cell whose most-opaque pixel is
 *  ≤4/255 contributes <2% opacity — imperceptible, i.e. it renders essentially nothing, the
 *  same failure a strictly-zero slot has. Kept low so a genuinely faint-but-visible sprite is
 *  never dimmed; genuine empties measure exactly 0. */
export const DEAD_ALPHA_MAX = 4;

/**
 * Frame indices (k = row*side + col) whose atlas cell is effectively fully transparent.
 *
 * @param rgba      RGBA pixel buffer (e.g. from CanvasRenderingContext2D.getImageData).
 * @param imgW/imgH pixel dimensions of `rgba` (use the sampled image's natural size, which
 *                  may differ from the texture's srcW/srcH if the preview was downscaled).
 * @param side      grid dimension (cells per row/col); frames = side*side.
 * @param threshold max-alpha cutoff (default DEAD_ALPHA_MAX).
 *
 * Cell bounds are proportional (`floor(col*imgW/side)`), so a non-power-of-two or downscaled
 * image still partitions cleanly. A cell with no sampleable pixels (degenerate side > dim) is
 * left ALIVE — we never dim what we cannot verify. Invalid inputs yield an empty set.
 */
export function deadCellsFromAlpha(
  rgba: Uint8ClampedArray,
  imgW: number,
  imgH: number,
  side: number,
  threshold: number = DEAD_ALPHA_MAX,
): Set<number> {
  const dead = new Set<number>();
  if (side < 1 || imgW < 1 || imgH < 1 || rgba.length < imgW * imgH * 4) return dead;

  for (let row = 0; row < side; row++) {
    const y0 = Math.floor((row * imgH) / side);
    const y1 = Math.floor(((row + 1) * imgH) / side);
    if (y1 <= y0) continue; // zero-height cell (degenerate) — unverifiable, leave alive
    for (let col = 0; col < side; col++) {
      const x0 = Math.floor((col * imgW) / side);
      const x1 = Math.floor(((col + 1) * imgW) / side);
      if (x1 <= x0) continue; // zero-width cell — leave alive

      let maxA = 0;
      for (let y = y0; y < y1 && maxA < threshold; y++) {
        const rowBase = y * imgW;
        for (let x = x0; x < x1; x++) {
          const a = rgba[(rowBase + x) * 4 + 3];
          if (a > maxA) {
            maxA = a;
            if (maxA >= threshold) break; // cell is alive — stop scanning it
          }
        }
      }
      if (maxA < threshold) dead.add(row * side + col);
    }
  }
  return dead;
}

/**
 * Sample an alpha-mode preview PNG (a `data:` URI) and return its dead frame indices.
 *
 * The impure shell around {@link deadCellsFromAlpha}: decode the image, draw it to an
 * offscreen canvas at natural size, read RGBA, classify. Resolves to an EMPTY set on ANY
 * failure — no DOM/canvas (jsdom), a decode error, or a tainted read — because dead-cell
 * dimming is ADVISORY and must never throw into the picker. Component tests mock THIS
 * function with a known set (jsdom has no real canvas), keeping the pixel logic under the
 * pure {@link deadCellsFromAlpha} unit test.
 */
export async function computeDeadCells(dataUri: string, side: number): Promise<Set<number>> {
  try {
    if (side < 1 || typeof document === "undefined") return new Set();
    const img = await loadImage(dataUri);
    const w = img.naturalWidth, h = img.naturalHeight;
    if (w < 1 || h < 1) return new Set();
    const canvas = document.createElement("canvas");
    canvas.width = w;
    canvas.height = h;
    const ctx = canvas.getContext("2d");
    if (!ctx) return new Set();
    ctx.drawImage(img, 0, 0);
    // Same-origin data: URI never taints the canvas, so getImageData is safe; the
    // surrounding try/catch is the belt-and-braces backstop.
    const { data } = ctx.getImageData(0, 0, w, h);
    return deadCellsFromAlpha(data, w, h, side);
  } catch {
    return new Set();
  }
}

function loadImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error("atlas dead-cell probe: image decode failed"));
    img.src = src;
  });
}
