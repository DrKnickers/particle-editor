// DEV-ONLY test seam for the Playwright "web" layout lane (tests-web/*.spec.ts).
//
// The mock scene has no multi-frame-atlas emitter, and opening the Atlas Picker
// normally requires selecting an emitter + focusing the index curve track. This
// seam lets the layout spec drive the mock app into a known Atlas-Picker state in
// one call — without a native host or a CDP-blackened viewport (the spec measures
// React DOM layout only).
//
// Loaded ONLY behind `import.meta.env.DEV` via a dynamic import in main.tsx, so
// `vite build` excludes this module from the production bundle entirely. A test
// (production-bundle guard) greps the built dist to prove `__atlasTest` is absent.

import { useMockEmitterProperties } from "@/bridge/mock-state";
import { publishAtlasContext } from "@/lib/atlas-context";
import { setDock } from "@/lib/right-dock";

export type AtlasTestApi = {
  /** Seed an emitter with a real atlas, publish it as the index-channel selection,
   *  and open the Atlas Picker dock. Defaults to an 8×8 (64-cell) atlas. */
  seedAtlas: (opts?: {
    textureSize?: number;
    colorTexture?: string;
    emitterId?: number;
  }) => void;
};

declare global {
  interface Window {
    __atlasTest?: AtlasTestApi;
  }
}

export function installAtlasTestSeam(): void {
  window.__atlasTest = {
    seedAtlas(opts = {}) {
      const emitterId = opts.emitterId ?? 1;
      useMockEmitterProperties.getState().patch(emitterId, {
        textureSize: opts.textureSize ?? 64,
        colorTexture: opts.colorTexture ?? "fire.dds",
      });
      // focusedTrack "index" + a populated selection are what unblock the grid
      // (see the AtlasPickerPanel placeholder cascade).
      publishAtlasContext({
        emitterId,
        focusedTrack: "index",
        interpolation: "step",
        selection: { frame: 0, keyTimes: [0.3] },
      });
      setDock("atlas");
    },
  };
}
