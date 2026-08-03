// MockBridge — a fully-in-process Bridge implementation backed by a
// Zustand store (`mock-state.ts`). Used when the React app runs outside
// the WebView2 host (browser-mode design iteration, Vitest contract
// tests).
//
// Coverage:
//   - engine/state/snapshot                  full DTO
//   - engine/set/*  (17 setters)             mutates the store, then
//                                            emits engine/state/changed
//   - engine/action/* (4 actions)            mutates where appropriate,
//                                            emits engine/state/changed
//   - engine/query/* (3 queries)             read-only
//   - register-accelerators                  accepted as a no-op
//   - layout/viewport-rect                   accepted as a no-op
//   - layout/scene-rect                      accepted as a no-op
//   - animate-scene-rect                     accepted as a no-op
//   - host/backing-color                     accepted as a no-op
// Everything else (emitters/*, file/*, undo/*, spawner/*) rejects with
// a "not implemented" error — those land later.

import type {
  Bridge,
  Request,
  ResponseFor,
  Event,
  EventKind,
  EventOf,
  EngineStateDto,
  EmitterTreeDto,
  EmitterTreeNode,
  LightDto,
  LightingSettingsDto,
  PaletteEntry,
  ReferenceObjectStatus,
  SkydomeSlotStatus,
  SpawnParamsDto,
} from "@particle-editor/bridge-schema";
import {
  addDeathChildEmitter,
  addLifetimeChildEmitter,
  addRootEmitterMock,
  addTrackKeyInOverlay,
  deriveLockViews,
  copyEmittersToClipboard,
  deleteEmitter,
  deleteTrackKeysInOverlay,
  duplicateEmitter,
  duplicateWithIndexIncrement,
  duplicateWithIndexIncrementMany,
  findEmitterNode,
  makeDefaultEngineState,
  moveEmitterInTree,
  pasteEmittersFromClipboard,
  pasteAsChildFromClipboard,
  renameEmitter,
  reorderManyRoots,
  reorderRootEmitter,
  reparentEmitterInTree,
  setAllEmittersVisibleMock,
  setEmitterVisibleMock,
  setLinkGroupMembership,
  setTrackInterpolationInOverlay,
  setTrackLockInOverlay,
  setTrackKeyInOverlay,
  useMockEmitterClipboard,
  useMockEmitterProperties,
  useMockEmitterTree,
  useMockEngineState,
  useMockLinkGroupExempt,
  useMockLinkGroupConflicts,
  useMockRecentFiles,
  useMockTrackOverlay,
  snapshotEngineState,
} from "./mock-state";

/** Returns true for request kinds that should mark the in-memory file
 *  state dirty. Every engine/set/* is mutating. Engine actions are
 *  mutating except for the read-only-ish reload-shaders / reload-textures
 *  / on-particle-system-changed / step-frames, which don't change
 *  user-visible parameters. file/*, query/*, undo/perform, spawner/*,
 *  layout, accelerators are not. The native host applies the same rule
 *  via per-handler `SetDirty(true)` calls. */
function isMutating(kind: Request["kind"]): boolean {
  // engine/set/paused (view-only preview clock toggle) and
  // engine/set/heat-debug (view-only debug overlay) are excluded —
  // both leave the document state untouched and shouldn't trigger
  // save-prompt gates. Native host applies the same rule in
  // BridgeDispatcher.cpp.
  if (kind === "engine/set/paused") return false;
  // [guard-config] View-only preview setting — same rule as paused;
  // native host mirrors this (handler never marks dirty).
  if (kind === "engine/set/overload-guard") return false;
  // [hard-guard] Estimated-load push is view-only; the browser preview
  // has no engine sim so the value has no effect here.
  if (kind === "engine/set/estimated-load") return false;
  if (kind === "engine/set/heat-debug") return false;
  // MSAA level, model-shadows, and soft-shadows are view-only display
  // preferences — the native host's handlers explicitly never mark the
  // document dirty (BridgeDispatcher.cpp: "View-only display preference").
  // Excluding them here keeps browser/jsdom mode from booting dirty off
  // AppShell's startup pushes of these kinds.
  if (kind === "engine/set/msaa-level") return false;
  if (kind === "engine/set/model-shadows") return false;
  if (kind === "engine/set/soft-shadows") return false;
  // Ground-plane visibility is a global VIEW preference (registry-persisted,
  // not part of the .alo document); native host no longer marks dirty (#617).
  if (kind === "engine/set/ground") return false;
  // stats/set-frozen is a test-only knob; never mutating.
  if (kind === "stats/set-frozen") return false;
  if (kind.startsWith("engine/set/")) return true;
  // engine/action/clear is destructive — destroying particles in the
  // world is a user-visible mutation worth a save-prompt gate.
  if (kind === "engine/action/clear") return true;
  // engine/action/rescale-system mutates emitter parameters.
  if (kind === "engine/action/rescale-system") return true;
  // Per-emitter rescale + structural mutations are
  // all mutating. Link-group exempt-set edits change propagation
  // behaviour but not engine-observable particle output; flag them
  // anyway so the dirty-bit + save-prompt gate matches the native
  // host's `markDirty` rule.
  if (kind === "engine/action/rescale-emitter") return true;
  if (kind === "emitters/duplicate") return true;
  if (kind === "emitters/duplicate-many") return true;
  if (kind === "emitters/delete") return true;
  if (kind === "emitters/delete-many") return true;
  if (kind === "emitters/rename") return true;
  if (kind === "emitters/duplicate-with-index-increment") return true;
  if (kind === "emitters/duplicate-with-index-increment-many") return true;
  // Add-child / move / link-group-membership all
  // change persisted tree state, so they ride the dirty bit.
  if (kind === "emitters/add-lifetime-child") return true;
  if (kind === "emitters/add-death-child") return true;
  if (kind === "emitters/add-root") return true;
  if (kind === "emitters/move") return true;
  if (kind === "emitters/move-many") return true;
  if (kind === "linkGroups/set-membership") return true;
  // Drag/drop reorder + reparent. Both modes
  // mutate persisted tree state.
  if (kind === "emitters/drop") return true;
  // Multi-select drag-reorder — same structural-mutation tier as emitters/drop.
  if (kind === "emitters/reorder-many") return true;
  // Clipboard. `copy` doesn't mutate the tree;
  // `cut` (delete) + `paste` (insert) both do. Matches the native
  // host's per-handler `SetDirty` rule.
  if (kind === "emitters/cut") return true;
  if (kind === "emitters/paste") return true;
  if (kind === "emitters/paste-as-child") return true;
  if (kind === "linkGroups/set-exempt-fields") return true;
  if (kind === "linkGroups/reset-exempt-fields") return true;
  // Track key deletion + interpolation
  // toggle are persisted mutations on the per-emitter Track state.
  if (kind === "emitters/delete-track-keys") return true;
  if (kind === "emitters/set-track-interpolation") return true;
  if (kind === "emitters/set-track-lock") return true;
  // Drag-to-move + click-to-add land in the same
  // mutating tier as delete + interpolation: both edit per-emitter
  // Track state.
  if (kind === "emitters/set-track-key") return true;
  if (kind === "emitters/add-track-key") return true;
  if (kind === "emitters/add-track-keys") return true;
  // Per-emitter property patch.
  if (kind === "emitters/set-properties") return true;
  return false;
}

/**
 * Whether a mutating request ACTUALLY changed persisted state — gates the
 * dirty bit so refused / no-op drag-commits stay clean, matching the native
 * host's per-handler actual-state rules. Most
 * mutating kinds always mutate when they return normally; slot setters and
 * drag commits are conditional:
 *  - ground/skydome compare the returned actual slot with the pre-call slot,
 *    so invalid requests and valid no-ops stay clean while a failed load that
 *    falls back to Dirt/Off still dirties.
 *  - emitters/drop, emitters/reorder-many → `{ ok:false }` on refusal.
 *  - emitters/move-many → no-op when the block is edge-pinned (nothing moved);
 *    detected from the pre-move root order, since the response always returns
 *    the surviving newIds regardless.
 */
function didMutate(
  req: Request,
  result: unknown,
  preMoveRootOrder: number[] | null,
  preEngineSlot: number | null,
): boolean {
  switch (req.kind) {
    case "engine/set/ground-texture":
    case "engine/set/skydome-slot":
      return (result as { slot: number }).slot !== preEngineSlot;
    case "emitters/drop":
    case "emitters/reorder-many":
      return (result as { ok?: boolean }).ok !== false;
    case "emitters/move-many": {
      const order = preMoveRootOrder ?? [];
      if (order.length === 0) return false;
      const edge = req.params.direction === "up" ? order[0]! : order[order.length - 1]!;
      // Block pinned against the edge in the move direction → nothing moves.
      return !req.params.ids.includes(edge);
    }
    default:
      return true;
  }
}

