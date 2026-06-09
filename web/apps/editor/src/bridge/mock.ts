// MockBridge — a fully-in-process Bridge implementation backed by a
// Zustand store (`mock-state.ts`). Used when the React app runs outside
// the WebView2 host (browser-mode design iteration, Vitest contract
// tests).
//
// Coverage as of Task 2.1:
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
// a "not implemented" error — those land in Phase 3+.

import type {
  Bridge,
  Request,
  ResponseFor,
  Event,
  EventKind,
  EventOf,
  EngineStateDto,
  LightDto,
} from "@particle-editor/bridge-schema";
import {
  addDeathChildEmitter,
  addLifetimeChildEmitter,
  addRootEmitterMock,
  addTrackKeyInOverlay,
  copyEmittersToClipboard,
  deleteEmitter,
  deleteTrackKeysInOverlay,
  duplicateEmitter,
  duplicateWithIndexIncrement,
  findEmitterNode,
  makeDefaultEngineState,
  moveEmitterInTree,
  pasteEmittersFromClipboard,
  pasteAsChildFromClipboard,
  renameEmitter,
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
  //: engine/set/paused (view-only preview clock toggle) and
  // engine/set/heat-debug (view-only debug overlay) are excluded —
  // both leave the document state untouched and shouldn't trigger
  // save-prompt gates. Native host applies the same rule in
  // BridgeDispatcher.cpp.
  if (kind === "engine/set/paused") return false;
  if (kind === "engine/set/heat-debug") return false;
  // T9] stats/set-frozen is a test-only knob; never mutating.
  if (kind === "stats/set-frozen") return false;
  if (kind.startsWith("engine/set/")) return true;
  // engine/action/clear is destructive — destroying particles in the
  // world is a user-visible mutation worth a save-prompt gate.
  if (kind === "engine/action/clear") return true;
  // engine/action/rescale-system mutates emitter parameters.
  if (kind === "engine/action/rescale-system") return true;
  // Screen 4 Batch B1: per-emitter rescale + structural mutations are
  // all mutating. Link-group exempt-set edits change propagation
  // behaviour but not engine-observable particle output; flag them
  // anyway so the dirty-bit + save-prompt gate matches the native
  // host's `markDirty` rule.
  if (kind === "engine/action/rescale-emitter") return true;
  if (kind === "emitters/duplicate") return true;
  if (kind === "emitters/duplicate-many") return true;
  if (kind === "emitters/delete") return true;
  if (kind === "emitters/rename") return true;
  if (kind === "emitters/duplicate-with-index-increment") return true;
  // Screen 4 Batch B2 — add-child / move / link-group-membership all
  // change persisted tree state, so they ride the dirty bit.
  if (kind === "emitters/add-lifetime-child") return true;
  if (kind === "emitters/add-death-child") return true;
  if (kind === "emitters/add-root") return true;
  if (kind === "emitters/move") return true;
  if (kind === "emitters/move-many") return true;
  if (kind === "linkGroups/set-membership") return true;
  // Screen 4 Batch B3 — drag/drop reorder + reparent. Both modes
  // mutate persisted tree state.
  if (kind === "emitters/drop") return true;
  // Screen 4 Batch C — clipboard. `copy` doesn't mutate the tree;
  // `cut` (delete) + `paste` (insert) both do. Matches the native
  // host's per-handler `SetDirty` rule.
  if (kind === "emitters/cut") return true;
  if (kind === "emitters/paste") return true;
  if (kind === "emitters/paste-as-child") return true;
  if (kind === "linkGroups/set-exempt-fields") return true;
  if (kind === "linkGroups/reset-exempt-fields") return true;
  // Screen 5 / Screen 6 Batch B-α — track key deletion + interpolation
  // toggle are persisted mutations on the per-emitter Track state.
  if (kind === "emitters/delete-track-keys") return true;
  if (kind === "emitters/set-track-interpolation") return true;
  if (kind === "emitters/set-track-lock") return true;
  // Screen 6 Batch B-β — drag-to-move + click-to-add land in the same
  // mutating tier as delete + interpolation: both edit per-emitter
  // Track state.
  if (kind === "emitters/set-track-key") return true;
  if (kind === "emitters/add-track-key") return true;
  // Phase 4.1 Fix dispatch 1 — per-emitter property patch.
  if (kind === "emitters/set-properties") return true;
  return false;
}

