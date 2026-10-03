// Mock counterpart of src/host/BridgeDispatch_EmitterTracks.cpp.

import type {
  Request,
} from "@particle-editor/bridge-schema";
import {
  addTrackKeyInOverlay,
  deriveLockViews,
  deleteTrackKeysInOverlay,
  findEmitterNode,
  setTrackInterpolationInOverlay,
  setTrackLockInOverlay,
  setTrackKeyInOverlay,
  useMockEmitterTree,
  useMockTrackOverlay,
  snapshotEngineState,
} from "../mock-state";
import type { MockDispatchHost } from "./host";

type EmitterTracksKind =
  | "emitters/get-tracks"
  | "emitters/delete-track-keys"
  | "emitters/set-track-interpolation"
  | "emitters/set-track-lock"
  | "emitters/set-track-key"
  | "emitters/add-track-key"
  | "emitters/add-track-keys";

type EmitterTracksRequest = Extract<Request, { kind: EmitterTracksKind }>;

export function dispatchEmitterTracks(this: MockDispatchHost, req: EmitterTracksRequest): unknown {
  switch (req.kind) {

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

    default: {
      // Exhaustiveness check — TS forces this to be `never`.
      const _exhaustive: never = req;
      throw new Error(`MockBridge: unknown request kind: ${JSON.stringify(_exhaustive)}`);
    }
  }
}