// live spawn values come from the properties overlay at emit time —
// ONE decoration point instead of mirroring into the tree store from every
// mutation handler. Tree-node literals carry ZERO_SPAWN purely to satisfy
// the type; this override is the source of truth.
function pickSpawn(id: number): SpawnParamsDto {
  const p = useMockEmitterProperties.getState().read(id);
  return {
    lifetime: p.lifetime,
    useBursts: p.useBursts,
    nBursts: p.nBursts,
    burstDelay: p.burstDelay,
    nParticlesPerSecond: p.nParticlesPerSecond,
    nParticlesPerBurst: p.nParticlesPerBurst,
  };
}

function decorateSpawn(node: EmitterTreeNode): EmitterTreeNode {
  return {
    ...node,
    // The synthetic root (id -1) keeps its stored ZERO_SPAWN — host parity
    // (the native synthetic roots serialize all-zeros, and the estimator
    // never reads the root's spawn).
    spawn: node.id === -1 ? node.spawn : pickSpawn(node.id),
    children: node.children.map(decorateSpawn),
  };
}

// Browser mode can't probe a real .alo, so these canned Names stand in
// for "skinned / unsupported" objects — selecting one drives the picker's
// "not supported" status path. Must match a Name in the reference-object-list.
const MOCK_SKINNED_REFS = new Set<string>(["Stormtrooper_Squad"]);

// Names that resolve in the catalog but whose model file is absent from the
// mod/base (getFile miss) — selecting one drives the "model file not found" status
// path (distinct from skinned / corrupt). Must match a Name in the catalog list.
// A structure (the picker now lists units + structures only; the earlier prop
// example would be filtered out, so the missing-model case rides a kept category).
const MOCK_MISSING_MODELS = new Set<string>(["Sensor_Array_NoModel"]);

// Mock layer catalog: two top-level mods (FoCMod has Data\Art at its root;
// BaseGameMod has nested layers) + nested layers under FoCMod.
const MOCK_LAYERS: readonly { path: string; label: string; parentLabel?: string; parentPath?: string; isFoC: boolean; kind: "mod" | "nested" }[] = [
  { path: "C:/mock/corruption/Mods/FoCMod",          label: "FoCMod",   isFoC: true,  kind: "mod" },
  { path: "C:/mock/corruption/Mods/FoCMod/Bravo",     label: "Bravo",     parentLabel: "FoCMod", parentPath: "C:/mock/corruption/Mods/FoCMod", isFoC: true, kind: "nested" },
  { path: "C:/mock/corruption/Mods/FoCMod/Core", label: "Core", parentLabel: "FoCMod", parentPath: "C:/mock/corruption/Mods/FoCMod", isFoC: true, kind: "nested" },
  { path: "C:/mock/GameData/Mods/BaseGameMod",       label: "Demo Mod", isFoC: false, kind: "mod" },
];

// Browser mode can't load a real .alo, so these canned dome Names stand in
// for "chosen but the .alo wouldn't load" — selecting one drives the picker's
// load-failed status path + the solid-colour fallback indicator.
const MOCK_MISSING_DOMES = new Set<string>(["Broken_Sky"]);

// Layout-lane seed for the texture palette (#683). Browser mode's palette is
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

// Representative average colour per built-in ground slot — the mock has no real
// textures, so it stands in for the host's GetGroundColor() (which averages the
// loaded texture). Slot 4 is the solid-colour slot (uses groundSolidColor).
const MOCK_GROUND_TEXTURE_COLOR: Record<number, number> = {
  0: 0x00334455, // dirt — brown, dark
  1: 0x00204a2a, // grass — green, medium-dark
  2: 0x0060a8c8, // sand — tan, light
  3: 0x00f0f0f0, // snow — near-white
};
function mockGroundColor(slot: number, solidColor: number): number {
  if (slot === 4) return solidColor; // kGroundSolidColorSlot
  return MOCK_GROUND_TEXTURE_COLOR[slot] ?? 0x00808080; // custom/unknown → mid-grey
}

// A valid deterministic 256×256 checkerboard PNG for the atlas picker mock.
// 4×4 grid of 64px squares alternating two grays (#CC and #44). Generated from
// raw RGBA scanlines via Node zlib.deflateSync + hand-assembled PNG chunks — no
// third-party deps. At 256×256 each 4-column atlas crop = 64px legible cells.
// Sentinels: "__missing__.dds" → status:"missing", "__broken__.dds" → status:"broken".
const MOCK_ATLAS_PNG = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAQAAAAEACAYAAABccqhmAAAFoElEQVR42u3UQQEAMAgDMcQhH0+bif6aBwquZO7uNd/uVp/+3f3HAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAAAAAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6B8EwAN4AP17DwAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAAAIABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPRPAuABPID+vQcAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAOgPAAMAgP4AMAAA6A8AAwCA/gAwAADoDwADAID+ADAAAADAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6A8AAAKA/AAwAAPoDwAAAoD8ADAAA+gPAAACgPwAMAAD6JwHwAB5A/94DAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAD0B4ABAEB/ABgAAPQHgAEAQH8AGAAA9AeAAQBAfwAYAAAAYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/QFgAADQHwAGAAD9AWAAANAfAAYAAP0BYAAA0B8ABgAA/YP3AXIVJtrUiwADAAAAAElFTkSuQmCC";

export class MockBridge implements Bridge {
  private listeners = new Map<EventKind, Set<(e: Event) => void>>();

  /** In-mock "active spawner instance count". Bumped
   *  by spawner/trigger (by burstSize), zeroed by spawner/stop. The
   *  native SpawnerDriver tracks real ParticleSystemInstance lifecycles;
   *  the mock counter is just a hook for UI badge testing. */
  private spawnerActiveCount = 0;

  /** Cross-mode Force Align flag. The native host persists this as the
   *  REG_DWORD `LightingForceFillAlignment`; browser mode keeps it in
   *  memory, defaulting to true (the legacy Win32 UI's force-align default).
   *  Read by `settings/lighting-force-align`, written by `…/set`. */
  private lightingForceAlign = true;

  /** Cross-mode raw lighting split. The native host persists this in the
   *  registry; browser mode keeps the last `settings/lighting/set` payload
   *  in memory so the panel's edits + Reset round-trip within a session.
   *  `null` until first written → `settings/lighting` returns the canonical
   *  defaults (with the live `lightingForceAlign` flag). */
  private lightingOverride: LightingSettingsDto | null = null;

  // [guard-config] Last engine/set/overload-guard params received —
  // test-observable; no mock behavior depends on it.
  lastOverloadGuard: { enabled: boolean; maxParticles: number } | null = null;

  // Ordered content-layer stack (absolute slash-free paths, front = highest
  // precedence; [] = Unmodded). Not part of the engine-state snapshot DTO — surfaced
  // only via the mods/list payload + driven by mods/set-layers.
  private layerStack: string[] = [];

  async request<R extends Request>(req: R): Promise<ResponseFor<R>> {
    // Capture pre-mutation root order for move-many, whose dirtiness depends on
    // whether anything actually moved (edge-pinned block → no move). The host
    // gates markDirty on its anyMoved flag; we reconstruct the same condition.
    const preMoveRootOrder =
      req.kind === "emitters/move-many"
        ? useMockEmitterTree.getState().tree.root.children.map((c) => c.id)
        : null;
    const preEngineSlot =
      req.kind === "engine/set/ground-texture"
        ? snapshotEngineState().groundTexture
        : req.kind === "engine/set/skydome-slot"
          ? snapshotEngineState().skydomeSlot
          : null;
    const result = this.handle(req);
    // After the handler completes, mark dirty for any engine mutation —
    // but only on a REAL mutation. A refused drag-commit (ok:false) or a
    // no-op batch move leaves the document clean, mirroring the native host
    // (which marks dirty only on the success branch of each handler). The
    // mock previously fired this unconditionally for any mutating kind, so a
    // refused/no-op drag-commit falsely dirtied the doc.
    // (file/* and engine/action/reload-* / clear are deliberately NOT
    // marked dirty — see isMutating below.)
    if (isMutating(req.kind) && didMutate(req, result, preMoveRootOrder, preEngineSlot)) {
      this.markDirty();
    }
    return result as ResponseFor<R>;
  }

