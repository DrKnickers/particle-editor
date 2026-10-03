// Contents (search for the quoted text):
//   "Mutation rules" - Persisted edits and refusal checks.
//   "Bridge state and requests" - Instance state and mutation wrapper.
//   "Event helpers" - Event decoration and state notifications.
//   "Request routing" - Handler groups and deferred requests.
//
// MockBridge — a fully-in-process Bridge implementation backed by a
// Zustand store (`mock-state.ts`). Used when the React app runs outside
// the WebView2 host (browser-mode design iteration, Vitest contract
// tests).
//
// The switch here routes requests to mock-dispatch/ groups matching the
// ten native BridgeDispatch_*.cpp files. This class keeps the mutation
// wrapper, shared instance state and event decoration; deferred requests
// and browser-mode undo stay inline.
//
// Coverage:
//   - engine/state/snapshot                  full DTO
//   - engine/set/*                           mutates the store, then
//                                            emits engine/state/changed
//   - engine/action/*                        mutates where appropriate,
//                                            emits engine/state/changed
//   - engine/query/*                         read-only (live-instances throws)
//   - register-accelerators                  accepted as a no-op
//   - layout/viewport-rect                   accepted as a no-op
//   - layout/scene-rect                      accepted as a no-op
//   - animate-scene-rect                     accepted as a no-op
//   - host/backing-color                     accepted as a no-op
// The remaining schema requests are implemented, explicitly rejected where
// browser mode cannot model them, or accepted as intentional no-ops.

import type {
  Bridge,
  Request,
  ResponseFor,
  Event,
  EventKind,
  EventOf,
  EngineStateDto,
  EmitterTreeDto,
  LightingSettingsDto,
} from "@particle-editor/bridge-schema";
import {
  findEmitterNode,
  useMockEmitterTree,
  useMockEngineState,
  useMockRecentFiles,
  snapshotEngineState,
} from "./mock-state";
import type { MockDispatchHost } from "./mock-dispatch/host";
import { dispatchEngine } from "./mock-dispatch/dispatch-engine";
import { decorateSpawn, dispatchEmitters } from "./mock-dispatch/dispatch-emitters";
import { dispatchEmitterProperties } from "./mock-dispatch/dispatch-emitter-properties";
import { dispatchEmitterTracks } from "./mock-dispatch/dispatch-emitter-tracks";
import { dispatchLinkGroups } from "./mock-dispatch/dispatch-link-groups";
import { dispatchEmitterClipboard } from "./mock-dispatch/dispatch-emitter-clipboard";
import { dispatchFile } from "./mock-dispatch/dispatch-file";
export { seedMockPalette } from "./mock-dispatch/dispatch-assets";
import { dispatchAssets } from "./mock-dispatch/dispatch-assets";
import { dispatchShell } from "./mock-dispatch/dispatch-shell";
import { dispatchSpawnerLighting } from "./mock-dispatch/dispatch-spawner-lighting";
import { EventHub } from "./event-hub";

/** Returns true for request kinds that should mark the in-memory file
 *  state dirty. Every engine/set/* is mutating. Engine actions are
 *  mutating except for the read-only-ish reload-shaders / reload-textures
 *  / on-particle-system-changed / step-frames, which don't change
 *  user-visible parameters. file/*, query/*, undo/perform, spawner/*,
 *  layout, accelerators are not. The native host applies the same rule
 *  via per-handler `SetDirty(true)` calls. */
// ─── Mutation rules ─────────────────────────────
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
  // not part of the .alo document); native host no longer marks dirty.
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
 *  - emitters/paste → `newIds: []` when the clipboard is empty.
 *  - emitters/paste-as-child, add-lifetime-child, add-death-child → `newId: -1`
 *    when refused (occupied slot, empty clipboard, unknown parent).
 *  - emitters/delete, delete-many → an unknown id deletes nothing; checked
 *    against the pre-call tree, since the response is `{}` either way.
 */
