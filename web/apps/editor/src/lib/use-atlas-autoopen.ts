// use-atlas-autoopen.ts — the React controller for the atlas picker's
// auto-open / restore behaviour. It observes the atlas context (focus /
// selection / emitter) and the right-dock store, turns those into events for
// the pure `autoOpenReducer`, and executes the reducer's commands by writing
// the right-dock store.
//
// NON-REACTIVE CONTEXT READ (why this hook does NOT use `useAtlasContext`):
// this hook is mounted by CurveEditorPanel, which ALSO publishes to the atlas
// context from its own effect. A reactive `useAtlasContext` selector here would
// re-render CurveEditorPanel every time it published — a self-feedback loop
// (~4 commits per emitter selection; #549 audit follow-up). So we observe the
// context NON-reactively: replay the current value once at mount, then
// `subscribeAtlasContext` for changes. The subscription does not re-render.
//
// Because the subscription effect runs ONCE (deps [dispatch]), a plain closure
// would freeze `atlasEligible` / `dock`; they are mirrored into refs synced in
// a LAYOUT effect. Layout effects flush BEFORE passive effects within a commit,
// and CurveEditorPanel's publish runs from a PASSIVE effect, so the refs are
// already current when a publish synchronously notifies the subscription. (A
// render-body `ref.current =` write would be wrong: a discarded/replayed render
// under StrictMode/concurrent mode still mutates the shared ref.)
//
// TIMING SUBTLETY (why a naive `selfDock=true; setDock(); selfDock=false`
// flag fails): `setDock(...)` mutates the zustand store synchronously, but
// `useRightDock()` only re-renders the hook on the NEXT React tick. A
// synchronous flag is already reset by the time the `[dock]` effect observes
// the change, so the controller's OWN write would be mis-read as a user
// mutation and the cancel rule would tear the auto-open down instantly. The
// fix below compares the observed dock against a PERSISTED `selfTarget` ref
// (the value we last wrote) which survives the next-render subscription read.
import { useCallback, useEffect, useLayoutEffect, useRef } from "react";
import {
  autoOpenReducer,
  initialAutoOpen,
  type AutoOpenState,
  type AutoOpenEvent,
} from "./atlas-autoopen";
import {
  getAtlasContext,
  subscribeAtlasContext,
  type AtlasContext,
} from "./atlas-context";
import { setDock, useRightDock, type RightDock } from "./right-dock";

export function useAtlasAutoOpen({ atlasEligible }: { atlasEligible: boolean }) {
  const dock = useRightDock();

  const stateRef = useRef<AutoOpenState>(initialAutoOpen);
  const prevFocus = useRef<string | null>(null);
  const hadSelection = useRef(false);
  const mounted = useRef(false);
  const selfTarget = useRef<RightDock | undefined>(undefined); // the dock value WE last wrote

  // Latest-value refs for the non-reactive context subscription (see header):
  // synced in a LAYOUT effect so they are current before CurveEditorPanel's
  // passive publish effect fires, and never mutate on a discarded render.
  const atlasEligibleRef = useRef(atlasEligible);
  const dockRef = useRef(dock);
  useLayoutEffect(() => {
    atlasEligibleRef.current = atlasEligible;
    dockRef.current = dock;
  }, [atlasEligible, dock]);

  // Stable identity: dispatch only reads refs + the module-stable `setDock`,
  // so empty deps is correct and adding it to effect dep arrays does NOT
  // change how often those effects run (satisfies exhaustive-deps).
  const dispatch = useCallback((ev: AutoOpenEvent) => {
    const { state, command } = autoOpenReducer(stateRef.current, ev);
    stateRef.current = state;
    if (command.type === "open") { selfTarget.current = "atlas"; setDock("atlas"); }
    else if (command.type === "restore") { selfTarget.current = command.to; setDock(command.to); }
  }, []);

  // Observe the atlas context NON-REACTIVELY: replay once at mount, then
  // subscribe. Each branch is gated by a per-slice `Object.is` diff so a
  // publish that changes only one slice fires exactly the branches the old
  // per-effect dependency arrays ([focusedTrack] / [keyTimes, focusedTrack] /
  // [emitterId]) would have. `prev === undefined` (the mount replay) runs all
  // three branch bodies once — matching React firing every useEffect at mount;
  // the internal prevFocus / hadSelection ref-guards keep the edge detection
  // identical and make a StrictMode double-mount idempotent.
  useEffect(() => {
    const runFocusTransition = (focusedTrack: AtlasContext["focusedTrack"]) => {
      if (focusedTrack === "index" && prevFocus.current !== "index") {
        hadSelection.current = false;
        dispatch({ type: "focusIndex" });
      } else if (focusedTrack !== "index" && prevFocus.current === "index") {
        dispatch({ type: "focusOther" });
      }
      prevFocus.current = focusedTrack;
    };
    const runSelectionEdge = (ctx: AtlasContext) => {
      const has = ctx.selection.keyTimes.length > 0;
      if (ctx.focusedTrack === "index" && has && !hadSelection.current)
        dispatch({
          type: "keySelected",
          eligible: atlasEligibleRef.current,
          currentDock: dockRef.current,
        });
      hadSelection.current = has;
    };
    const handle = (ctx: AtlasContext, prev: AtlasContext | undefined) => {
      if (prev === undefined || !Object.is(ctx.focusedTrack, prev.focusedTrack))
        runFocusTransition(ctx.focusedTrack);
      if (
        prev === undefined ||
        !Object.is(ctx.focusedTrack, prev.focusedTrack) ||
        !Object.is(ctx.selection.keyTimes, prev.selection.keyTimes)
      )
        runSelectionEdge(ctx);
      if (
        (prev === undefined || !Object.is(ctx.emitterId, prev.emitterId)) &&
        ctx.emitterId === null
      )
        dispatch({ type: "emitterCleared" });
    };
    handle(getAtlasContext(), undefined); // mount replay
    return subscribeAtlasContext(handle);
  }, [dispatch]);

  // Eligibility loss is driven by the `atlasEligible` PROP (not the context
  // store), so it stays a normal reactive effect — it is not part of the loop.
  useEffect(() => { if (!atlasEligible) dispatch({ type: "eligibilityLost" }); }, [atlasEligible, dispatch]);

  // User dock mutation = a dock change we did NOT cause. Compare to the persisted
  // target (survives the next-render subscription read; a synchronous flag would not).
  useEffect(() => {
    if (!mounted.current) {
      mounted.current = true;
      // Establish the baseline ONLY if the mount replay above did not already
      // auto-open/restore. That replay runs synchronously in this same commit
      // (before this effect), so an unconditional `selfTarget = dock` here would
      // clobber the "atlas" it just wrote with the stale pre-open dock — and the
      // ensuing setDock rerender would then mis-read it as a user mutation and
      // cancel the just-opened picker. `=== undefined` (not `??`) because `null`
      // is a valid RightDock we must preserve.
      if (selfTarget.current === undefined) selfTarget.current = dock;
      return;
    }
    if (dock === selfTarget.current) { selfTarget.current = undefined; return; } // our own write
    dispatch({ type: "userDockMutation" });
  }, [dock, dispatch]);
}