  on<K extends EventKind>(kind: K, handler: (e: EventOf<K>) => void): () => void {
    let bucket = this.listeners.get(kind);
    if (!bucket) {
      bucket = new Set();
      this.listeners.set(kind, bucket);
    }
    bucket.add(handler as (e: Event) => void);
    return () => { bucket?.delete(handler as (e: Event) => void); };
  }

  // ---------------------------------------------------------------- internals

  private emit(e: Event): void {
    // decorate tree payloads with live spawn values at the single
    // event choke point (see decorateSpawn above).
    if (e.kind === "emitters/tree/changed") {
      e = { ...e, payload: { ...e.payload, root: decorateSpawn(e.payload.root) } };
    }
    const bucket = this.listeners.get(e.kind);
    bucket?.forEach((h) => h(e));
  }

  /** Patch the store and broadcast engine/state/changed with the full snapshot. */
  private patchAndBroadcast(patch: Partial<EngineStateDto>): void {
    useMockEngineState.getState().applyPatch(patch);
    this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
  }

  /** Every mutating setter/action sets dirty=true. The
   *  debounce (don't re-emit if already dirty) avoids spamming
   *  `dirty/changed` on every slider drag tick. The native host applies
   *  the same rule. */
  private markDirty(): void {
    if (snapshotEngineState().dirty) return;
    useMockEngineState.getState().applyPatch({ dirty: true });
    this.emit({ kind: "dirty/changed", payload: { dirty: true } });
    // Don't re-emit engine/state/changed here — the caller's
    // patchAndBroadcast already fired one (or will fire one) with the
    // updated dirty=true field. The dirty/changed event is the
    // dedicated narrow-payload channel for components watching only
    // the dirty bit (window title, save-prompt gates).
  }

  /** Clear dirty + emit. Used by file/new, file/open, file/save success. */
  private markClean(): void {
    const cur = snapshotEngineState();
    if (!cur.dirty) return;
    useMockEngineState.getState().applyPatch({ dirty: false });
    this.emit({ kind: "dirty/changed", payload: { dirty: false } });
  }

  /** Update currentFilePath, push to recents (dedup, cap 9), emit
   *  recent/changed + engine/state/changed. Used by file/open and
   *  file/save success paths. */
  private commitFilePath(path: string): void {
    useMockEngineState.getState().applyPatch({ currentFilePath: path });
    const recents = useMockRecentFiles.getState().push(path);
    this.emit({ kind: "recent/changed", payload: { paths: recents } });
    this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
  }

  private handle(req: Request): unknown {
    switch (req.kind) {
      // ---------------- engine state ----------------
      case "engine/state/snapshot":
        return snapshotEngineState();

      // ---------------- engine setters: ground ----------------
      case "engine/set/ground":
        this.patchAndBroadcast({ ground: req.params.enabled });
        return {};

      case "engine/set/ground-z":
        this.patchAndBroadcast({ groundZ: req.params.z });
        return {};

      case "engine/set/ground-texture": {
        const requestedSlot = req.params.slot;
        const before = snapshotEngineState();
        const inRange = requestedSlot >= 0 && requestedSlot < before.groundSlotCustomPaths.length;
        // Browser mode treats built-in slots 0..4 as available. Custom slots
        // mirror the native engine and require a path before selection.
        const available =
          inRange &&
          (requestedSlot <= 4 || (before.groundSlotCustomPaths[requestedSlot] ?? "") !== "");
        if (available) {
          this.patchAndBroadcast({
            groundTexture: requestedSlot,
            groundColor: mockGroundColor(requestedSlot, before.groundSolidColor),
          });
        } else {
          this.patchAndBroadcast({});
        }
        const actualSlot = available ? requestedSlot : before.groundTexture;
        return { slot: actualSlot, applied: available };
      }

      case "engine/set/ground-solid-color":
        this.patchAndBroadcast(
          snapshotEngineState().groundTexture === 4
            ? { groundSolidColor: req.params.rgb, groundColor: req.params.rgb }
            : { groundSolidColor: req.params.rgb },
        );
        return {};

      case "engine/set/ground-slot-custom-path": {
        const { slot, path } = req.params;
        const snap = snapshotEngineState();
        const paths = [...snap.groundSlotCustomPaths];
        if (slot >= 0 && slot < paths.length) paths[slot] = path;
        const patch: Partial<EngineStateDto> = { groundSlotCustomPaths: paths };
        // Mirror the host: changing the SELECTED slot's texture refreshes the floor colour.
        if (slot === snap.groundTexture) patch.groundColor = mockGroundColor(slot, snap.groundSolidColor);
        this.patchAndBroadcast(patch);
        return {};
      }

      // ---------------- engine setters: skydome / background ----------------
      case "engine/set/skydome-slot": {
        const requestedSlot = req.params.slot;
        const before = snapshotEngineState();
        const inRange = requestedSlot >= 0 && requestedSlot < 12;
        const customPath =
          requestedSlot >= 9 ? (before.skydomeCustomPaths[requestedSlot - 9] ?? "") : "";
        const applied = inRange && (requestedSlot < 9 || customPath !== "");
        // The native setter falls back to Off when a valid custom slot cannot
        // load. Invalid indices leave the prior slot untouched.
        const actualSlot = applied ? requestedSlot : inRange ? 0 : before.skydomeSlot;
        this.patchAndBroadcast({ skydomeSlot: actualSlot });
        return { slot: actualSlot, applied };
      }

      case "engine/set/skydome-custom-path": {
        const { slot, path } = req.params;
        const customPaths = [...snapshotEngineState().skydomeCustomPaths];
        // slot is the absolute engine slot index (9..11); map to 0..2 in the
        // custom-only array.
        const idx = slot - 9;
        if (idx >= 0 && idx < customPaths.length) customPaths[idx] = path;
        this.patchAndBroadcast({ skydomeCustomPaths: customPaths });
        return {};
      }

      // game-dome environment: store context + the two chosen Names.
      // derive each slot's load outcome so the picker can surface a
      // chosen-but-unloadable dome (mirrors the native RebuildSkydomeMeshes).
      case "engine/set/skydome-environment": {
        const slotStatus = (name: string): SkydomeSlotStatus =>
          name === "" ? "none" : MOCK_MISSING_DOMES.has(name) ? "load-failed" : "ok";
        this.patchAndBroadcast({
          skydomeContext: req.params.context,
          skydomePrimaryName: req.params.primaryName,
          skydomeSecondaryName: req.params.secondaryName,
          skydomePrimaryStatus: slotStatus(req.params.primaryName),
          skydomeSecondaryStatus: slotStatus(req.params.secondaryName),
        });
        return {};
      }

      // imported reference object: select by Name (browser mode can't probe
      // a real .alo, so a canned skinned-set drives the "not supported" status),
      // toggle visibility, set transform; plus the unit grid toggle/spacing.
      case "engine/set/reference-object": {
        const name = req.params.name;
        const status: ReferenceObjectStatus =
          name === ""                      ? "none"
          : MOCK_MISSING_MODELS.has(name)  ? "model-missing"
          : MOCK_SKINNED_REFS.has(name)    ? "skinned"
          :                                  "ok";
        this.patchAndBroadcast({ referenceObjectName: name, referenceObjectStatus: status });
        return {};
      }

      case "engine/set/reference-object-visible":
        this.patchAndBroadcast({ referenceObjectVisible: req.params.visible });
        return {};

      case "engine/set/reference-object-lock":
        this.patchAndBroadcast({ referenceObjectLocked: req.params.locked });
        return {};

      case "engine/set/reference-object-transform":
        // Mirror the native bridge: a locked object drops
        // UI-routed transform requests (the picker also disables the inputs).
        if (useMockEngineState.getState().referenceObjectLocked) return {};
        this.patchAndBroadcast({
          referenceObjectPosition: req.params.position,
          referenceObjectRotation: req.params.rotation,
        });
        return {};

      case "engine/set/grid-visible":
        this.patchAndBroadcast({ gridVisible: req.params.visible });
        return {};

      case "engine/set/grid-spacing":
        this.patchAndBroadcast({
          gridSpacing: req.params.spacing > 0 ? req.params.spacing : 1,
        });
        return {};

      case "engine/set/snap-enabled":
        this.patchAndBroadcast({ snapEnabled: req.params.enabled });
        return {};

      case "engine/set/background":
        this.patchAndBroadcast({ background: req.params.rgb });
        return {};

      // ---------------- engine setters: bloom ----------------
      case "engine/set/bloom":
        this.patchAndBroadcast({ bloom: req.params.enabled });
        return {};

      case "engine/set/bloom-strength":
        this.patchAndBroadcast({ bloomStrength: req.params.v });
        return {};

      case "engine/set/bloom-cutoff":
        this.patchAndBroadcast({ bloomCutoff: req.params.v });
        return {};

      case "engine/set/bloom-size":
        this.patchAndBroadcast({ bloomSize: req.params.v });
        return {};

      // Leave particles after instance death. Mirrors the
      // native ParticleSystem::setLeaveParticles handler in
      // BridgeDispatcher.cpp.
      case "engine/set/leave-particles":
        this.patchAndBroadcast({ leaveParticles: req.params.enabled });
        return {};

      // ---------------- engine setters: debug / camera / lighting ----------------
      case "engine/set/heat-debug":
        this.patchAndBroadcast({ heatDebug: req.params.enabled });
        return {};

      case "engine/set/camera":
        this.patchAndBroadcast({ camera: { ...req.params } });
        return {};

      case "engine/set/light": {
        const { which, diffuse, specular, position, direction } = req.params;
        const next: LightDto = { diffuse, specular, position, direction };
        const lights = { ...snapshotEngineState().lights, [which]: next };
        this.patchAndBroadcast({ lights });
        return {};
      }

      case "engine/set/ambient":
        this.patchAndBroadcast({ ambient: req.params.color });
        return {};

      case "engine/set/shadow":
        this.patchAndBroadcast({ shadow: req.params.color });
        return {};

      // ---------------- engine setters: view state (preview clock) ----------------
      case "engine/set/paused":
        this.patchAndBroadcast({ paused: req.params.paused });
        return {};

      case "engine/set/overload-guard":
        // View-only preview config; the mock has no simulation to govern —
        // store it so contract tests can assert the round-trip.
        this.lastOverloadGuard = { ...req.params };
        return {};

      case "engine/set/msaa-level":
        // MockBridge: no GPU to configure — accept as a no-op.
        return {};

      case "engine/set/model-shadows":
        // MockBridge: no engine renderer — accept as a no-op.
        return {};

      case "engine/set/soft-shadows":
        // MockBridge: no engine renderer — accept as a no-op.
        return {};

      case "engine/set/estimated-load":
        // [hard-guard] The browser preview has no engine sim, so the
        // estimated-load value has no effect here; accept as a no-op so
        // web code paths are identical to native.
        return {};

      // ---------------- stats freeze (test-only knob) ----------------
      // Mock parity for native stats/set-frozen. Browser
      // mode emits no stats/tick, so freezing is largely no-op, but
      // emit the frozen-changed event for any consumer that listens.
      case "stats/set-frozen":
        this.emit({
          kind: "stats/frozen-changed",
          payload: { frozen: req.params.frozen },
        });
        return {};

      // ---------------- engine actions ----------------
      case "engine/action/clear":
        // No engine-state mutation; emit anyway so any UI watching for the
        // post-action redraw cue still fires.
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};

      case "engine/action/reload-shaders":
      case "engine/action/reload-textures":
      case "engine/action/on-particle-system-changed":
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};

