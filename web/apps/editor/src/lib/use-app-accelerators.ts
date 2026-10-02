// use-app-accelerators.ts — wires the legacy global keyboard accelerators
// (ParticleEditor.en.rc:508-530) to the new UI's existing actions.
//
// The host (AcceleratorBridge) translates registered combos and emits
// `accelerator/pressed`; this hook dispatches each to the same bridge call
// the corresponding menu item uses, so the menu and the shortcut stay in
// lock-step. Toggles (Ground / Pause / Heat Debug) read the live engine
// state so the shortcut flips the current value exactly like the checkbox
// menu item.
//
// Deliberately NOT registered globally:
//   - `Delete` / `F2` — these would fire while the user is typing in a
//     spinner/name field (deleting an emitter or starting a rename
//     mid-edit). The EmitterTree handles them when it has focus, which is
//     the safe scope.
//
// Flagged GAPS (no underlying action yet, so not wired): none. Every legacy
// accelerator target below resolves to an existing bridge command or UI action.

import { useEffect, useRef } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { replaceDocument, runFileOp } from "@/lib/file-op";
import { toggleDock } from "@/lib/right-dock";
import { useEmitterSelectionStore } from "@/lib/tree/emitter-selection";
import { moveEmitters } from "@/lib/tree/emitter-reorder";
import { RESET_CAMERA } from "@/lib/reset-camera";
import { bumpTextureEpoch } from "@/lib/atlas/atlas-preview-cache";
import { fireAndReport } from "@/lib/status-feedback";

const ACCEL_COMBOS = [
  "Ctrl+N",
  "Ctrl+O",
  "Ctrl+S",
  "Ctrl+Del",
  "Ctrl+G",
  "Ctrl+H",
  "Ctrl+L",
  "Ctrl+Home",
  "F5",
  "F6",
  "F7",
  "F8",
  "F9",
  "F10",
  "Ctrl+Z",
  "Ctrl+Y",
  "Ctrl+Shift+Z",
  "Ctrl+Space",
  "Alt+Up",
  "Alt+Down",
];

export function useAppAccelerators(bridge: Bridge): void {
  // Live engine state, read by the toggle shortcuts. Held in a ref so the
  // accelerator handler always sees the latest value without re-binding.
  const stateRef = useRef<EngineStateDto | null>(null);
  useEffect(() => {
    let cancelled = false;
    // Set by the first engine/state/changed: a broadcast is newer than the
    // in-flight mount snapshot, so a late snapshot must not overwrite it (the
    // same guard useEngineField's shared store applies).
    let changedSeen = false;
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled && !changedSeen) stateRef.current = s;
      })
      .catch(() => {});
    const off = bridge.on("engine/state/changed", (e) => {
      changedSeen = true;
      stateRef.current = e.payload;
    });
    return () => {
      cancelled = true;
      off();
    };
  }, [bridge]);

  useEffect(() => {
    bridge
      .request({ kind: "register-accelerators", params: { combos: ACCEL_COMBOS } })
      .catch((err) => console.warn("[accel] register-accelerators failed:", err));

    const off = bridge.on("accelerator/pressed", (e) => {
      const combo = e.payload.combo;
      const st = stateRef.current;
      switch (combo) {
        // ── File ──
        case "Ctrl+N":
          replaceDocument(bridge, { kind: "file/new", params: {} });
          break;
        case "Ctrl+O":
          replaceDocument(bridge, { kind: "file/open", params: {} });
          break;
        case "Ctrl+S":
          // runFileOp surfaces any failure in the error modal and then
          // re-throws; swallow the rejection here so a failing Ctrl+S never
          // becomes an unhandled promise rejection (#489).
          runFileOp(bridge, { kind: "file/save", params: {} }).catch((err) =>
            console.warn("[accel] Ctrl+S save failed:", err),
          );
          break;
        // ── Edit ──
        case "Ctrl+Del":
          void fireAndReport(bridge, { kind: "engine/action/clear", params: {} }, "Clear particles");
          break;
        case "Ctrl+Z":
          void fireAndReport(bridge, { kind: "undo/perform", params: { direction: "undo" } }, "Undo");
          break;
        case "Ctrl+Y":
        case "Ctrl+Shift+Z":
          void fireAndReport(bridge, { kind: "undo/perform", params: { direction: "redo" } }, "Redo");
          break;
        // ── Emitters ──
        case "Alt+Up":
        case "Alt+Down": {
          // Move the whole selection as a block; the highlight follows.
          void moveEmitters(
            bridge,
            useEmitterSelectionStore.getState().ids,
            combo === "Alt+Up" ? "up" : "down",
          );
          break;
        }
        case "Ctrl+Space":
          void fireAndReport(bridge, { kind: "spawner/trigger", params: {} }, "Spawn");
          break;
        // ── View (toggles read live state) ──
        case "Ctrl+G":
          void fireAndReport(bridge, {
            kind: "engine/set/ground",
            params: { enabled: !(st?.ground ?? false) },
          }, "Toggle ground");
          break;
        case "Ctrl+H":
          void fireAndReport(bridge, {
            kind: "engine/set/heat-debug",
            params: { enabled: !(st?.heatDebug ?? false) },
          }, "Toggle heat debug");
          break;
        case "Ctrl+L":
          // Toggle the reference-object lock — only meaningful when one is loaded.
          if ((st?.referenceObjectName ?? "") !== "") {
            void fireAndReport(bridge, {
              kind: "engine/set/reference-object-lock",
              params: { locked: !(st?.referenceObjectLocked ?? false) },
            }, "Toggle reference lock");
          }
          break;
        case "F8":
          void fireAndReport(bridge, {
            kind: "engine/set/paused",
            params: { paused: !(st?.paused ?? false) },
          }, st?.paused ? "Play" : "Pause");
          break;
        case "F9":
          void fireAndReport(bridge, { kind: "engine/action/step-frames", params: { frames: 1 } }, "Step");
          break;
        case "F10":
          void fireAndReport(bridge, { kind: "engine/action/step-frames", params: { frames: 10 } }, "Step 10");
          break;
        case "F7":
          toggleDock("spawner");
          break;
        case "Ctrl+Home":
          void fireAndReport(bridge, { kind: "engine/set/camera", params: RESET_CAMERA }, "Reset camera");
          break;
        case "F5":
          void fireAndReport(bridge, { kind: "engine/action/reload-textures", params: {} }, "Reload textures")
            .then((r) => { if (r !== undefined) bumpTextureEpoch(); }); // re-fetch atlas previews with fresh content
          break;
        case "F6":
          void fireAndReport(bridge, { kind: "engine/action/reload-shaders", params: {} }, "Reload shaders");
          break;
      }
    });
    return off;
  }, [bridge]);
}
