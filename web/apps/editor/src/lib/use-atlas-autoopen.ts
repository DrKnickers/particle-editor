// use-atlas-autoopen.ts — the React controller for the atlas picker's
// auto-open / restore behaviour (spec §3.2). It subscribes to the atlas
// context (focus / selection / emitter) and the right-dock store, turns
// those into events for the pure `autoOpenReducer`, and executes the
// reducer's commands by writing the right-dock store.
//
// TIMING SUBTLETY (why a naive `selfDock=true; setDock(); selfDock=false`
// flag fails): `setDock(...)` mutates the zustand store synchronously, but
// `useRightDock()` only re-renders the hook on the NEXT React tick. A
// synchronous flag is already reset by the time the `[dock]` effect observes
// the change, so the controller's OWN write would be mis-read as a user
// mutation and the cancel rule would tear the auto-open down instantly. The
// fix below compares the observed dock against a PERSISTED `selfTarget` ref
// (the value we last wrote) which survives the next-render subscription read.
import { useCallback, useEffect, useRef } from "react";
import {
  autoOpenReducer,
  initialAutoOpen,
  type AutoOpenState,
  type AutoOpenEvent,
} from "./atlas-autoopen";
import { useAtlasContext } from "./atlas-context";
import { setDock, useRightDock, type RightDock } from "./right-dock";

export function useAtlasAutoOpen({ atlasEligible }: { atlasEligible: boolean }) {
  const focusedTrack = useAtlasContext((c) => c.focusedTrack);
  const keyTimes = useAtlasContext((c) => c.selection.keyTimes);
  const emitterId = useAtlasContext((c) => c.emitterId);
  const dock = useRightDock();

  const stateRef = useRef<AutoOpenState>(initialAutoOpen);
  const prevFocus = useRef<string | null>(null);
  const hadSelection = useRef(false);
  const mounted = useRef(false);
  const selfTarget = useRef<RightDock | undefined>(undefined); // the dock value WE last wrote

  // Stable identity: dispatch only reads refs + the module-stable `setDock`,
  // so empty deps is correct and adding it to effect dep arrays does NOT
  // change how often those effects run (satisfies exhaustive-deps).
  const dispatch = useCallback((ev: AutoOpenEvent) => {
    const { state, command } = autoOpenReducer(stateRef.current, ev);
    stateRef.current = state;
    if (command.type === "open") { selfTarget.current = "atlas"; setDock("atlas"); }
    else if (command.type === "restore") { selfTarget.current = command.to; setDock(command.to); }
  }, []);

  // Focus transitions; reset the selection edge on each fresh index entry.
  useEffect(() => {
    if (focusedTrack === "index" && prevFocus.current !== "index") { hadSelection.current = false; dispatch({ type: "focusIndex" }); }
    else if (focusedTrack !== "index" && prevFocus.current === "index") dispatch({ type: "focusOther" });
    prevFocus.current = focusedTrack;
  }, [focusedTrack, dispatch]);

  // First key selection (rising edge while on index).
  useEffect(() => {
    const has = keyTimes.length > 0;
    if (focusedTrack === "index" && has && !hadSelection.current)
      dispatch({ type: "keySelected", eligible: atlasEligible, currentDock: dock });
    hadSelection.current = has;
  }, [keyTimes, focusedTrack, atlasEligible, dock, dispatch]);

  useEffect(() => { if (!atlasEligible) dispatch({ type: "eligibilityLost" }); }, [atlasEligible, dispatch]);
  useEffect(() => { if (emitterId === null) dispatch({ type: "emitterCleared" }); }, [emitterId, dispatch]);

  // User dock mutation = a dock change we did NOT cause. Compare to the persisted
  // target (survives the next-render subscription read; a synchronous flag would not).
  useEffect(() => {
    if (!mounted.current) { mounted.current = true; selfTarget.current = dock; return; }
    if (dock === selfTarget.current) { selfTarget.current = undefined; return; } // our own write
    dispatch({ type: "userDockMutation" });
  }, [dock, dispatch]);
}