      // Step one or more frames. In browser mode there's no engine clock
      // to advance; the response-only no-op keeps the schema reachable so
      // UI surfaces can wire the dispatch without a runtime error.
      case "engine/action/step-frames":
        return {};

      // Cascade-reset background, ground, bloom,
      // skydome, lighting back to engine defaults. The mock applies
      // a patch of just the view-setting fields (background / ground
      // / skydome / bloom) so editor state — currentFilePath, dirty
      // flag — is preserved across the reset. Emits one
      // engine/state/changed at the end.
      case "engine/action/reset-view-settings": {
        const defaults = makeDefaultEngineState();
        useMockEngineState.getState().applyPatch({
          background:    defaults.background,
          ground:        defaults.ground,
          groundZ:       defaults.groundZ,
          groundTexture: defaults.groundTexture,
          skydomeSlot:   defaults.skydomeSlot,
          bloom:         defaults.bloom,
          bloomStrength: defaults.bloomStrength,
          bloomCutoff:   defaults.bloomCutoff,
          bloomSize:     defaults.bloomSize,
        });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      // Rescale the whole particle system by a duration / size percentage.
      // MockBridge has no ParticleSystem to mutate; the handler logs the
      // call (so Vitest can assert on it via a spy) and emits the standard
      // post-action state/changed cue. Returns {} per schema.
      case "engine/action/rescale-system":
        console.log(
          "[MockBridge] engine/action/rescale-system",
          req.params,
        );
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};

      // ---------------- engine queries ----------------
      case "engine/query/ground-slot-empty": {
        const { slot } = req.params;
        const state = snapshotEngineState();
        const paths = state.groundSlotCustomPaths;
        // Mirrors Engine::IsGroundSlotEmpty: a slot is "empty" (no editor-owned
        // asset) unless it's dirt (slot 0, bundled IDB_GROUND), the procedural solid
        // colour (slot 4), or has a user custom path. Game-sourced slots 1..3
        // (grass/sand/snow) have NO bundled resource, so they read empty here even
        // though they render from the game install -- availability is a separate query.
        const hasBuiltin = slot === 0 || slot === 4;  // dirt + procedural solid only
        const hasCustom  = slot >= 0 && slot < paths.length && (paths[slot] ?? "") !== "";
        return !(hasBuiltin || hasCustom);
      }

      case "engine/query/skydome-slot-empty": {
        const { slot } = req.params;
        const state = snapshotEngineState();
        // Slots 0..8 are bundled (slot 0 = Off, never "empty" in the
        // picker sense — single-click commits it). Slots 9..11 are
        // empty iff their custom path is empty.
        if (slot >= 0 && slot < 9) return false;
        const idx = slot - 9;
        const paths = state.skydomeCustomPaths;
        if (idx < 0 || idx >= paths.length) return true;
        return (paths[idx] ?? "") === "";
      }

      // Enumerate selectable game-dome Names. Browser mode has no disk
      // to read *Skydomes.xml from, so return a small canned set per context
      // (real Names from vanilla FoC) to exercise the picker dispatch surface.
      case "engine/query/skydome-list": {
        const lists = {
          space: {
            // "Broken_Sky" is in MOCK_MISSING_DOMES — selecting it drives the
            // load-failed status path (browser-mode stand-in for an .alo
            // that won't load).
            primary: ["Stars_Low", "Stars_Medium", "Stars_High", "Stars_Cinematic", "Broken_Sky"],
            secondary: ["Star_Backdrop_Blue", "Star_Backdrop_Green", "Nebula_Field_Blue"],
          },
          land: {
            primary: ["Day_Blue_Sky", "Day_Clear_Sky", "Day_Storm_Sky", "Night_Stars"],
            secondary: ["Planet_Rings00", "Horizon_Haze00"],
          },
        };
        return req.params.context === "land" ? lists.land : lists.space;
      }

