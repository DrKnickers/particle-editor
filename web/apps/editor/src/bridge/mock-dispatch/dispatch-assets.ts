// Mock counterpart of src/host/BridgeDispatch_Assets.cpp.

import type {
  Request,
  PaletteEntry,
} from "@particle-editor/bridge-schema";
import {
  useMockEngineState,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";
import { MOCK_ATLAS_PNG } from "./mock-atlas-png";

type AssetsKind =
  | "mods/list"
  | "mods/refresh"
  | "mods/set-layers"
  | "textures/browse"
  | "textures/palette/list"
  | "textures/palette/thumbnail"
  | "textures/get-preview"
  | "textures/palette/toggle-pin"
  | "textures/palette/touch-recent";

type AssetsRequest = Extract<Request, { kind: AssetsKind }>;

// Mock layer catalog: two top-level mods (FoCMod has Data\Art at its root;
// BaseGameMod has nested layers) + nested layers under FoCMod.
const MOCK_LAYERS: readonly { path: string; label: string; parentLabel?: string; parentPath?: string; isFoC: boolean; kind: "mod" | "nested" }[] = [
  { path: "C:/mock/corruption/Mods/FoCMod",          label: "FoCMod",   isFoC: true,  kind: "mod" },
  { path: "C:/mock/corruption/Mods/FoCMod/Bravo",     label: "Bravo",     parentLabel: "FoCMod", parentPath: "C:/mock/corruption/Mods/FoCMod", isFoC: true, kind: "nested" },
  { path: "C:/mock/corruption/Mods/FoCMod/Core", label: "Core", parentLabel: "FoCMod", parentPath: "C:/mock/corruption/Mods/FoCMod", isFoC: true, kind: "nested" },
  { path: "C:/mock/GameData/Mods/BaseGameMod",       label: "Demo Mod", isFoC: false, kind: "mod" },
];

// Layout-lane seed for the texture palette. Browser mode's palette is
// deliberately inert (no per-mod Store), so the tests-web geometry spec seeds
// entries through the dev-only window.__paletteTest seam, which calls
// seedMockPalette. null = unseeded = the inert default.
let mockPaletteSeed: PaletteEntry[] | null = null;
export function seedMockPalette(entries: PaletteEntry[] | null): void {
  mockPaletteSeed = entries;
}
function getSeededMockPalette(): PaletteEntry[] | null {
  return mockPaletteSeed;
}

export function dispatchAssets(this: MockDispatchHost, req: AssetsRequest): unknown {
  switch (req.kind) {

    // ---------------- mods -----------------------------------------
    //
    // Browser-mode MockBridge has no disk to scan, so `mods/list` /
    // `mods/refresh` return a small synthetic fixture (a flat layer catalog
    // + the ordered stack) sufficient for React component tests and design
    // iteration. `mods/set-layers` replaces the whole stack, mutates
    // activeModPath on the store, and fires engine/state/changed so
    // subscribed components see the new primary layer.
    case "mods/list":
    case "mods/refresh": {
      const mods = [
        { path: "C:/mock/corruption/Mods/FoCMod",    folderName: "FoCMod",      nickname: "",         isFoC: true,  rootHasArt: true },
        { path: "C:/mock/GameData/Mods/BaseGameMod", folderName: "BaseGameMod", nickname: "Demo Mod", isFoC: false, rootHasArt: true },
      ];
      return {
        mods,
        layers: [...MOCK_LAYERS],
        stack: [...this.layerStack],
        activePath: this.layerStack[0] ?? null,
      };
    }

    case "mods/set-layers": {
      const params = req.params as { paths: string[] };
      // Canonicalise (mock: keep only known catalog paths, dedup, preserve order).
      const known = new Set(MOCK_LAYERS.map((l) => l.path.toLowerCase()));
      const seen = new Set<string>();
      const next: string[] = [];
      for (const p of params.paths ?? []) {
        const k = p.replace(/[\\/]+$/, "");
        if (known.has(k.toLowerCase()) && !seen.has(k.toLowerCase())) { seen.add(k.toLowerCase()); next.push(k); }
      }
      this.layerStack = next;
      useMockEngineState.getState().applyPatch({ activeModPath: next[0] ?? null });
      this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
      return { ok: true, stack: [...this.layerStack] };
    }

    case "textures/browse":
      // No native file dialog in browser mode — simulate a cancelled
      // picker (empty filename). Callers branch on a non-empty string
      // before committing, so this is a clean no-op. The native host
      // opens GetOpenFileNameW in the active mod's texture folder.
      return { filename: "" };

    // ---------------- texture palette ----------------
    //
    // Browser mode has no per-mod Store and no texture decode, so the
    // palette is inert by default: empty pins/recents, null thumbnails,
    // no-op mutations. The TexturePickerField stays fully usable via Browse
    // and manual entry. The native host backs these with
    // TexturePalette::Store + EncodeThumbnailPng. The layout lane seeds
    // entries via seedMockPalette (dev seam window.__paletteTest) so
    // tests-web can measure a POPULATED popover's geometry.
    case "textures/palette/list": {
      const seeded = getSeededMockPalette();
      if (seeded) {
        return {
          hasMod: true,
          filter: req.params.slot,
          pins: seeded.filter((e) => e.pinned),
          recents: seeded.filter((e) => !e.pinned),
        };
      }
      return { hasMod: false, filter: req.params.slot, pins: [], recents: [] };
    }

    case "textures/palette/thumbnail":
      return { dataUri: null, status: "missing" as const };

    case "textures/get-preview": {
      // Browser mode has no async native decode worker, so the mock remains
      // synchronous and never returns status:"pending".
      const f = req.params.filename;
      if (f === "__missing__.dds") return { status: "missing" } as const;
      if (f === "__broken__.dds") return { status: "broken" } as const;
      return { status: "ok", dataUri: MOCK_ATLAS_PNG, srcW: 256, srcH: 256 } as const;
    }

    case "textures/palette/toggle-pin":
      return { ok: true, pinned: false };

    case "textures/palette/touch-recent":
      return { ok: true };

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
