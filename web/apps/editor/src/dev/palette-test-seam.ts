// DEV-ONLY test seam for the Playwright "web" layout lane (tests-web/*.spec.ts).
//
// Browser mode's texture palette is deliberately inert (no per-mod Store), so a
// layout spec could never measure a POPULATED popover — and #683 (the palette
// growing past a small window with its lower tiles unreachable) is only
// reproducible with enough entries to overflow. This seam seeds the mock
// bridge's palette with N synthetic entries; the spec then opens the popover
// through the real UI (select emitter → Appearance → palette button) and
// measures real geometry.
//
// Loaded ONLY behind `import.meta.env.DEV` via a dynamic import in main.tsx, so
// `vite build` excludes this module from the production bundle entirely — the
// same guarantee as window.__atlasTest (see src/dev/atlas-test-seam.ts).

import { seedMockPalette } from "@/bridge/mock";

export type PaletteTestApi = {
  /** Seed the mock palette with `pinned` pinned + `recent` recent entries
   *  (defaults sized to overflow a small window: 12 + 16). Pass 0/0 to reset
   *  to the inert default. */
  seedPalette: (opts?: { pinned?: number; recent?: number }) => void;
};

declare global {
  interface Window {
    __paletteTest?: PaletteTestApi;
  }
}

export function installPaletteTestSeam(): void {
  window.__paletteTest = {
    seedPalette(opts = {}) {
      const pinned = opts.pinned ?? 12;
      const recent = opts.recent ?? 16;
      if (pinned === 0 && recent === 0) {
        seedMockPalette(null);
        return;
      }
      const entries = [
        ...Array.from({ length: pinned }, (_, i) => ({
          filename: `p_pin_${String(i).padStart(2, "0")}.dds`,
          pinned: true,
          slotMask: 3,
        })),
        ...Array.from({ length: recent }, (_, i) => ({
          filename: `p_recent_${String(i).padStart(2, "0")}.dds`,
          pinned: false,
          slotMask: 3,
        })),
      ];
      seedMockPalette(entries);
    },
  };
}
