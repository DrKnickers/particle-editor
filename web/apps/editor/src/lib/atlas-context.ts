// atlas-context.ts — Publishes ONLY what the curve editor uniquely owns (spec §4.5).
//
// Mirrors right-dock.ts: a writer-only publish fn + narrow read selector + a test reset.
// colorTexture/textureSize are NOT here — the panel reads those from
// emitters/get-properties so there is one source of truth.
import { create } from "zustand";
import type { TrackName, InterpolationType } from "@particle-editor/bridge-schema";

export interface AtlasSelection {
  /** Shared floor(value) when 1 key or N keys share a floor(value); else null. */
  frame: number | null;
  keyTimes: number[];
}

export interface AtlasContext {
  emitterId: number | null;                  // matches EmitterDto.id (number)
  focusedTrack: TrackName | null;            // consumers gate on === "index"
  interpolation: InterpolationType | null;   // for the header badge
  selection: AtlasSelection;
}

const EMPTY: AtlasContext = {
  emitterId: null,
  focusedTrack: null,
  interpolation: null,
  selection: { frame: null, keyTimes: [] },
};

const useStore = create<AtlasContext>(() => ({ ...EMPTY }));

/** Writer-only: called by CurveEditorPanel on focus/selection/emitter change. */
export function publishAtlasContext(ctx: AtlasContext): void {
  useStore.setState(ctx, true); // replace whole object atomically
}

export function getAtlasContext(): AtlasContext {
  return useStore.getState();
}

/** Narrow read hook — pass a selector so consumers re-render only on their slice. */
export function useAtlasContext<T>(selector: (c: AtlasContext) => T): T {
  return useStore(selector);
}

export function __resetAtlasContext(): void {
  useStore.setState({ ...EMPTY }, true);
}