      // Enumerate selectable game objects. Browser mode has no disk to read
      // GameObjectFiles.xml from, so return a small canned set (real vanilla Names
      // across categories) to exercise the picker dispatch + grouping surface.
      // Units + structures only — mirrors Engine::EnumerateReferenceObjects
      // (props/projectiles/uncategorized are filtered out engine-side); `building`
      // is always false here (browser mode has no off-thread catalog build).
      case "engine/query/reference-object-list":
        return {
          building: false,
          // {domain, role, bucket} drive the collapsible tree; affiliation drives the faction chips.
          objects: [
            { name: "AT_AT_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle", affiliation: "Empire" },
            { name: "AT_ST_Walker", domain: "Ground", role: "Unit", bucket: "Vehicle", affiliation: "Empire" },
            { name: "Stormtrooper_Squad", domain: "Ground", role: "Unit", bucket: "Infantry", affiliation: "Empire" }, // MOCK_SKINNED_REFS
            { name: "Rebel_Barracks", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Rebel" },
            { name: "Empire_Anti_Aircraft_Turret", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Empire" },
            { name: "Sensor_Array_NoModel", domain: "Ground", role: "Structure", bucket: "Structure", affiliation: "Rebel" }, // MOCK_MISSING_MODELS
            { name: "Imperial_Bunker_Capturable", domain: "Ground", role: "Unit", bucket: "Other", affiliation: "" }, // no affiliation -> only under "All"
            { name: "Star_Destroyer", domain: "Space", role: "Unit", bucket: "Capital", affiliation: "Empire" },
            { name: "Nebulon_B_Frigate", domain: "Space", role: "Unit", bucket: "Frigate", affiliation: "Rebel, Empire" }, // multi-faction
            { name: "TIE_Fighter", domain: "Space", role: "Unit", bucket: "Fighter", affiliation: "Empire" },
            { name: "Imperial_Star_Base", domain: "Space", role: "Structure", bucket: "Structure", affiliation: "Empire" },
            { name: "Darth_Vader", domain: "Ground", role: "Hero", bucket: "Hero", affiliation: "Empire" },
            { name: "Emperor_Palpatine", domain: "Space", role: "Hero", bucket: "Hero", affiliation: "Empire" },
            // Props + templates: their own flat sections, exempt from the fieldable gate.
            { name: "Asteroid_Field_Prop", domain: "Space", role: "Prop", bucket: "Other", affiliation: "" },
            { name: "Rebel_Crate_Prop", domain: "Ground", role: "Prop", bucket: "Other", affiliation: "Rebel" },
            { name: "Generic_Frigate_Template", domain: "Space", role: "Template", bucket: "Other", affiliation: "" },
          ],
        };

      case "engine/query/bloom-available":
        return snapshotEngineState().bloomAvailable;

      case "engine/query/msaa-levels":
        // MockBridge: report 0/2/4 as supported (8 deliberately excluded
        // so tests can verify that the UI clamps/hides unsupported levels).
        return { levels: [0, 2, 4], current: 4 };

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

      // ---------------- host plumbing: accepted no-ops ----------------
      case "register-accelerators":
        // Mock: nothing to register. Accelerator handling lives in the
        // native host; in browser mode the design iteration doesn't need
        // a real hotkey system, so swallow the call.
        return {};

      case "app/quit":
        // Mock: no host window to close. In browser mode the design
        // iteration doesn't need a real "quit" — the dev server keeps
        // running. Accept the request silently.
        return {};

      // ---------------- autosave crash-recovery ----------------
      case "autosave/check-recovery":
        // Mock: no %TEMP% scan in browser mode — always "no orphan", so the
        // recovery dialog never appears under `pnpm dev` / web vitest.
        // Component tests that exercise the dialog inject an orphan directly
        // (they render AutosaveRecoveryDialog, not the whole shell).
        return { orphan: null };

      case "autosave/recover":
        // Mock: no document to swap. Report success per the chosen action.
        return { status: req.params?.choice === "discard" ? "discarded" : "recovered" };

      case "layout/viewport-rect":
        // Mock: no native HWND to reposition.
        return {};

      case "layout/scene-rect":
        // Mock: no native AlphaCompositor to mask.
        return {};

      case "animate-scene-rect":
        // Mock: no native viewport-anim system (the host interpolates the
        // dock-slide rect under this architecture). Accept silently.
        return {};

      case "host/backing-color":
        // Mock: no native DComp backing visual to recolour.
        return {};

      case "viewport/capture-snapshot":
        // Mock: no engine to snapshot. Empty image + zero dims so the
        // React Modal's render guard (`snapshot && snapshot.imageBase64`)
        // short-circuits the <img> portal in unit tests.
        return { imageBase64: "", w: 0, h: 0 };

      case "viewport/input":
        // Mock: no native HWND to PostMessage to.
        // Tests assert on `dispatch` call args (kind + payload shape);
        // the return shape is the standard empty-object ack. Browser
        // mode never has an engine to drive, so this is a pure no-op.
        return {};

      // ---------------- file ops ----------
      //
      // The mock implementations are deliberately UI-free: there's no
      // real picker, no on-disk read/write. They simulate the host's
      // observable side-effects (currentFilePath, dirty, recentFiles)
      // so React handlers + Playwright specs can exercise the round
      // trip in browser mode. The schema-level contract (return shapes,
      // event ordering) matches the native host.
      //
      // Two historical callers also depend on this:
      //   - BackgroundPicker chains file/open → set skydome-custom-path
      //     → set skydome-slot. In browser mode (with no real picker)
      //     the call still resolves with ok:false so the chain aborts
      //     cleanly without surfacing a raw rejection. The signal that
      //     "this is a fake/cancelled pick" is the lack of `path`.

      case "file/new":
        // Reset engine state to defaults, clear currentFilePath, clear
        // dirty. Emit dirty/changed (always — markClean dedupes on
        // already-clean, but file/new from a clean state may still
        // need to fire if anything else listens to "I just made a new
        // file"). For consistency: only emit if there was a change.
        useMockEngineState.getState().applyPatch({
          ...makeDefaultEngineState(),
        });
        this.markClean();
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        // Legacy parity: the default root (id 0) is selected after New. The
        // snapshot (above) carries it, but EmitterTree tracks selection via the
        // `emitters/selected` event (it doesn't re-read the snapshot post-mount),
        // so fire it explicitly — mirrors the native host's file/new.
        this.emit({ kind: "emitters/selected", payload: { id: 0 } });
        return {};

      case "file/pick-open":
        // Non-mutating picker: no native dialog in browser mode, so report no path
        // acquired (callers branch on `ok`). Never touches document state.
        return { ok: false, error: "browser-mode" };

      case "file/open": {
        // If the caller passed a path explicitly (e.g. Recent Files), use it.
        // Otherwise we simulate a cancelled native picker — the picker
        // doesn't exist in browser mode. The contract callers branch on
        // `ok` so this is the cleanest signal of "no path acquired".
        // `req.params.filter` ("alo" | "skydome" | "ground") is accepted
        // for type-compat but ignored here: there's no native dialog to
        // re-filter, and the browser-mode return value is the same
        // regardless of which surface invoked the picker.
        const explicit = req.params?.path;
        if (!explicit) {
          return { ok: false, error: "browser-mode" };
        }
        this.commitFilePath(explicit);
        this.markClean();
        return { ok: true, path: explicit };
      }

      case "file/save": {
        const explicit = req.params?.path;
        const cur = snapshotEngineState().currentFilePath;
        const target = explicit ?? cur ?? "/mock/untitled.alo";
        this.commitFilePath(target);
        this.markClean();
        return { ok: true, path: target };
      }

      case "file/save-as": {
        // Always "open the picker" — mock answers with a fixed path so
        // tests can assert deterministic behaviour. The native host
        // calls GetSaveFileNameW.
        const target = "/mock/saved-as.alo";
        this.commitFilePath(target);
        this.markClean();
        return { ok: true, path: target };
      }

      case "file/recent/list":
        return { paths: useMockRecentFiles.getState().paths };

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
      // tests-web can measure a POPULATED popover's geometry (#683).
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

      // ---------------- spawner ----------------
      //
      // The native host treats spawner/start as a full-config replace
      // (mirrors `SpawnerDriver::SetConfig`). The mock matches: every
      // incoming params overwrites the cached spawner block in
      // EngineStateDto, then emits engine/state/changed so any panel
      // subscribed to snapshots picks up the new config.
      //
      // spawner/trigger + spawner/stop are no-ops aside from the
      // active-count event. The mock doesn't simulate physics: trigger
      // bumps the count by burstSize, stop zeroes it. Real instance
      // tracking lives in the native SpawnerDriver.

      case "spawner/start":
        this.patchAndBroadcast({ spawner: { ...req.params } });
        return {};

      case "spawner/trigger": {
        const params = snapshotEngineState().spawner;
        const next = this.spawnerActiveCount + params.burstSize;
        this.spawnerActiveCount = next;
        this.emit({
          kind: "spawner/active-count",
          payload: { count: next },
        });
        return {};
      }

      case "spawner/stop":
        this.spawnerActiveCount = 0;
        this.emit({
          kind: "spawner/active-count",
          payload: { count: 0 },
        });
        return {};

      // ---------------- emitters/preview-from-file
      //
      // Returns a fixed 3-emitter mock tree regardless of path. Lets the
      // Import Emitters modal exercise the checkbox tree in browser
      // mode + Vitest. The native host forward-defers with a friendly
      // error (the legacy ImportEmitters_LoadFile path requires
      // FileManager + ParticleSystem which the new-UI host doesn't yet
      // own).
      case "emitters/preview-from-file":
        // stableId parity with the native preview tree (BuildEmitterTreeNode
        // emits it; synthetic root uses the reserved 0). The preview tree is
        // throwaway — fixed values are fine, they just must be present+unique.
        return {
          ok: true,
          tree: {
            id: 0,
            stableId: 0,
            name: "root",
            children: [
              { id: 1, stableId: 9001, name: "Smoke",  children: [
                { id: 4, stableId: 9004, name: "Smoke embers", children: [] },
              ] },
              { id: 2, stableId: 9002, name: "Sparks", children: [] },
              { id: 3, stableId: 9003, name: "Flash",  children: [] },
            ],
          },
        };

      // ---------------- emitters/get-tracks -------
      //
      // Read-only. Always returns 7 deterministic tracks per emitter
      // id from the fixture generator (see `makeFixtureTracks`). An
      // unknown id is not an error — the contract returns the same
      // 7-element shape with empty key arrays so the panel can render
      // a "no data" stub without special-casing failure.
      case "emitters/get-tracks": {
        const cur = useMockEmitterTree.getState().tree;
        const node = findEmitterNode(cur, req.params.id);
        if (node === null || node.id === -1) {
          // Empty tracks for missing / synthetic-root id. The overlay
          // is bypassed here intentionally — invalid ids must not be
          // observable through the overlay channel either.
          return {
            tracks: useMockTrackOverlay.getState().read(-1).map((t) => ({
              ...t,
              keys: [],
            })),
          };
        }
        // Read through the overlay so mutations made via
        // delete-track-keys / set-track-interpolation are reflected.
        // deriveLockViews applies the pointer-alias semantics at this
        // read boundary: locked channels present their master's current
        // canonical content rather than a stale copy taken at lock time.
        return { tracks: deriveLockViews(useMockTrackOverlay.getState().read(node.id)) };
      }

      // ---------------- emitters/get-properties ----
      //
      // Returns the merged fixture+overlay DTO for `id`. Unknown ids
      // (including the synthetic root id=-1) return default-shaped
      // properties so the React form can render a disabled placeholder
      // rather than special-casing the failure. The native host returns
      // ok:false on unknown id; the contract test asserts the success
      // path against a known id.
      case "emitters/get-properties": {
        const cur = useMockEmitterTree.getState().tree;
        const node = findEmitterNode(cur, req.params.id);
        if (node === null || node.id === -1) {
          return {
            properties: useMockEmitterProperties.getState().read(-1),
          };
        }
        return {
          properties: useMockEmitterProperties.getState().read(node.id),
        };
      }

      // ---------------- emitters/set-properties ----
      //
      // Batch patch: apply every key in `patch` to the overlay, emit
      // tree/changed + state/changed once so the React form re-fetches
      // and any downstream consumers (selection-aware components) see
      // the mutation. Missing ids are a silent no-op (the React side
      // disables the form when no emitter is selected).
      case "emitters/set-properties": {
        const cur = useMockEmitterTree.getState().tree;
        const node = findEmitterNode(cur, req.params.id);
        if (node === null || node.id === -1) {
          return {};
        }
        useMockEmitterProperties.getState().patch(node.id, req.params.patch);
        // If the patch includes `name`, mirror it onto the tree node so
        // the EmitterTree label updates without an extra `emitters/rename`
        // round-trip.
        if (typeof req.params.patch.name === "string") {
          useMockEmitterTree.getState().setTree(
            renameEmitter(cur, node.id, req.params.patch.name),
          );
        }
        this.emit({
          kind: "emitters/tree/changed",
          payload: useMockEmitterTree.getState().tree,
        });
        this.emit({
          kind: "engine/state/changed",
          payload: snapshotEngineState(),
        });
        return {};
      }

      // ---------------- emitters/delete-track-keys --
      //
      // Border keys (first + last in time order) are silently skipped.
      // The wire contract returns Record<string, never> on every call;
      // a request that targets only border keys is a successful no-op
      // from the React side's perspective (the C++ host is the source
      // of truth for what's a border key — React filters defensively).
      case "emitters/delete-track-keys": {
        const { id, track, times } = req.params;
        const removed = deleteTrackKeysInOverlay(id, track, times);
        if (removed > 0) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
        }
        return {};
      }

      // ---------------- emitters/set-track-interpolation
      //
      // Always succeeds (when the track is known); the mock surfaces a
      // missing-track as a silent no-op (matching the native host's
      // "track pointer null" path). Fires tree/changed so the panel
      // re-fetches and the toolbar's active-button visual updates.
      case "emitters/set-track-interpolation": {
        const { id, track, interpolation } = req.params;
        const ok = setTrackInterpolationInOverlay(id, track, interpolation);
        if (ok) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
        }
        return {};
      }

