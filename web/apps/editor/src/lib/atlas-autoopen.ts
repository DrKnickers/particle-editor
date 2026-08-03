// Pure reducer for the picker's auto-open/persist/restore.
// The hook (use-atlas-autoopen.ts) supplies events and executes commands;
// this module holds NO React/store deps so it is exhaustively unit-tested.
import type { RightDock } from "./right-dock";

export interface AutoOpenState {
  armed: boolean;          // a fresh index-focus entry re-armed one auto-open
  active: boolean;         // the picker is AUTO-occupying the dock right now
  remembered: RightDock;   // the panel the auto-open displaced (null allowed)
}

export const initialAutoOpen: AutoOpenState = { armed: false, active: false, remembered: null };

export type AutoOpenEvent =
  | { type: "focusIndex" }
  | { type: "focusOther" }
  | { type: "keySelected"; eligible: boolean; currentDock: RightDock }
  | { type: "userDockMutation" }
  | { type: "emitterCleared" }
  | { type: "eligibilityLost" };

export type AutoOpenCommand =
  | { type: "none" }
  | { type: "open"; remembered: RightDock }
  | { type: "restore"; to: RightDock };

export function autoOpenReducer(
  s: AutoOpenState,
  e: AutoOpenEvent,
): { state: AutoOpenState; command: AutoOpenCommand } {
  switch (e.type) {
    case "focusIndex":
      return { state: { ...s, armed: true }, command: { type: "none" } };

    case "keySelected":
      if (s.armed && e.eligible && !s.active && e.currentDock !== "atlas") {
        return {
          state: { armed: false, active: true, remembered: e.currentDock },
          command: { type: "open", remembered: e.currentDock },
        };
      }
      return { state: s, command: { type: "none" } }; // keep armed for a later eligible selection

    case "focusOther":
      if (s.active) {
        return {
          state: { armed: false, active: false, remembered: null },
          command: { type: "restore", to: s.remembered },
        };
      }
      return { state: { ...s, armed: false }, command: { type: "none" } };

    case "emitterCleared":
    case "eligibilityLost":
      if (s.active) {
        return {
          state: { armed: false, active: false, remembered: null },
          command: { type: "restore", to: s.remembered },
        };
      }
      return { state: s, command: { type: "none" } };

    case "userDockMutation":
      // Cancel rule: once the user touches the dock, stop auto-driving entirely.
      return { state: { armed: false, active: false, remembered: null }, command: { type: "none" } };
  }
}
