// Module-level caches for AtlasPickerPanel. The panel UNMOUNTS when the dock
// closes, so component state is lost; these survive the unmount so a re-open
// renders its first frame from the last known values instead of a placeholder.
//
// Kept out of AtlasPickerPanel.tsx so the component module exports only
// components (Fast Refresh) and the test reset lives with the other lib
// `__reset*ForTests` seams.

export type CachedAtlasEmitterProps = {
  id: number;
  textureSize: number;
  colorTexture: string;
  blendAlphaGated: boolean;
};

export const atlasPanelCache: {
  /** Last settled grid width; null until the first real measure. Lets a
   *  re-open render at its prior settled width immediately (no first-frame
   *  narrow transient before the ResizeObserver fires). */
  gridW: number | null;
  /** Last fetched emitter props. Seeding the initial state from this (when the
   *  id matches) renders the grid SYNCHRONOUSLY on a re-open's first frame,
   *  before the dock-slide tween starts, so the tween runs uncontended. The
   *  fetch still runs to confirm/refresh. null until the first successful fetch. */
  emitterProps: CachedAtlasEmitterProps | null;
} = { gridW: null, emitterProps: null };

/** Test-only: clear both caches so neither can leak across independent test
 *  cases — a width-mocking test would otherwise seed gridW (→ a different
 *  column count) into a later keyboard-nav test. */
export function __resetAtlasPanelCacheForTests(): void {
  atlasPanelCache.gridW = null;
  atlasPanelCache.emitterProps = null;
}