function didMutate(
  req: Request,
  result: unknown,
  preMoveRootOrder: number[] | null,
  preEngineSlot: number | null,
  preTree: EmitterTreeDto,
): boolean {
  switch (req.kind) {
    case "emitters/paste":
      return (result as { newIds: number[] }).newIds.length > 0;
    case "emitters/paste-as-child":
    case "emitters/add-lifetime-child":
    case "emitters/add-death-child":
      return (result as { newId: number }).newId !== -1;
    case "emitters/delete":
    case "emitters/delete-many": {
      const ids = req.kind === "emitters/delete" ? [req.params.id] : req.params.ids;
      // id -1 is the synthetic root, which findEmitterNode would match.
      return ids.some((id) => id !== -1 && findEmitterNode(preTree, id) !== null);
    }
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

// ─── Bridge state and requests ─────────────────────────────
export class MockBridge implements Bridge, MockDispatchHost {
  private events = new EventHub<{ [K in EventKind]: EventOf<K> }>("MockBridge");
  private viewportUnavailable: EventOf<"viewport/unavailable"> | null = null;

  /** Browser dev: window.bridge.setViewportUnavailable("Graphics frame attach failed.").
   *  Retained for late subscriptions, just like the native host-session failure. */
  setViewportUnavailable(reason: string): void {
    this.viewportUnavailable = { kind: "viewport/unavailable", payload: { reason } };
    this.emit(this.viewportUnavailable);
  }

  /** In-mock "active spawner instance count". Bumped
   *  by Manual spawner/trigger (by burstSize), zeroed by spawner/stop. The
   *  native SpawnerDriver tracks real ParticleSystemInstance lifecycles;
   *  the mock counter is just a hook for UI badge testing. */
  spawnerActiveCount = 0;

  /** Cross-mode Force Align flag. The native host persists this as the
   *  REG_DWORD `LightingForceFillAlignment`; browser mode keeps it in
   *  memory, defaulting to true (the legacy Win32 UI's force-align default).
   *  Read by `settings/lighting-force-align`, written by `…/set`. */
  lightingForceAlign = true;

  /** Cross-mode raw lighting split. The native host persists this in the
   *  registry; browser mode keeps the last `settings/lighting/set` payload
   *  in memory so the panel's edits + Reset round-trip within a session.
   *  `null` until first written → `settings/lighting` returns the canonical
   *  defaults (with the live `lightingForceAlign` flag). */
  lightingOverride: LightingSettingsDto | null = null;

  // [guard-config] Last engine/set/overload-guard params received —
  // test-observable; no mock behavior depends on it.
  lastOverloadGuard: { enabled: boolean; maxParticles: number } | null = null;

  // Ordered content-layer stack (absolute slash-free paths, front = highest
  // precedence; [] = Unmodded). Not part of the engine-state snapshot DTO — surfaced
  // only via the mods/list payload + driven by mods/set-layers.
  layerStack: string[] = [];

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
    const preTree = useMockEmitterTree.getState().tree;
    const result = this.handle(req);
    // After the handler completes, mark dirty for any engine mutation —
    // but only on a REAL mutation. A refused drag-commit (ok:false) or a
    // no-op batch move leaves the document clean, mirroring the native host
    // (which marks dirty only on the success branch of each handler). The
    // mock previously fired this unconditionally for any mutating kind, so a
    // refused/no-op drag-commit falsely dirtied the doc.
    // (file/* and engine/action/reload-* / clear are deliberately NOT
    // marked dirty — see isMutating below.)
    if (isMutating(req.kind) && didMutate(req, result, preMoveRootOrder, preEngineSlot, preTree)) {
      this.markDirty();
    }
    return result as ResponseFor<R>;
  }

  on<K extends EventKind>(kind: K, handler: (e: EventOf<K>) => void): () => void {
    const unsubscribe = this.events.on(kind, handler);
    if (kind === "viewport/unavailable" && this.viewportUnavailable)
      handler(this.viewportUnavailable as EventOf<K>);
    return unsubscribe;
  }

  // ─── Event helpers ─────────────────────────────
  // ---------------------------------------------------------------- internals

  emit(e: Event): void {
    // decorate tree payloads with live spawn values at the single
    // event choke point (see decorateSpawn above).
    if (e.kind === "emitters/tree/changed") {
      e = { ...e, payload: { ...e.payload, root: decorateSpawn(e.payload.root) } };
    }
    this.events.emit(e.kind, e);
  }

  /** Patch the store and broadcast engine/state/changed with the full snapshot. */
  patchAndBroadcast(patch: Partial<EngineStateDto>): void {
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
  markClean(): void {
    const cur = snapshotEngineState();
    if (!cur.dirty) return;
    useMockEngineState.getState().applyPatch({ dirty: false });
    this.emit({ kind: "dirty/changed", payload: { dirty: false } });
  }

  /** Update currentFilePath, push to recents (dedup, cap 9), emit
   *  recent/changed + engine/state/changed. Used by file/open and
   *  file/save success paths. */
  commitFilePath(path: string): void {
    useMockEngineState.getState().applyPatch({ currentFilePath: path });
    const recents = useMockRecentFiles.getState().push(path);
    this.emit({ kind: "recent/changed", payload: { paths: recents } });
    this.emit({ kind: "engine/state/changed", payload: snapshotEngineState() });
  }

  // ─── Request routing ─────────────────────────────
  private handle(req: Request): unknown {
    switch (req.kind) {
      case "engine/state/snapshot":
      case "engine/set/ground":
      case "engine/set/ground-z":
      case "engine/set/ground-texture":
      case "engine/set/ground-solid-color":
      case "engine/set/ground-slot-custom-path":
      case "engine/set/skydome-slot":
      case "engine/set/skydome-custom-path":
      case "engine/set/skydome-environment":
      case "engine/set/reference-object":
      case "engine/set/reference-object-visible":
      case "engine/set/reference-object-lock":
      case "engine/set/reference-object-transform":
      case "engine/set/grid-visible":
      case "engine/set/grid-spacing":
      case "engine/set/snap-enabled":
      case "engine/set/background":
      case "engine/set/bloom":
      case "engine/set/bloom-strength":
      case "engine/set/bloom-cutoff":
      case "engine/set/bloom-size":
      case "engine/set/leave-particles":
      case "engine/set/heat-debug":
      case "engine/set/camera":
      case "engine/set/light":
      case "engine/set/ambient":
      case "engine/set/shadow":
      case "engine/set/paused":
      case "engine/set/overload-guard":
      case "engine/set/msaa-level":
      case "engine/set/model-shadows":
      case "engine/set/soft-shadows":
      case "engine/set/estimated-load":
      case "engine/action/clear":
      case "engine/action/reload-shaders":
      case "engine/action/reload-textures":
      case "engine/action/on-particle-system-changed":
      case "engine/action/step-frames":
      case "engine/action/reset-view-settings":
      case "engine/action/rescale-system":
      case "engine/query/ground-slot-empty":
      case "engine/query/skydome-slot-empty":
      case "engine/query/skydome-list":
      case "engine/query/reference-object-list":
      case "engine/query/bloom-available":
      case "engine/query/msaa-levels":
      case "engine/action/rescale-emitter":
        return dispatchEngine.call(this, req);

      case "emitters/preview-from-file":
      case "emitters/list":
      case "emitters/select":
      case "emitters/duplicate":
      case "emitters/duplicate-many":
      case "emitters/delete":
      case "emitters/delete-many":
      case "emitters/rename":
      case "emitters/duplicate-with-index-increment":
      case "emitters/duplicate-with-index-increment-many":
      case "emitters/add-lifetime-child":
      case "emitters/add-death-child":
      case "emitters/add-root":
      case "emitters/move":
      case "emitters/move-many":
      case "emitters/reorder-many":
      case "emitters/set-visible":
      case "emitters/set-all-visible":
      case "emitters/drop":
        return dispatchEmitters.call(this, req);

      case "emitters/get-properties":
      case "emitters/set-properties":
        return dispatchEmitterProperties.call(this, req);

      case "emitters/get-tracks":
      case "emitters/delete-track-keys":
      case "emitters/set-track-interpolation":
      case "emitters/set-track-lock":
      case "emitters/set-track-key":
      case "emitters/add-track-key":
      case "emitters/add-track-keys":
        return dispatchEmitterTracks.call(this, req);

      case "linkGroups/set-membership":
      case "linkGroups/list-exempt-fields":
      case "linkGroups/set-exempt-fields":
      case "linkGroups/reset-exempt-fields":
      case "linkGroups/diff-membership":
      case "linkGroups/diff-exempt-change":
        return dispatchLinkGroups.call(this, req);

      case "emitters/copy":
      case "emitters/cut":
      case "emitters/paste":
      case "emitters/paste-as-child":
        return dispatchEmitterClipboard.call(this, req);

      case "autosave/check-recovery":
      case "autosave/recover":
      case "file/new":
      case "file/pick-open":
      case "file/open":
      case "file/save":
      case "file/save-as":
      case "file/recent/list":
        return dispatchFile.call(this, req);

      case "mods/list":
      case "mods/refresh":
      case "mods/set-layers":
      case "textures/browse":
      case "textures/palette/list":
      case "textures/palette/thumbnail":
      case "textures/get-preview":
      case "textures/palette/toggle-pin":
      case "textures/palette/touch-recent":
        return dispatchAssets.call(this, req);

      case "stats/set-frozen":
      case "register-accelerators":
      case "app/open-external":
      case "app/quit":
      case "layout/viewport-rect":
      case "layout/scene-rect":
      case "animate-scene-rect":
      case "host/backing-color":
      case "viewport/capture-snapshot":
      case "viewport/input":
        return dispatchShell.call(this, req);

      case "spawner/start":
      case "spawner/trigger":
      case "spawner/stop":
      case "settings/lighting":
      case "settings/lighting/set":
      case "settings/lighting-force-align/set":
        return dispatchSpawnerLighting.call(this, req);

      // ---------------- emitters / undo: not yet implemented ----------------
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