export class MockBridge implements Bridge {
  private listeners = new Map<EventKind, Set<(e: Event) => void>>();

  /** Screen 8 Batch 4: in-mock "active spawner instance count". Bumped
   *  by spawner/trigger (by burstSize), zeroed by spawner/stop. The
   *  native SpawnerDriver tracks real ParticleSystemInstance lifecycles;
   *  the mock counter is just a hook for UI badge testing. */
  private spawnerActiveCount = 0;

  /** Cross-mode Force Align flag. The native host persists this as the
   *  REG_DWORD `LightingForceFillAlignment`; browser mode keeps it in
   *  memory, defaulting to the legacy `kLightForceAlignDefault = true`.
   *  Read by `settings/lighting-force-align`, written by `…/set`. */
  private lightingForceAlign = true;

  async request<R extends Request>(req: R): Promise<ResponseFor<R>> {
    const result = this.handle(req);
    // After the handler completes, mark dirty for any engine mutation.
    // (file/* and engine/action/reload-* / clear are deliberately NOT
    // marked dirty — see isMutating below.)
    if (isMutating(req.kind)) {
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
    const bucket = this.listeners.get(e.kind);
    bucket?.forEach((h) => h(e));
  }

  /** Patch the store and broadcast engine/state/changed with the full snapshot. */
  private patchAndBroadcast(patch: Partial<EngineStateDto>): void {
    useMockEngineState.getState().applyPatch(patch);
    this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
  }

  /** Screen 8 Batch 3: every mutating setter/action sets dirty=true. The
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

      case "engine/set/ground-texture":
        this.patchAndBroadcast({ groundTexture: req.params.slot });
        return {};

      case "engine/set/ground-solid-color":
        this.patchAndBroadcast({ groundSolidColor: req.params.rgb });
        return {};

      case "engine/set/ground-slot-custom-path": {
        const { slot, path } = req.params;
        const paths = [...snapshotEngineState().groundSlotCustomPaths];
        if (slot >= 0 && slot < paths.length) paths[slot] = path;
        this.patchAndBroadcast({ groundSlotCustomPaths: paths });
        return {};
      }

      // ---------------- engine setters: skydome / background ----------------
      case "engine/set/skydome-slot":
        this.patchAndBroadcast({ skydomeSlot: req.params.slot });
        return {};

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

      // Task 2.7 — leave particles after instance death. Mirrors the
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

      // ---------------- stats freeze (test-only knob) ----------------
      // T9] Mock parity for native stats/set-frozen. Browser
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

      // Group D: cascade-reset background, ground, bloom,
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
        // Mirrors Engine::IsGroundSlotEmpty: slots 0..(kGroundTextureBundledCount-1)
        // have bundled defaults (except kGroundSolidColorSlot=4 which is the
        // procedural solid colour and is never empty either). Slots >=5 are
        // empty iff their custom path is empty.
        const hasBuiltin = slot >= 0 && slot < 5;  // 0..4 are bundled / procedural
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

      case "engine/query/bloom-available":
        return snapshotEngineState().bloomAvailable;

      // ---------------- mods (D6) -------------------------------
      //
      // Browser-mode MockBridge has no disk to scan, so `mods/list` /
      // `mods/refresh` return a small synthetic fixture (one FoC + one
      // Base Game entry) sufficient for React component tests and
      // design iteration. `mods/select` mutates activeModPath on the
      // store and fires engine/state/changed so subscribed components
      // see the new active-path.
      case "mods/list":
      case "mods/refresh": {
        const fixture = [
          { path: "C:/mock/corruption/Mods/FoCMod",        folderName: "FoCMod",        nickname: "",         isFoC: true  },
          { path: "C:/mock/GameData/Mods/BaseGameMod",     folderName: "BaseGameMod",   nickname: "Demo Mod", isFoC: false },
        ];
        return { mods: fixture, activePath: snapshotEngineState().activeModPath };
      }

      case "mods/select": {
        const params = req.params as { path: string | null };
        useMockEngineState.getState().applyPatch({ activeModPath: params.path });
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true, activePath: params.path } as { ok: true; activePath: string | null };
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

      // ---------------- autosave crash-recovery () ----------------
      case "autosave/check-recovery":
        // Mock: no %TEMP% scan in browser mode — always "no orphan", so the
        // recovery dialog never appears under `pnpm dev` / web vitest.
        // Component tests that exercise the dialog inject an orphan directly
        // (they render AutosaveRecoveryDialog, not the whole shell).
        return { orphan: null };

      case "autosave/recover":
        // Mock: no document to swap. Accept silently.
        return {};

      case "layout/viewport-rect":
        // Mock: no native HWND to reposition.
        return {};

      case "layout/scene-rect":
        // Mock: no native AlphaCompositor to mask.
        return {};

      case "animate-scene-rect":
        // Mock: no native viewport-anim system (the host interpolates the
        // dock-slide rect under). Accept silently.
        return {};

      case "host/backing-color":
        // Mock: no native DComp backing visual to recolour.
        return {};

      case "viewport/occlude":
        // Mock: no native HWND to clip. Acknowledge silently.
        return {};

      case "viewport/capture-snapshot":
        // Mock: no engine to snapshot. Empty image + zero dims so the
        // React Modal's render guard (`snapshot && snapshot.imageBase64`)
        // short-circuits the <img> portal in unit tests.
        return { imageBase64: "", w: 0, h: 0 };

      case "viewport/input":
        // Phase 2 — mock: no native HWND to PostMessage to.
        // Tests assert on `dispatch` call args (kind + payload shape);
        // the return shape is the standard empty-object ack. Browser
        // mode never has an engine to drive, so this is a pure no-op.
        return {};

      // ---------------- file ops (Phase 3 Screen 8 Batch 3) ----------
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
        return {};

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

      // ---------------- texture palette (sub-feature B) ----------------
      //
      // Browser mode has no per-mod Store and no texture decode, so the
      // palette is inert: empty pins/recents, null thumbnails, no-op
      // mutations. The TexturePickerField stays fully usable via Browse
      // and manual entry. The native host backs these with
      // TexturePalette::Store + EncodeThumbnailPng.
      case "textures/palette/list":
        return { hasMod: false, filter: req.params.slot, pins: [], recents: [] };

      case "textures/palette/thumbnail":
        return { dataUri: null, status: "missing" as const };

      case "textures/palette/toggle-pin":
        return { ok: true, pinned: false };

      case "textures/palette/touch-recent":
        return { ok: true };

      // ---------------- spawner (Phase 3 Screen 8 Batch 4) ----------------
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

      // ---------------- emitters/preview-from-file (Phase 3 Screen 8 Batch 4)
      //
      // Returns a fixed 3-emitter mock tree regardless of path. Lets the
      // Import Emitters modal exercise the checkbox tree in browser
      // mode + Vitest. The native host forward-defers with a friendly
      // error (the legacy ImportEmitters_LoadFile path requires
      // FileManager + ParticleSystem which the new-UI host doesn't yet
      // own).
      case "emitters/preview-from-file":
        return {
          ok: true,
          tree: {
            id: 0,
            name: "root",
            children: [
              { id: 1, name: "Smoke",  children: [
                { id: 4, name: "Smoke embers", children: [] },
              ] },
              { id: 2, name: "Sparks", children: [] },
              { id: 3, name: "Flash",  children: [] },
            ],
          },
        };

      // ---------------- emitters/get-tracks (Screen 6 Batch A) -------
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
        return { tracks: useMockTrackOverlay.getState().read(node.id) };
      }

      // ---------------- emitters/get-properties (Phase 4.1 Fix 1) ----
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

      // ---------------- emitters/set-properties (Phase 4.1 Fix 1) ----
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

      // ---------------- emitters/delete-track-keys (Screen 5/6 B-α) --
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

      // ---------------- emitters/set-track-interpolation (Screen 5/6 B-α)
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

      // ---------------- emitters/set-track-key (Screen 6 Batch B-β) --
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

      // ---------------- emitters/add-track-key (Screen 6 Batch B-β) --
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

      // ---------------- emitters/list + emitters/select (Screen 4 Batch A)
      //
      // The fixture tree lives in `mock-state.useMockEmitterTree`. The
      // list response returns a fresh copy so React-side consumers can't
      // mutate the store. `emitters/select` updates the snapshot's
      // selectedEmitterId scalar and emits both `emitters/selected` and
      // `engine/state/changed` so subscribers picking up either channel
      // see the change. Selection of an unknown id resets to null.
      case "emitters/list":
        return JSON.parse(JSON.stringify(useMockEmitterTree.getState().tree));

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

      // ---------------- emitters/* mutations (Screen 4 Batch B1) -----
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

      // ---------------- emitters/add-* / move / set-membership (B2) -
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

      // Phase 4.1 Fix dispatch 5 — new top-level "New Root Emitter"
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

      // ---------------- emitters/drop (Screen 4 Batch B3) ------------
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
        if (req.params.mode === "reorder") {
          const next = reorderRootEmitter(cur, req.params.id, req.params.rootIndex);
          if (next === null) {
            return { ok: false, error: "reorder refused" };
          }
          useMockEmitterTree.getState().setTree(next);
          this.emit({ kind: "emitters/tree/changed", payload: next });
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
        this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
        return { ok: true };
      }

      // ---------------- emitters/copy / cut / paste (Screen 4 Batch C)
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

      // ---------------- engine/action/rescale-emitter (Screen 4 B1) --
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

      // ---------------- linkGroups/* (Screen 4 Batch B1,) ------
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

      //: the native host diffs real emitter params; the mock has
      // none, so it echoes whatever conflicts the test seeded (default:
      // none). Read-only — no mutation, no events.
      case "linkGroups/diff-membership": {
        const groupId = req.params.groupId;
        // Leaving (0/null) never overwrites anything → no conflicts.
        if (groupId === null || groupId === 0) return { conflicts: [] };
        return { conflicts: useMockLinkGroupConflicts.getState().conflicts };
      }

      // LNK settings surface: same stub — the real exempt→shared field diff
      // is a native concern; echo the seeded conflicts so a test/preview can
      // drive the inline settings warning.
      case "linkGroups/diff-exempt-change":
        return { conflicts: useMockLinkGroupConflicts.getState().conflicts };

      // ---------------- settings: cross-mode registry ----------------
      //
      // `settings/lighting` returns the raw lighting split (the native
      // host reads it from the registry; browser mode returns the
      // canonical defaults from src/main.cpp:6180-6195, with the live
      // in-memory `lightingForceAlign` flag). `…/set` writes just the
      // flag. No event is emitted — the constraint is enforced UI-side
      // in LightingPanel, and lighting isn't part of EngineStateDto.
      case "settings/lighting": {
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

      case "settings/lighting-force-align/set":
        this.lightingForceAlign = req.params.enabled;
        return {};

      // ---------------- emitters / undo: Phase 3+ ----------------
      case "emitters/update":
      case "emitters/import-from-file":
        throw new Error(`MockBridge: '${req.kind}' not implemented (Phase 3+)`);

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