      // ---------------- emitters/set-track-lock ----------------------
      //
      // Per-channel track lock. Mirrors the native semantic at
      // [BridgeDispatcher.cpp emitters/set-track-lock] — only RGBA
      // participate, only earlier-channel targets are honoured, and
      // invalid combinations silently degrade to unlock.
      case "emitters/set-track-lock": {
        const { id, channel, lockTo } = req.params;
        const ok = setTrackLockInOverlay(id, channel, lockTo);
        if (ok) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
        }
        return {};
      }

      // ---------------- emitters/set-track-key --
      //
      // Drag-to-move commit. Erases the key at `oldTime` and inserts
      // `(newTime, newValue)` in time order. Border keys (first + last
      // in time order) silently override `newTime = oldTime` so only
      // the value moves — matches the drag-time-fixed rule + native
      // host semantics. Emits tree/changed + state/changed when the
      // mutation lands so the panel re-fetches.
      case "emitters/set-track-key": {
        const { id, track, oldTime, newTime, newValue } = req.params;
        const ok = setTrackKeyInOverlay(id, track, oldTime, newTime, newValue);
        if (ok) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
        }
        return {};
      }

      // ---------------- emitters/add-track-key --
      //
      // Click-to-add commit. Inserts a new key at `(time, value)` in
      // time order. If a key already exists at the exact `time`, the
      // helper bumps `time` by 0.001 until unique (matches the native
      // dedupe-by-epsilon rule). Returns the actual inserted (time,
      // value) so the React side can auto-select the new key.
      case "emitters/add-track-key": {
        const { id, track, time, value } = req.params;
        const result = addTrackKeyInOverlay(id, track, time, value);
        if (result !== null) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
          return result;
        }
        // Track lookup failed (unknown name). Return the request shape
        // so the React caller has a stable promise resolution; the
        // panel ignores the return value when no mutation landed.
        return { time, value };
      }

      // ---------------- emitters/add-track-keys --
      //
      // Multi-key paste. Loops the single insert so each key gets the
      // same dedupe-by-epsilon bump the native host applies, and returns
      // the ACTUAL inserted keys aligned to the input order. The undo
      // batching this exists for is native-only (the mock has no undo
      // stack), so what the mock pins is the wire shape: one request,
      // one tree/changed, N keys back.
      case "emitters/add-track-keys": {
        const { id, track, keys } = req.params;
        const inserted: { time: number; value: number }[] = [];
        for (const k of keys) {
          const r = addTrackKeyInOverlay(id, track, k.time, k.value);
          inserted.push(r ?? { time: k.time, value: k.value });
        }
        if (keys.length > 0) {
          this.emit({
            kind: "emitters/tree/changed",
            payload: useMockEmitterTree.getState().tree,
          });
          this.emit({
            kind: "engine/state/changed",
            payload: snapshotEngineState(),
          });
        }
        return { keys: inserted };
      }

      // ---------------- emitters/list + emitters/select
      //
      // The fixture tree lives in `mock-state.useMockEmitterTree`. The
      // list response returns a fresh copy so React-side consumers can't
      // mutate the store. `emitters/select` updates the snapshot's
      // selectedEmitterId scalar and emits both `emitters/selected` and
      // `engine/state/changed` so subscribers picking up either channel
      // see the change. Selection of an unknown id resets to null.
      case "emitters/list": {
        const cloned = JSON.parse(
          JSON.stringify(useMockEmitterTree.getState().tree),
        ) as EmitterTreeDto;
        // decorate the clone with live spawn values (see decorateSpawn).
        return { root: decorateSpawn(cloned.root) };
      }

      case "emitters/select": {
        const reqId = req.params.id;
        const tree = useMockEmitterTree.getState().tree;
        const valid = reqId !== null && findEmitterNode(tree, reqId) !== null
          ? reqId
          : null;
        useMockEngineState.getState().applyPatch({ selectedEmitterId: valid });
        this.emit({ kind: "emitters/selected", payload: { id: valid } });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      // ---------------- emitters/* mutations -----
      //
      // The fixture tree is mutated in place via the helpers in
      // mock-state. Each handler emits `emitters/tree/changed` so the
      // React EmitterTree re-fetches via `emitters/list`. Selection
      // bookkeeping mirrors the native host: deleting the selected
      // emitter clears the selection scalar.
      case "emitters/duplicate": {
        const cur = useMockEmitterTree.getState().tree;
        const result = duplicateEmitter(cur, req.params.id);
        if (result === null) {
          return { ok: false, error: "emitter not found" };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true, newId: result.newId };
      }

      case "emitters/duplicate-many": {
        // Loop the single duplicate. The mock appends each copy with a fresh
        // high id and does NOT reindex existing emitters, so the collected
        // newIds stay valid across the loop. (The real host inserts after the
        // source + reindexes; both honour the {newIds} contract.)
        let tree = useMockEmitterTree.getState().tree;
        const newIds: number[] = [];
        for (const id of req.params.ids) {
          const r = duplicateEmitter(tree, id);
          if (r !== null) {
            tree = r.tree;
            newIds.push(r.newId);
          }
        }
        if (newIds.length === 0) {
          return { ok: false, error: "no emitters to duplicate" };
        }
        useMockEmitterTree.getState().setTree(tree);
        this.emit({ kind: "emitters/tree/changed", payload: tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true, newIds };
      }

      case "emitters/delete": {
        const cur = useMockEmitterTree.getState().tree;
        const next = deleteEmitter(cur, req.params.id);
        if (next === null) {
          // Nothing to delete — still emit so subscribers know we tried.
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return {};
        }
        useMockEmitterTree.getState().setTree(next);
        // If the deleted id was selected, clear the selection.
        const snap = snapshotEngineState();
        if (snap.selectedEmitterId === req.params.id) {
          useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
          this.emit({ kind: "emitters/selected", payload: { id: null } });
        }
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      // ---------------- emitters/delete-many --
      //
      // The multi-root delete gesture. Loops the single delete and emits
      // ONE tree/changed for the whole batch. Unlike the native host the
      // mock stores ids ON the nodes and never reindexes, so the incoming
      // descending order is irrelevant here — it matters on the real host,
      // where an id is a position. The single undo entry this batching
      // exists for is likewise native-only; the mock pins the wire shape.
      case "emitters/delete-many": {
        let tree = useMockEmitterTree.getState().tree;
        const snap = snapshotEngineState();
        let removed = 0;
        let clearedSelection = false;
        for (const id of req.params.ids) {
          const next = deleteEmitter(tree, id);
          if (next === null) continue;
          tree = next;
          removed++;
          if (snap.selectedEmitterId === id) clearedSelection = true;
        }
        if (removed === 0) {
          // Nothing matched — still emit so subscribers know we tried,
          // matching the single-delete handler above.
          this.emit({ kind: "emitters/tree/changed", payload: tree });
          return {};
        }
        useMockEmitterTree.getState().setTree(tree);
        if (clearedSelection) {
          useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
          this.emit({ kind: "emitters/selected", payload: { id: null } });
        }
        this.emit({ kind: "emitters/tree/changed", payload: tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      case "emitters/rename": {
        const cur = useMockEmitterTree.getState().tree;
        const next = renameEmitter(cur, req.params.id, req.params.name);
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        return {};
      }

      case "emitters/duplicate-with-index-increment": {
        const cur = useMockEmitterTree.getState().tree;
        const result = duplicateWithIndexIncrement(cur, req.params.id, req.params.delta);
        if (result === null) {
          // Shape says { newId: number } unconditionally. We surface
          // the failure by returning newId=-1; native handler errors
          // via the wire's ok:false path (the schema variant is the
          // single-arm `{ newId }`). Tests that assert success branch
          // pre-stage a valid id.
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return { newId: -1 };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newId: result.newId };
      }

      case "emitters/duplicate-with-index-increment-many": {
        const cur = useMockEmitterTree.getState().tree;
        const result = duplicateWithIndexIncrementMany(
          cur, req.params.id, req.params.delta, req.params.count);
        if (result === null) {
          // Mirror the host's error path (SendErr): no copies made.
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return { newIds: [] };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newIds: result.newIds };
      }

      // ---------------- emitters/add-* / move / set-membership -
      //
      // Each mutates the fixture tree via mock-state helpers, then
      // emits `emitters/tree/changed` + `engine/state/changed`. Refusal
      // semantics mirror the host: add-child returns `{ newId: -1 }`
      // when the parent's slot is already filled or the id is missing;
      // move returns `{}` regardless (a refused move is a silent no-op
      // because the React side disables the menu item at the edges).
      case "emitters/add-lifetime-child": {
        const cur = useMockEmitterTree.getState().tree;
        const result = addLifetimeChildEmitter(cur, req.params.parentId);
        if (result === null) {
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return { newId: -1 };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newId: result.newId };
      }

      case "emitters/add-death-child": {
        const cur = useMockEmitterTree.getState().tree;
        const result = addDeathChildEmitter(cur, req.params.parentId);
        if (result === null) {
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return { newId: -1 };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newId: result.newId };
      }

      // New top-level "New Root Emitter"
      // menu item. Always succeeds at the mock level (the engine has
      // no max-roots cap). Tree-changed + state-changed events match
      // the other add-child handlers.
      case "emitters/add-root": {
        const cur = useMockEmitterTree.getState().tree;
        const result = addRootEmitterMock(cur);
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newId: result.newId };
      }

      case "emitters/move": {
        const cur = useMockEmitterTree.getState().tree;
        const next = moveEmitterInTree(cur, req.params.id, req.params.direction);
        if (next === null) {
          // Refused (non-root or at edge). Still emit so subscribers
          // that re-fetch defensively don't get stuck.
          this.emit({ kind: "emitters/tree/changed", payload: cur });
          return {};
        }
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      case "emitters/move-many": {
        // Move the selected ROOTS as a UNIT, preserving order: if the edge-most
        // root in the move direction is selected, the block is pinned → nothing
        // moves (no compacting past non-selected roots). Otherwise shift every
        // selected root by one (ascending for up / descending for down). The
        // mock keeps node ids stable through a move, so newIds are the selected
        // ids still at root level. (The real host reindexes; both honour {newIds}.)
        let tree = useMockEmitterTree.getState().tree;
        const dir = req.params.direction;
        const sel = new Set(req.params.ids);
        const order = tree.root.children.map((c) => c.id);
        const movable: number[] = [];
        const edgePinned =
          order.length > 0 && sel.has(dir === "up" ? order[0]! : order[order.length - 1]!);
        if (!edgePinned) {
          if (dir === "up") {
            for (let i = 0; i < order.length; i++) if (sel.has(order[i]!)) movable.push(order[i]!);
          } else {
            for (let i = order.length - 1; i >= 0; i--) if (sel.has(order[i]!)) movable.push(order[i]!);
          }
        }
        let moved = false;
        for (const id of movable) {
          const next = moveEmitterInTree(tree, id, dir);
          if (next !== null) {
            tree = next;
            moved = true;
          }
        }
        useMockEmitterTree.getState().setTree(tree);
        this.emit({ kind: "emitters/tree/changed", payload: tree });
        if (moved) this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        const finalRootIds = new Set(tree.root.children.map((c) => c.id));
        return { newIds: req.params.ids.filter((id) => finalRootIds.has(id)) };
      }

      case "emitters/reorder-many": {
        const cur = useMockEmitterTree.getState().tree;
        const next = reorderManyRoots(cur, req.params.ids, req.params.rootIndex);
        if (next === null) return { ok: false, error: "reorder refused" };
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        // Mock ids are stable across a reorder; newIds = the selected ids still
        // at root level, in input order (aligned for applyNewSelection).
        const rootIds = new Set(next.root.children.map((c) => c.id));
        return { ok: true, newIds: req.params.ids.filter((id) => rootIds.has(id)) };
      }

      case "emitters/set-visible": {
        const cur = useMockEmitterTree.getState().tree;
        const next = setEmitterVisibleMock(cur, req.params.id, req.params.visible);
        if (next === null) return {};
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      case "emitters/set-all-visible": {
        const cur = useMockEmitterTree.getState().tree;
        const next = setAllEmittersVisibleMock(cur, req.params.visible);
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      // ---------------- emitters/drop ------------
      //
      // Tagged-union: { mode: "reorder", id, rootIndex } reorders a
      // root via `reorderRootEmitter`; { mode: "reparent", id,
      // targetId, slot } moves the source under target in the named
      // slot via `reparentEmitterInTree`. Both helpers refuse cleanly
      // (return null) on cycle / slot-full / non-root / no-op — the
      // mock surfaces refusal as `{ ok: false, error: "..." }` to
      // match the native dispatcher's contract.
      case "emitters/drop": {
        const cur = useMockEmitterTree.getState().tree;
        // After a successful drop the highlight FOLLOWS the moved emitter
        // (re-select it + emit emitters/selected), mirroring the native host.
        // Mock ids are stable across a drop, so the moved id is req.params.id.
        const followSelection = () => {
          const id = req.params.id;
          useMockEngineState.getState().applyPatch({ selectedEmitterId: id });
          this.emit({ kind: "emitters/selected", payload: { id } });
        };
        if (req.params.mode === "reorder") {
          const next = reorderRootEmitter(cur, req.params.id, req.params.rootIndex);
          if (next === null) {
            return { ok: false, error: "reorder refused" };
          }
          useMockEmitterTree.getState().setTree(next);
          this.emit({ kind: "emitters/tree/changed", payload: next });
          followSelection();
          this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
          return { ok: true };
        }
        // mode === "reparent"
        const next = reparentEmitterInTree(
          cur,
          req.params.id,
          req.params.targetId,
          req.params.slot,
        );
        if (next === null) {
          return { ok: false, error: "reparent refused" };
        }
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        followSelection();
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true };
      }

      // ---------------- emitters/copy / cut / paste
      //
      // Process-local clipboard mirrors the native host's
      // `std::vector<std::vector<uint8_t>>`. `copy` snapshots subtrees
      // into the in-memory buffer; `cut` does the same then deletes
      // the originals in descending-id order (so prior indices stay
      // valid during the loop); `paste` deep-clones the buffer back
      // into the tree with fresh ids, splicing after `afterId` (when
      // present and matching a root) or appending at the end.
      case "emitters/copy": {
        const cur = useMockEmitterTree.getState().tree;
        const buf = copyEmittersToClipboard(cur, req.params.ids);
        useMockEmitterClipboard.getState().set(buf);
        return {};
      }

      case "emitters/cut": {
        const cur = useMockEmitterTree.getState().tree;
        const buf = copyEmittersToClipboard(cur, req.params.ids);
        useMockEmitterClipboard.getState().set(buf);
        // Delete in descending id order — keeps indices valid even if
        // a future implementation drops in-place id reuse. Single
        // tree-changed event at the end (atomic cut).
        let next: typeof cur = cur;
        const ids = [...req.params.ids].sort((a, b) => b - a);
        for (const id of ids) {
          const after = deleteEmitter(next, id);
          if (after !== null) next = after;
        }
        useMockEmitterTree.getState().setTree(next);
        // Clear selection if any cut id was selected.
        const snap = snapshotEngineState();
        if (snap.selectedEmitterId !== null && req.params.ids.includes(snap.selectedEmitterId)) {
          useMockEngineState.getState().applyPatch({ selectedEmitterId: null });
          this.emit({ kind: "emitters/selected", payload: { id: null } });
        }
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      case "emitters/paste": {
        const cur = useMockEmitterTree.getState().tree;
        const buf = useMockEmitterClipboard.getState().buffer;
        const afterId = req.params.afterId ?? null;
        const result = pasteEmittersFromClipboard(cur, buf, afterId);
        if (result.newIds.length === 0) {
          // Empty clipboard or nothing pasted; emit nothing so dirty
          // doesn't flip pointlessly. Still return the empty newIds.
          return { newIds: [] };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newIds: result.newIds };
      }

      case "emitters/paste-as-child": {
        const cur = useMockEmitterTree.getState().tree;
        const buf = useMockEmitterClipboard.getState().buffer;
        const result = pasteAsChildFromClipboard(
          cur,
          buf,
          req.params.parentId,
          req.params.slot,
        );
        if (result === null) {
          // Empty clipboard or occupied slot — emit nothing, no dirty flip.
          return { newId: -1 };
        }
        useMockEmitterTree.getState().setTree(result.tree);
        this.emit({ kind: "emitters/tree/changed", payload: result.tree });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { newId: result.newId };
      }

      case "linkGroups/set-membership": {
        const cur = useMockEmitterTree.getState().tree;
        const next = setLinkGroupMembership(
          cur,
          req.params.ids,
          req.params.groupId,
        );
        useMockEmitterTree.getState().setTree(next);
        this.emit({ kind: "emitters/tree/changed", payload: next });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return {};
      }

      // ---------------- engine/action/rescale-emitter --
      //
      // Per-emitter rescale. The mock has no engine state to mutate so
      // the handler is a logging stub; the dirty-bit ride-along via
      // isMutating still fires, matching the native host's contract.
      case "engine/action/rescale-emitter": {
        console.log(
          "[MockBridge] engine/action/rescale-emitter",
          req.params,
        );
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        // The rescale changes per-emitter scalars but not the tree's
        // structural shape; still emit tree/changed so future inspector
        // panels relying on it re-fetch.
        this.emit({
          kind: "emitters/tree/changed",
          payload: useMockEmitterTree.getState().tree,
        });
        return {};
      }

      // ---------------- linkGroups/* ------
      case "linkGroups/list-exempt-fields": {
        const fields = useMockLinkGroupExempt.getState().get(req.params.groupId);
        return { fields };
      }

      case "linkGroups/set-exempt-fields": {
        useMockLinkGroupExempt.getState().set(req.params.groupId, req.params.fields);
        this.emit({
          kind: "emitters/tree/changed",
          payload: useMockEmitterTree.getState().tree,
        });
        return {};
      }

      case "linkGroups/reset-exempt-fields": {
        useMockLinkGroupExempt.getState().reset(req.params.groupId);
        this.emit({
          kind: "emitters/tree/changed",
          payload: useMockEmitterTree.getState().tree,
        });
        return {};
      }

      // The native host diffs real emitter params; the mock has
      // none, so it echoes whatever conflicts the test seeded (default:
      // none). Read-only — no mutation, no events.
      case "linkGroups/diff-membership": {
        const groupId = req.params.groupId;
        // Leaving (0/null) never overwrites anything → no conflicts.
        if (groupId === null || groupId === 0) return { conflicts: [] };
        return { conflicts: useMockLinkGroupConflicts.getState().conflicts };
      }

      // Link settings surface: same stub — the real exempt→shared field diff
      // is a native concern; echo the seeded conflicts so a test/preview can
      // drive the inline settings warning.
      case "linkGroups/diff-exempt-change":
        return { conflicts: useMockLinkGroupConflicts.getState().conflicts };

      // ---------------- settings: cross-mode registry ----------------
      //
      // `settings/lighting` returns the raw lighting split (the native
      // host reads it from the registry; browser mode returns the
      // canonical defaults (matching the legacy Win32 dialog), with the live
      // in-memory `lightingForceAlign` flag). `…/set` writes just the
      // flag. No event is emitted — the constraint is enforced UI-side
      // in LightingPanel, and lighting isn't part of EngineStateDto.
      case "settings/lighting": {
        if (this.lightingOverride) {
          // Last written snapshot wins; forceAlign tracks the live flag so a
          // standalone `…/force-align/set` after a full write is still seen.
          return { ...this.lightingOverride, forceAlign: this.lightingForceAlign };
        }
        const rgb = (r: number, g: number, b: number) => r | (g << 8) | (b << 16);
        return {
          sun:   { intensity: 0.5, az: 0,   alt: 45,  diffuse: rgb(180, 180, 190), specular: rgb(190, 190, 200) },
          fill1: { intensity: 0.5, az: 120, alt: -10, diffuse: rgb(60, 80, 160),   specular: 0 },
          fill2: { intensity: 0.5, az: 210, alt: -10, diffuse: rgb(60, 80, 160),   specular: 0 },
          ambient: rgb(40, 40, 50),
          shadow:  rgb(100, 100, 110),
          forceAlign: this.lightingForceAlign,
        };
      }

      case "settings/lighting/set":
        this.lightingOverride = req.params;
        this.lightingForceAlign = req.params.forceAlign;
        return {};

      case "settings/lighting-force-align/set":
        this.lightingForceAlign = req.params.enabled;
        return {};

      // ---------------- emitters / undo: not yet implemented ----------------
      case "emitters/update":
      case "emitters/import-from-file":
      // Live-simulation counters read the real Engine's instance/emitter/
      // particle totals. Browser mode runs no simulation, so a plausible-looking
      // zero would be worse than a throw — it would let a test assert against a
      // number that means nothing (see contract-drift DEFERRED list).
      case "engine/query/live-instances":
      // Frameless title-bar controls act on the native HWND; browser mode has no
      // window to drive, so they fail loudly (see contract-drift DEFERRED list).
      case "window/minimize":
      case "window/maximize":
      case "window/close":
        throw new Error(`MockBridge: '${req.kind}' not implemented`);

      // Browser-mode undo is a no-op — the mock doesn't capture
      // snapshots of its multi-store state, so there's nothing to
      // restore. Native host (BridgeDispatcher.cpp) implements full
      // snap-restore via UndoStack + ParticleSystem write/read. Return
      // `{applied: false}` so accelerator + menu paths don't blow up
      // in browser mode while the native host owns the real undo
      // behaviour.
      case "undo/perform":
        return { applied: false };

      default: {
        // Exhaustiveness check — TS forces this to be `never`.
        const _exhaustive: never = req;
        throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
      }
    }
  }
}
