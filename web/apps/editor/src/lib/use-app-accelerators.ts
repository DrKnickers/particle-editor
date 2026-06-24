// use-app-accelerators.ts — wires the legacy global keyboard accelerators
// (ParticleEditor.en.rc:508-530) to the new UI's existing actions (,
//,).
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
// Flagged GAPS (no underlying action yet, so not wired — see
// tasks/fix-plan.md): none. Every legacy accelerator target below resolves
// to an existing bridge command or UI action.

import { useEffect, useRef } from "react";
import type { Bridge, EngineStateDto } from "@particle-editor/bridge-schema";
import { promptSaveChanges } from "@/lib/file-state";
import { runFileOp } from "@/lib/file-op";
import { toggleDock } from "@/lib/right-dock";
import { useEmitterSelectionStore } from "@/lib/emitter-selection";
import { moveEmitters } from "@/lib/emitter-reorder";
import { RESET_CAMERA } from "@/lib/reset-camera";
import { bumpTextureEpoch } from "@/lib/atlas-preview-cache";

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
    bridge
      .request({ kind: "engine/state/snapshot", params: {} })
      .then((s) => {
        if (!cancelled) stateRef.current = s;
      })
      .catch(() => {});
    const off = bridge.on("engine/state/changed", (e) => {
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
          promptSaveChanges(async () => {
            await bridge.request({ kind: "file/new", params: {} });
          });
          break;
        case "Ctrl+O":
          promptSaveChanges(async () => {
            await runFileOp(bridge, { kind: "file/open", params: {} });
          });
          break;
        case "Ctrl+S":
          void runFileOp(bridge, { kind: "file/save", params: {} });
          break;
        // ── Edit ──
        case "Ctrl+Del":
          void bridge.request({ kind: "engine/action/clear", params: {} });
          break;
        case "Ctrl+Z":
          void bridge.request({ kind: "undo/perform", params: { direction: "undo" } });
          break;
        case "Ctrl+Y":
        case "Ctrl+Shift+Z":
          void bridge.request({ kind: "undo/perform", params: { direction: "redo" } });
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
          void bridge.request({ kind: "spawner/trigger", params: {} });
          break;
        // ── View (toggles read live state) ──
        case "Ctrl+G":
          void bridge.request({
            kind: "engine/set/ground",
            params: { enabled: !(st?.ground ?? false) },
          });
          break;
        case "Ctrl+H":
          void bridge.request({
            kind: "engine/set/heat-debug",
            params: { enabled: !(st?.heatDebug ?? false) },
          });
          break;
        case "Ctrl+L":
          // Toggle the reference-object lock — only meaningful when one is loaded.
          if ((st?.referenceObjectName ?? "") !== "") {
            void bridge.request({
              kind: "engine/set/reference-object-lock",
              params: { locked: !(st?.referenceObjectLocked ?? false) },
            });
          }
          break;
        case "F8":
          void bridge.request({
            kind: "engine/set/paused",
            params: { paused: !(st?.paused ?? false) },
          });
          break;
        case "F9":
          void bridge.request({ kind: "engine/action/step-frames", params: { frames: 1 } });
          break;
        case "F10":
          void bridge.request({ kind: "engine/action/step-frames", params: { frames: 10 } });
          break;
        case "F7":
          toggleDock("spawner");
          break;
        case "Ctrl+Home":
          void bridge.request({ kind: "engine/set/camera", params: RESET_CAMERA });
          break;
        case "F5":
          void bridge
            .request({ kind: "engine/action/reload-textures", params: {} })
            .then(() => bumpTextureEpoch()); // re-fetch atlas previews with fresh content
          break;
        case "F6":
          void bridge.request({ kind: "engine/action/reload-shaders", params: {} });
          break;
      }
    });
    return off;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [bridge]);
}
